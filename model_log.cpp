/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#include "model_log.h"
#include "logger.h"
#include "file_utils.h"
#include "config_mgr.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

using namespace rapidjson;

namespace DMContext {

// 静态成员初始化
std::map<std::string, std::map<std::string, std::map<std::string, QueryApiLog*>>> ModelLogUtil::s_queryApiCache;
std::map<std::string, std::map<std::string, std::map<std::string, WriteApiLog*>>> ModelLogUtil::s_writeApiCache;
std::map<std::string, std::map<std::string, DeleteApiLog*>> ModelLogUtil::s_deleteApiCache;
std::mutex ModelLogUtil::s_cacheMutex;

// ==================== 日志老化（rotation）相关 ====================
namespace {
// 日志根路径写死，与 file_utils.cpp 的 WriteModelLogNoTime 保持一致。
// dmcontextservice_config.json:log.path 字段当前预留，未启用。
static const std::string LOG_ROOT = "/opt/coremind/logs/ModelLogs/";
constexpr size_t GZIP_CHUNK = 64 * 1024;
constexpr size_t DEFAULT_MAX_FILE_SIZE = 20ULL * 1024 * 1024;
constexpr int DEFAULT_MAX_ARCHIVE_COUNT = 10;

// 解析 "20K"/"20M"/"20G"（不接受纯数字）。
bool ParseSize(const std::string& sizeStr, size_t& outBytes)
{
    if (sizeStr.empty()) {
        return false;
    }
    char unit = sizeStr.back();
    size_t multiplier = 0;
    switch (unit) {
        case 'K': case 'k': multiplier = 1024ULL; break;
        case 'M': case 'm': multiplier = 1024ULL * 1024; break;
        case 'G': case 'g': multiplier = 1024ULL * 1024 * 1024; break;
        default:
            LOG_ERR("ParseSize: '%s' missing unit (K/M/G)", sizeStr.c_str());
            return false;
    }
    try {
        long long n = std::stoll(sizeStr.substr(0, sizeStr.size() - 1));
        if (n <= 0) {
            LOG_ERR("ParseSize: '%s' non-positive", sizeStr.c_str());
            return false;
        }
        outBytes = static_cast<size_t>(n) * multiplier;
        return true;
    } catch (...) {
        LOG_ERR("ParseSize: '%s' parse failed", sizeStr.c_str());
        return false;
    }
}

// 每次调用都从 ConfigMgr 重新读取，支持 PUT /api/v1/contexts/configs/item?type=log 热生效。
// 仅当原始字符串与上次不同才重新打 LOG，避免高频 flush 时刷屏。
size_t GetMaxFileSize()
{
    static std::mutex mtx;
    static std::string lastRaw;
    static size_t lastParsed = DEFAULT_MAX_FILE_SIZE;
    static bool initialized = false;

    auto m = ConfigMgr::GetInstance()->GetLogMap();
    std::string raw;
    auto it = m.find("size");
    if (it != m.end()) {
        raw = it->second;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (initialized && raw == lastRaw) {
        return lastParsed;
    }

    size_t parsed = 0;
    if (raw.empty()) {
        LOG_WARNING("Log rotation: log.size missing, fallback to default %zu bytes",
                    DEFAULT_MAX_FILE_SIZE);
        lastParsed = DEFAULT_MAX_FILE_SIZE;
    } else if (ParseSize(raw, parsed)) {
        LOG_INFO("Log rotation: maxFileSize -> %zu bytes (raw='%s')", parsed, raw.c_str());
        lastParsed = parsed;
    } else {
        LOG_WARNING("Log rotation: log.size='%s' invalid, fallback to default %zu bytes",
                    raw.c_str(), DEFAULT_MAX_FILE_SIZE);
        lastParsed = DEFAULT_MAX_FILE_SIZE;
    }
    lastRaw = raw;
    initialized = true;
    return lastParsed;
}

int GetMaxArchiveCount()
{
    static std::mutex mtx;
    static std::string lastRaw;
    static int lastParsed = DEFAULT_MAX_ARCHIVE_COUNT;
    static bool initialized = false;

    auto m = ConfigMgr::GetInstance()->GetLogMap();
    std::string raw;
    auto it = m.find("num");
    if (it != m.end()) {
        raw = it->second;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (initialized && raw == lastRaw) {
        return lastParsed;
    }

    int parsed = 0;
    bool ok = false;
    if (!raw.empty()) {
        try {
            parsed = std::stoi(raw);
            ok = parsed > 0;
        } catch (...) {
            ok = false;
        }
    }
    if (ok) {
        LOG_INFO("Log rotation: maxArchiveCount -> %d (raw='%s')", parsed, raw.c_str());
        lastParsed = parsed;
    } else {
        LOG_WARNING("Log rotation: log.num='%s' invalid or missing, fallback to default %d",
                    raw.c_str(), DEFAULT_MAX_ARCHIVE_COUNT);
        lastParsed = DEFAULT_MAX_ARCHIVE_COUNT;
    }
    lastRaw = raw;
    initialized = true;
    return lastParsed;
}

// 全局：(dir|fileName) -> 互斥锁；用于把 stat / 写文件 / rotate 串成一把锁内的原子动作。
std::mutex g_fileLockMapMutex;
std::map<std::string, std::shared_ptr<std::mutex>> g_fileLockMap;

std::shared_ptr<std::mutex> GetFileLock(const std::string& dir, const std::string& fileName)
{
    std::lock_guard<std::mutex> lk(g_fileLockMapMutex);
    std::string key = dir + "|" + fileName;
    auto it = g_fileLockMap.find(key);
    if (it != g_fileLockMap.end()) {
        return it->second;
    }
    auto m = std::make_shared<std::mutex>();
    g_fileLockMap[key] = m;
    return m;
}

bool GzipFile(const std::string& src, const std::string& dst)
{
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) {
        LOG_ERR("GzipFile: open src %s failed", src.c_str());
        return false;
    }
    gzFile gz = gzopen(dst.c_str(), "wb");
    if (gz == nullptr) {
        LOG_ERR("GzipFile: gzopen dst %s failed", dst.c_str());
        return false;
    }
    std::vector<char> buf(GZIP_CHUNK);
    while (true) {
        in.read(buf.data(), buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        int written = gzwrite(gz, buf.data(), static_cast<unsigned>(got));
        if (written <= 0) {
            LOG_ERR("GzipFile: gzwrite failed, src=%s", src.c_str());
            gzclose(gz);
            return false;
        }
    }
    gzclose(gz);
    return true;
}

// 从 OM_<contextId>_<ms>.gz 中解析 ms；不匹配则返回 0。
int64_t ExtractTimestampFromArchiveName(const std::string& contextId, const std::string& fname)
{
    std::string prefix = "OM_" + contextId + "_";
    static const std::string suffix = ".gz";
    if (fname.size() <= prefix.size() + suffix.size()) {
        return 0;
    }
    if (fname.compare(0, prefix.size(), prefix) != 0) {
        return 0;
    }
    if (fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return 0;
    }
    std::string tsStr = fname.substr(prefix.size(), fname.size() - prefix.size() - suffix.size());
    try {
        return std::stoll(tsStr);
    } catch (...) {
        return 0;
    }
}

// 扫描目录下属于该 contextId 的归档，超过 maxKeep 则按时间戳从老到新删除。
void TrimArchives(const std::string& dirPath, const std::string& contextId, int maxKeep)
{
    std::vector<std::pair<int64_t, std::string>> archives;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dirPath, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            LOG_ERR("TrimArchives: iterate %s failed: %s", dirPath.c_str(), ec.message().c_str());
            return;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        std::string fname = it->path().filename().string();
        int64_t ts = ExtractTimestampFromArchiveName(contextId, fname);
        if (ts > 0) {
            archives.emplace_back(ts, it->path().string());
        }
    }
    if (static_cast<int>(archives.size()) <= maxKeep) {
        return;
    }
    std::sort(archives.begin(), archives.end()); // 升序，最老的在前
    int toDelete = static_cast<int>(archives.size()) - maxKeep;
    for (int i = 0; i < toDelete; ++i) {
        std::error_code rmEc;
        std::filesystem::remove(archives[i].second, rmEc);
        if (rmEc) {
            LOG_ERR("TrimArchives: remove %s failed: %s",
                    archives[i].second.c_str(), rmEc.message().c_str());
        } else {
            LOG_INFO("TrimArchives: removed old archive %s", archives[i].second.c_str());
        }
    }
}

// fileName 必须形如 "OM_<contextId>"。在已持有 (dir,fileName) 锁的前提下调用。
void RotateIfExceeded(const std::string& dir, const std::string& fileName)
{
    if (fileName.size() <= 3 || fileName.compare(0, 3, "OM_") != 0) {
        return;
    }
    std::string contextId = fileName.substr(3);
    std::string dirPath = LOG_ROOT + dir;
    std::string filePath = dirPath + "/" + fileName;

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return;
    }
    auto sz = std::filesystem::file_size(filePath, ec);
    if (ec) {
        LOG_ERR("RotateIfExceeded: file_size %s failed: %s",
                filePath.c_str(), ec.message().c_str());
        return;
    }
    size_t threshold = GetMaxFileSize();
    if (sz < threshold) {
        return;
    }

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    std::string archivePath = dirPath + "/OM_" + contextId + "_" + std::to_string(nowMs) + ".gz";
    std::string tmpArchivePath = archivePath + ".tmp";

    LOG_INFO("RotateIfExceeded: %s size=%zu >= threshold=%zu, archive to %s",
             filePath.c_str(), sz, threshold, archivePath.c_str());

    // 1. 先压到 .tmp，避免半成品被识别为正式归档
    if (!GzipFile(filePath, tmpArchivePath)) {
        std::filesystem::remove(tmpArchivePath, ec);
        return;
    }
    // 2. .tmp → 最终归档
    std::filesystem::rename(tmpArchivePath, archivePath, ec);
    if (ec) {
        LOG_ERR("RotateIfExceeded: rename %s -> %s failed: %s",
                tmpArchivePath.c_str(), archivePath.c_str(), ec.message().c_str());
        std::filesystem::remove(tmpArchivePath, ec);
        return;
    }
    // 3. 删原日志
    std::filesystem::remove(filePath, ec);
    if (ec) {
        LOG_ERR("RotateIfExceeded: remove original %s failed: %s",
                filePath.c_str(), ec.message().c_str());
    }
    // 4. 裁剪到 maxArchiveCount 份（只针对该 contextId）
    TrimArchives(dirPath, contextId, GetMaxArchiveCount());
}

void WriteAndRotate(const std::string& dir, const std::string& fileName,
                    const std::string& jsonStr)
{
    auto fileLock = GetFileLock(dir, fileName);
    std::lock_guard<std::mutex> lk(*fileLock);
    FileUtils::WriteModelLogNoTime(dir, fileName, jsonStr);
    RotateIfExceeded(dir, fileName);
}
}  // namespace

// ==================== QueryApi缓存相关实现 ====================

std::string ModelLogUtil::GetQueryApiCacheKey(const std::string& dir, const std::string& fileName,
    const std::string& timestamp)
{
    return dir + "|" + fileName + "|" + timestamp;
}

bool ModelLogUtil::IsQueryApiLogComplete(const QueryApiLog& logData)
{
    // 检查QueryApiLog中所有成员是否都已填充
    // requestBody中的必填字段
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.content.empty() || logData.requestBody.contextId.empty()) {
        return false;
    }
    // ts字段（时间戳）
    if (logData.ts.empty()) {
        return false;
    }
    return true;
}

void ModelLogUtil::FlushQueryApiToFile(const std::string& dir, const std::string& fileName,
    const QueryApiLog& logData)
{
    std::string jsonStr = QueryApiLogToJson(logData);
    if (!jsonStr.empty()) {
        WriteAndRotate(dir, fileName, jsonStr);
        LOG_INFO("FlushQueryApiToFile success, dir: %s, fileName: %s", dir.c_str(), fileName.c_str());
    }
}

bool ModelLogUtil::GetQueryApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp, QueryApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_queryApiCache.find(dir);
    if (dirIt == s_queryApiCache.end()) {
        LOG_WARNING("GetQueryApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("GetQueryApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    auto timestampIt = fileNameIt->second.find(timestamp);
    if (timestampIt == fileNameIt->second.end()) {
        LOG_WARNING("GetQueryApiCache: timestamp not found, timestamp: %s", timestamp.c_str());
        return false;
    }
    logData = timestampIt->second;
    return true;
}

bool ModelLogUtil::GetOrCreateQueryApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp, QueryApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    // 如果缓存中不存在，则创建新实例
    QueryApiLog* cachedLog = s_queryApiCache[dir][fileName][timestamp];
    if (cachedLog == nullptr) {
        cachedLog = new(QueryApiLog){};
        s_queryApiCache[dir][fileName][timestamp] = cachedLog;
        LOG_INFO("GetOrCreateQueryApiCache: created new cache, dir: %s, fileName: %s, timestamp: %s",
                 dir.c_str(), fileName.c_str(), timestamp.c_str());
    } else {
        LOG_INFO("GetOrCreateQueryApiCache: cache has exist, dir: %s, fileName: %s, timestamp: %s",
                 dir.c_str(), fileName.c_str(), timestamp.c_str());
    }
    logData = cachedLog;
    return true;
}

bool ModelLogUtil::RemoveQueryApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_queryApiCache.find(dir);
    if (dirIt == s_queryApiCache.end()) {
        LOG_WARNING("RemoveQueryApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("RemoveQueryApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    auto timestampIt = fileNameIt->second.find(timestamp);
    if (timestampIt == fileNameIt->second.end()) {
        LOG_WARNING("RemoveQueryApiCache: timestamp not found, timestamp: %s", timestamp.c_str());
        return false;
    }
    fileNameIt->second.erase(timestampIt);
    if (timestampIt->second != nullptr) {
        delete timestampIt->second;
    }
    LOG_INFO("RemoveQueryApiCache success, dir: %s, fileName: %s, timestamp: %s",
        dir.c_str(), fileName.c_str(), timestamp.c_str());
    return true;
}

// ==================== WriteApi缓存相关实现 ====================

std::string ModelLogUtil::GetWriteApiCacheKey(const std::string& dir, const std::string& fileName,
    const std::string& timestamp)
{
    return dir + "|" + fileName + "|" + timestamp;
}

bool ModelLogUtil::IsWriteApiLogComplete(const WriteApiLog& logData)
{
    // 检查WriteApiLog中所有成员是否都已填充
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.contextId.empty()) {
        return false;
    }
    if (logData.ts.empty()) {
        return false;
    }
    return true;
}

void ModelLogUtil::FlushWriteApiToFile(const std::string& dir, const std::string& fileName,
    const WriteApiLog& logData)
{
    std::string jsonStr = WriteApiLogToJson(logData);
    if (!jsonStr.empty()) {
        WriteAndRotate(dir, fileName, jsonStr);
        LOG_INFO("FlushWriteApiToFile success, dir: %s, fileName: %s", dir.c_str(), fileName.c_str());
    }
}

bool ModelLogUtil::GetWriteApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp, WriteApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_writeApiCache.find(dir);
    if (dirIt == s_writeApiCache.end()) {
        LOG_WARNING("GetWriteApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("GetWriteApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    auto timestampIt = fileNameIt->second.find(timestamp);
    if (timestampIt == fileNameIt->second.end()) {
        LOG_WARNING("GetWriteApiCache: timestamp not found, timestamp: %s", timestamp.c_str());
        return false;
    }
    logData = timestampIt->second;
    return true;
}

bool ModelLogUtil::GetOrCreateWriteApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp, WriteApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    // 如果缓存中不存在，则创建新实例
    WriteApiLog* cachedLog = s_writeApiCache[dir][fileName][timestamp];
    if (cachedLog == nullptr) {
        cachedLog = new(WriteApiLog){};
        s_writeApiCache[dir][fileName][timestamp] = cachedLog;
        LOG_INFO("GetOrCreateWriteApiCache: created new cache, dir: %s, fileName: %s, timestamp: %s",
                 dir.c_str(), fileName.c_str(), timestamp.c_str());
    } else {
        LOG_INFO("GetOrCreateWriteApiCache: cache has exist, dir: %s, fileName: %s, timestamp: %s",
                 dir.c_str(), fileName.c_str(), timestamp.c_str());
    }
    logData = cachedLog;
    return true;
}

bool ModelLogUtil::RemoveWriteApiCache(const std::string& dir, const std::string& fileName,
    const std::string& timestamp)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_writeApiCache.find(dir);
    if (dirIt == s_writeApiCache.end()) {
        LOG_WARNING("RemoveWriteApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("RemoveWriteApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    auto timestampIt = fileNameIt->second.find(timestamp);
    if (timestampIt == fileNameIt->second.end()) {
        LOG_WARNING("RemoveWriteApiCache: timestamp not found, timestamp: %s", timestamp.c_str());
        return false;
    }
    fileNameIt->second.erase(timestampIt);
    if (timestampIt->second != nullptr) {
        delete timestampIt->second;
    }
    LOG_INFO("RemoveWriteApiCache success, dir: %s, fileName: %s, timestamp: %s",
        dir.c_str(), fileName.c_str(), timestamp.c_str());
    return true;
}

// ==================== DeleteApi缓存相关实现 ====================

std::string ModelLogUtil::GetDeleteApiCacheKey(const std::string& dir, const std::string& fileName)
{
    return dir + "|" + fileName;
}

bool ModelLogUtil::IsDeleteApiLogComplete(const DeleteApiLog& logData)
{
    // 检查DeleteApiLog中所有成员是否都已填充
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.contextId.empty()) {
        return false;
    }
    return true;
}

void ModelLogUtil::FlushDeleteApiToFile(const std::string& dir, const std::string& fileName,
    const DeleteApiLog& logData)
{
    std::string jsonStr = DeleteApiLogToJson(logData);
    if (!jsonStr.empty()) {
        WriteAndRotate(dir, fileName, jsonStr);
        LOG_INFO("FlushDeleteApiToFile success, dir: %s, fileName: %s", dir.c_str(), fileName.c_str());
    }
}

bool ModelLogUtil::GetDeleteApiCache(const std::string& dir, const std::string& fileName,
    DeleteApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_deleteApiCache.find(dir);
    if (dirIt == s_deleteApiCache.end()) {
        LOG_WARNING("GetDeleteApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("GetDeleteApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    logData = fileNameIt->second;
    return true;
}

bool ModelLogUtil::GetOrCreateDeleteApiCache(const std::string& dir, const std::string& fileName,
    DeleteApiLog*& logData)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    // 如果缓存中不存在，则创建新实例
    DeleteApiLog* cachedLog = s_deleteApiCache[dir][fileName];
    if (cachedLog == nullptr) {
        cachedLog = new(DeleteApiLog){};
        s_deleteApiCache[dir][fileName] = cachedLog;
        LOG_INFO("GetOrCreateWriteApiCache: created new cache, dir: %s, fileName: %s",
                 dir.c_str(), fileName.c_str());
    } else {
        LOG_INFO("GetOrCreateWriteApiCache: cache has exist, dir: %s, fileName: %s",
                 dir.c_str(), fileName.c_str());
    }
    logData = cachedLog;
    LOG_INFO("GetOrCreateDeleteApiCache: created new cache, dir: %s, fileName: %s",
        dir.c_str(), fileName.c_str());
    return true;
}

bool ModelLogUtil::RemoveDeleteApiCache(const std::string& dir, const std::string& fileName)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto dirIt = s_deleteApiCache.find(dir);
    if (dirIt == s_deleteApiCache.end()) {
        LOG_WARNING("RemoveDeleteApiCache: dir not found, dir: %s", dir.c_str());
        return false;
    }
    auto fileNameIt = dirIt->second.find(fileName);
    if (fileNameIt == dirIt->second.end()) {
        LOG_WARNING("RemoveDeleteApiCache: fileName not found, fileName: %s", fileName.c_str());
        return false;
    }
    dirIt->second.erase(fileNameIt);
    if (fileNameIt->second != nullptr) {
        delete fileNameIt->second;
    }
    LOG_INFO("RemoveDeleteApiCache success, dir: %s, fileName: %s", dir.c_str(), fileName.c_str());
    return true;
}

// ==================== 合并字段函数实现 ====================

// 遍历source中的所有非空成员，合并到target中
void ModelLogUtil::MergeQueryApiLog(QueryApiLog& target, const QueryApiLog& source)
{
    // requestBody
    if (!source.requestBody.userId.empty()) {
        target.requestBody.userId = source.requestBody.userId;
    }
    if (!source.requestBody.serviceId.empty()) {
        target.requestBody.serviceId = source.requestBody.serviceId;
    }
    if (!source.requestBody.content.empty()) {
        target.requestBody.content = source.requestBody.content;
    }
    if (!source.requestBody.contextId.empty()) {
        target.requestBody.contextId = source.requestBody.contextId;
    }
    if (!source.requestBody.responseId.empty()) {
        target.requestBody.responseId = source.requestBody.responseId;
    }

    // historyData.abstractQA - 如果有数据则覆盖
    if (!source.historyData.abstractQA.empty()) {
        target.historyData.abstractQA = source.historyData.abstractQA;
    }
    // historyData.qaList
    if (!source.historyData.qaList.empty()) {
        target.historyData.qaList = source.historyData.qaList;
    }

    // rewrite
    if (!source.rewrite.modelInput.empty()) {
        target.rewrite.modelInput = source.rewrite.modelInput;
    }
    if (!source.rewrite.modelOutput.empty()) {
        target.rewrite.modelOutput = source.rewrite.modelOutput;
    }
    if (!source.rewrite.delay.empty()) {
        target.rewrite.delay = source.rewrite.delay;
    }

    // memory
    if (!source.memory.query.empty()) {
        target.memory.query = source.memory.query;
    }
    if (!source.memory.memoryResult.empty()) {
        target.memory.memoryResult = source.memory.memoryResult;
    }
    if (!source.memory.delay.empty()) {
        target.memory.delay = source.memory.delay;
    }

    // response - 只覆盖非零值或非空值
    if (source.response.code != 0) {
        target.response.code = source.response.code;
    }
    if (!source.response.msg.empty()) {
        target.response.msg = source.response.msg;
    }
    if (!source.response.memory.empty()) {
        target.response.memory = source.response.memory;
    }
    if (!source.response.history.empty()) {
        target.response.history = source.response.history;
    }
    if (!source.response.responseId.empty()) {
        target.response.responseId = source.response.responseId;
    }
    if (!source.response.rewrittenQuery.empty()) {
        target.response.rewrittenQuery = source.response.rewrittenQuery;
    }
    if (source.response.totalToken != 0) {
        target.response.totalToken = source.response.totalToken;
    }
    if (source.response.historyTokenCount != 0) {
        target.response.historyTokenCount = source.response.historyTokenCount;
    }
    if (source.response.memoryTokenCount != 0) {
        target.response.memoryTokenCount = source.response.memoryTokenCount;
    }

    // ts, delay and turn
    if (!source.ts.empty()) {
        target.ts = source.ts;
    }
    if (!source.delay.empty()) {
        target.delay = source.delay;
    }
    if (!source.turn.empty()) {
        target.turn = source.turn;
    }
    // sessionType
    if (!source.sessionType.empty()) {
        target.sessionType = source.sessionType;
    }
}

void ModelLogUtil::MergeWriteApiLog(WriteApiLog& target, const WriteApiLog& source)
{
    // requestBody
    if (!source.requestBody.userId.empty()) {
        target.requestBody.userId = source.requestBody.userId;
    }
    if (!source.requestBody.serviceId.empty()) {
        target.requestBody.serviceId = source.requestBody.serviceId;
    }
    if (!source.requestBody.messages.empty()) {
        target.requestBody.messages = source.requestBody.messages;
    }
    if (!source.requestBody.contextId.empty()) {
        target.requestBody.contextId = source.requestBody.contextId;
    }
    if (!source.requestBody.biometricIdentity.voiceprintId.empty()) {
        target.requestBody.biometricIdentity.voiceprintId = source.requestBody.biometricIdentity.voiceprintId;
    }
    if (!source.requestBody.biometricIdentity.faceId.empty()) {
        target.requestBody.biometricIdentity.faceId = source.requestBody.biometricIdentity.faceId;
    }
    if (!source.requestBody.agentRole.empty()) {
        target.requestBody.agentRole = source.requestBody.agentRole;
    }

    // response
    if (source.response.code != 0) {
        target.response.code = source.response.code;
    }
    if (!source.response.msg.empty()) {
        target.response.msg = source.response.msg;
    }

    // other fields
    if (!source.qaExtractDelay.empty()) {
        target.qaExtractDelay = source.qaExtractDelay;
    }
    if (!source.qaSummaryDelay.empty()) {
        target.qaSummaryDelay = source.qaSummaryDelay;
    }
    if (!source.ts.empty()) {
        target.ts = source.ts;
    }
    if (!source.delay.empty()) {
        target.delay = source.delay;
    }
    if (!source.turn.empty()) {
        target.turn = source.turn;
    }
    if (!source.uuId.empty()) {
        target.uuId = source.uuId;
    }
    // sessionType
    if (!source.sessionType.empty()) {
        target.sessionType = source.sessionType;
    }
}

void ModelLogUtil::MergeDeleteApiLog(DeleteApiLog& target, const DeleteApiLog& source)
{
    // requestBody
    if (!source.requestBody.userId.empty()) {
        target.requestBody.userId = source.requestBody.userId;
    }
    if (!source.requestBody.serviceId.empty()) {
        target.requestBody.serviceId = source.requestBody.serviceId;
    }
    if (!source.requestBody.contextId.empty()) {
        target.requestBody.contextId = source.requestBody.contextId;
    }

    // response
    if (source.response.code != 0) {
        target.response.code = source.response.code;
    }
    if (!source.response.msg.empty()) {
        target.response.msg = source.response.msg;
    }

    // ts, delay and turn
    if (!source.ts.empty()) {
        target.ts = source.ts;
    }
    if (!source.delay.empty()) {
        target.delay = source.delay;
    }
    if (!source.turn.empty()) {
        target.turn = source.turn;
    }
    // sessionType
    if (!source.sessionType.empty()) {
        target.sessionType = source.sessionType;
    }
}

// ==================== 公共方法实现 ====================

// 检查QueryApi日志是否所有参数都填充完毕（用于入参检查）
bool ModelLogUtil::CheckQueryApiLogComplete(const QueryApiLog& logData)
{
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.content.empty() || logData.requestBody.contextId.empty()) {
        LOG_ERR("QueryApiLog request body not complete, userId: %s, serviceId: %s, content: %s, contextId: %s",
            logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
            logData.requestBody.content.c_str(), logData.requestBody.contextId.c_str());
        return false;
    }
    return true;
}

// 检查WriteApi日志是否所有参数都填充完毕
bool ModelLogUtil::CheckWriteApiLogComplete(const WriteApiLog& logData)
{
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.contextId.empty()) {
        LOG_ERR("WriteApiLog request body not complete, userId: %s, serviceId: %s, contextId: %s",
            logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
            logData.requestBody.contextId.c_str());
        return false;
    }
    return true;
}

// 检查DeleteApi日志是否所有参数都填充完毕
bool ModelLogUtil::CheckDeleteApiLogComplete(const DeleteApiLog& logData)
{
    if (logData.requestBody.userId.empty() || logData.requestBody.serviceId.empty() ||
        logData.requestBody.contextId.empty()) {
        LOG_ERR("DeleteApiLog request body not complete, userId: %s, serviceId: %s, contextId: %s",
            logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
            logData.requestBody.contextId.c_str());
        return false;
    }
    return true;
}

// 将QueryApiLog转换为JSON字符串
std::string ModelLogUtil::QueryApiLogToJson(const QueryApiLog& logData)
{
    StringBuffer s;
    Writer<StringBuffer> writer(s);

    writer.StartObject();
    writer.Key("request_body");
    writer.StartObject();
    writer.Key("userId");
    writer.String(logData.requestBody.userId.c_str());
    writer.Key("service_id");
    writer.String(logData.requestBody.serviceId.c_str());
    writer.Key("content");
    writer.String(logData.requestBody.content.c_str());
    writer.Key("context_id");
    writer.String(logData.requestBody.contextId.c_str());
    writer.Key("response_id");
    writer.String(logData.requestBody.responseId.c_str());
    writer.EndObject();

    writer.Key("history_data");
    writer.StartObject();
    writer.Key("abstractQA");
    writer.StartArray();
    for (const auto& abstractQA : logData.historyData.abstractQA) {
        writer.StartObject();
        writer.Key("content");
        writer.StartArray();
        for (const auto& msg : abstractQA.content) {
            writer.StartObject();
            writer.Key("content");
            writer.String(msg.content.c_str());
            writer.Key("role");
            writer.String(msg.role.c_str());
            writer.EndObject();
        }
        writer.EndArray();
        writer.Key("id");
        writer.String(abstractQA.id.c_str());
        writer.EndObject();
    }
    writer.EndArray();

    writer.Key("qaList");
    writer.StartArray();
    for (const auto& qaItem : logData.historyData.qaList) {
        writer.StartObject();
        writer.Key("content");
        writer.StartArray();
        for (const auto& msg : qaItem.content) {
            writer.StartObject();
            writer.Key("content");
            writer.String(msg.content.c_str());
            writer.Key("role");
            writer.String(msg.role.c_str());
            writer.EndObject();
        }
        writer.EndArray();
        writer.Key("id");
        writer.String(qaItem.id.c_str());
        writer.Key("label");
        writer.String(qaItem.label.c_str());
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();

    writer.Key("rewrite");
    writer.StartObject();
    writer.Key("model_input");
    writer.String(logData.rewrite.modelInput.c_str());
    writer.Key("model_output");
    writer.String(logData.rewrite.modelOutput.c_str());
    writer.Key("delay");
    writer.String(logData.rewrite.delay.c_str());
    writer.EndObject();

    writer.Key("memory");
    writer.StartObject();
    writer.Key("query");
    writer.String(logData.memory.query.c_str());
    writer.Key("memory_result");
    writer.String(logData.memory.memoryResult.c_str());
    writer.Key("delay");
    writer.String(logData.memory.delay.c_str());
    writer.EndObject();

    writer.Key("response");
    writer.StartObject();
    writer.Key("code");
    writer.Int(logData.response.code);
    writer.Key("msg");
    writer.String(logData.response.msg.c_str());
    writer.Key("memory");
    writer.String(logData.response.memory.c_str());
    writer.Key("history");
    writer.StartArray();
    for (const auto& record : logData.response.history) {
        writer.StartObject();
        writer.Key("human");
        writer.String(record.human.c_str());
        writer.Key("ai");
        writer.String(record.ai.c_str());
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("response_id");
    writer.String(logData.response.responseId.c_str());
    writer.Key("rewritten_query");
    writer.String(logData.response.rewrittenQuery.c_str());
    writer.Key("total_token");
    writer.Int(logData.response.totalToken);
    writer.Key("history_token_count");
    writer.Int(logData.response.historyTokenCount);
    writer.Key("memory_token_count");
    writer.Int(logData.response.memoryTokenCount);
    writer.EndObject();

    writer.Key("ts");
    writer.String(logData.ts.c_str());
    writer.Key("delay");
    writer.String(logData.delay.c_str());
    writer.Key("turn");
    writer.String(logData.turn.c_str());
    writer.Key("session_type");
    writer.String(logData.sessionType.c_str());
    writer.EndObject();

    return s.GetString();
}

// 将WriteApiLog转换为JSON字符串
std::string ModelLogUtil::WriteApiLogToJson(const WriteApiLog& logData)
{
    StringBuffer s;
    Writer<StringBuffer> writer(s);

    writer.StartObject();
    writer.Key("request_body");
    writer.StartObject();
    writer.Key("userId");
    writer.String(logData.requestBody.userId.c_str());
    writer.Key("service_id");
    writer.String(logData.requestBody.serviceId.c_str());
    writer.Key("messages");
    writer.StartArray();
    for (const auto& msg : logData.requestBody.messages) {
        writer.StartObject();
        writer.Key("human");
        writer.String(msg.human.c_str());
        writer.Key("ai");
        writer.String(msg.ai.c_str());
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("context_id");
    writer.String(logData.requestBody.contextId.c_str());
    writer.Key("biometric_identity");
    writer.StartObject();
    writer.Key("voiceprint_id");
    writer.String(logData.requestBody.biometricIdentity.voiceprintId.c_str());
    writer.Key("face_id");
    writer.String(logData.requestBody.biometricIdentity.faceId.c_str());
    writer.EndObject();
    writer.Key("agent_role");
    writer.String(logData.requestBody.agentRole.c_str());
    writer.EndObject();

    writer.Key("response");
    writer.StartObject();
    writer.Key("code");
    writer.Int(logData.response.code);
    writer.Key("msg");
    writer.String(logData.response.msg.c_str());
    writer.EndObject();

    writer.Key("qa_extract_delay");
    writer.String(logData.qaExtractDelay.c_str());
    writer.Key("qa_summary_delay");
    writer.String(logData.qaSummaryDelay.c_str());
    writer.Key("ts");
    writer.String(logData.ts.c_str());
    writer.Key("delay");
    writer.String(logData.delay.c_str());
    writer.Key("turn");
    writer.String(logData.turn.c_str());
    writer.Key("uu_id");
    writer.String(logData.uuId.c_str());
    writer.Key("session_type");
    writer.String(logData.sessionType.c_str());
    writer.EndObject();

    return s.GetString();
}

// 将DeleteApiLog转换为JSON字符串
std::string ModelLogUtil::DeleteApiLogToJson(const DeleteApiLog& logData)
{
    StringBuffer s;
    Writer<StringBuffer> writer(s);

    writer.StartObject();
    writer.Key("request_body");
    writer.StartObject();
    writer.Key("userId");
    writer.String(logData.requestBody.userId.c_str());
    writer.Key("service_id");
    writer.String(logData.requestBody.serviceId.c_str());
    writer.Key("context_id");
    writer.String(logData.requestBody.contextId.c_str());
    writer.EndObject();

    writer.Key("response");
    writer.StartObject();
    writer.Key("code");
    writer.Int(logData.response.code);
    writer.Key("msg");
    writer.String(logData.response.msg.c_str());
    writer.EndObject();

    writer.Key("ts");
    writer.String(logData.ts.c_str());
    writer.Key("delay");
    writer.String(logData.delay.c_str());
    writer.Key("turn");
    writer.String(logData.turn.c_str());
    writer.Key("session_type");
    writer.String(logData.sessionType.c_str());
    writer.EndObject();

    return s.GetString();
}

// 写入QueryApi日志（带缓存机制）
// forceFlush: true-不校验实例是否填充完毕，直接从缓存获取、填充、写入文件、删除缓存
bool ModelLogUtil::WriteQueryApiLog(const std::string& userId, const std::string& serviceId,
    const std::string& contextId, const QueryApiLog& logData, bool forceFlush)
{
    // 打印入口参数信息
    LOG_INFO("WriteQueryApiLog ENTRY: userId=%s, serviceId=%s, contextId=%s, forceFlush=%d, "
        "requestBody={userId=%s, serviceId=%s, content=%s, contextId=%s, responseId=%s}, "
        "historyData={abstractQA.size=%zu, qaList.size=%zu}, "
        "rewrite={modelInput=%s, modelOutput=%s, delay=%s}, "
        "memory={query=%s, memoryResult=%s, delay=%s}, "
        "response={code=%d, msg=%s, memory=%s, responseId=%s, rewrittenQuery=%s, totalToken=%d, historyTokenCount=%d, memoryTokenCount=%d}, "
        "turn=%s, ts=%s, delay=%s, sessionType=%s",
        userId.c_str(), serviceId.c_str(), contextId.c_str(), forceFlush,
        logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
        logData.requestBody.content.c_str(), logData.requestBody.contextId.c_str(),
        logData.requestBody.responseId.c_str(),
        logData.historyData.abstractQA.size(), logData.historyData.qaList.size(),
        logData.rewrite.modelInput.c_str(), logData.rewrite.modelOutput.c_str(),
        logData.rewrite.delay.c_str(),
        logData.memory.query.c_str(), logData.memory.memoryResult.c_str(),
        logData.memory.delay.c_str(),
        logData.response.code, logData.response.msg.c_str(), logData.response.memory.c_str(),
        logData.response.responseId.c_str(), logData.response.rewrittenQuery.c_str(),
        logData.response.totalToken, logData.response.historyTokenCount,
        logData.response.memoryTokenCount,
        logData.turn.c_str(), logData.ts.c_str(), logData.delay.c_str(),
        logData.sessionType.c_str());

    std::string dir = userId;
    std::string timestamp = logData.ts.empty() ? "0" : logData.ts;

    // 根据 responseId 是否为空自动设置 sessionType
    QueryApiLog logDataWithSession = logData;
    if (logData.requestBody.responseId.empty()) {
        logDataWithSession.sessionType = "query_api_1";
    } else {
        logDataWithSession.sessionType = "query_api_2";
    }

    // forceFlush模式下：先从缓存获取实例，如果不存在则创建，填充字段，写入文件，删除缓存
    if (forceFlush) {
        LOG_INFO("WriteQueryApiLog: force flush mode, dir: %s, contextId: %s, timestamp: %s",
            dir.c_str(), contextId.c_str(), timestamp.c_str());

        QueryApiLog* cachedLog = nullptr;
        // 使用GetOrCreate获取缓存实例，如果不存在则创建
        if (!GetOrCreateQueryApiCache(dir, contextId, timestamp, cachedLog)) {
            LOG_ERR("WriteQueryApiLog: force flush failed, cannot get or create cache, dir: %s, contextId: %s",
                dir.c_str(), contextId.c_str());
            return false;
        }

        // 填充字段
        MergeQueryApiLog(*cachedLog, logDataWithSession);

        // 写入文件
        FlushQueryApiToFile(dir, "OM_" + contextId, *cachedLog);

        // 删除缓存
        RemoveQueryApiCache(dir, contextId, timestamp);

        LOG_INFO("WriteQueryApiLog: force flush success, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return true;
    }

    // 正常模式：检查必填参数是否填充完毕
    if (!CheckQueryApiLogComplete(logData)) {
        LOG_ERR("QueryApiLog check failed, not all params filled");
        return false;
    }

    // 当responseId为空时，代表开启新的一轮对话，需要创建新的缓存实例
    bool isNewTurn = logData.requestBody.responseId.empty();

    if (isNewTurn) {
        LOG_INFO("WriteQueryApiLog: new turn started, dir: %s, contextId: %s, timestamp: %s",
            dir.c_str(), contextId.c_str(), timestamp.c_str());
    }

    // 使用缓存接口获取或创建缓存实例
    QueryApiLog* cachedLog = nullptr;
    if (!GetOrCreateQueryApiCache(dir, contextId, timestamp, cachedLog)) {
        LOG_ERR("WriteQueryApiLog: failed to get or create cache, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return false;
    }

    // 遍历所有非空成员进行填充
    MergeQueryApiLog(*cachedLog, logDataWithSession);

    LOG_INFO("WriteQueryApiLog: filled cache, dir: %s, contextId: %s, timestamp: %s",
        dir.c_str(), contextId.c_str(), timestamp.c_str());

    return true;
}

// 写入WriteApi日志（带缓存机制）
bool ModelLogUtil::WriteWriteApiLog(const std::string& userId, const std::string& serviceId,
    const std::string& contextId, const WriteApiLog& logData, bool forceFlush)
{
    // 打印入口参数信息
    LOG_INFO("WriteWriteApiLog ENTRY: userId=%s, serviceId=%s, contextId=%s, forceFlush=%d, "
        "requestBody={userId=%s, serviceId=%s, messages.size=%zu, contextId=%s, "
        "biometricIdentity={voiceprintId=%s, faceId=%s}, agentRole=%s}, "
        "response={code=%d, msg=%s}, "
        "turn=%s, uuId=%s, qaExtractDelay=%s, qaSummaryDelay=%s, ts=%s, delay=%s, sessionType=%s",
        userId.c_str(), serviceId.c_str(), contextId.c_str(), forceFlush,
        logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
        logData.requestBody.messages.size(), logData.requestBody.contextId.c_str(),
        logData.requestBody.biometricIdentity.voiceprintId.c_str(),
        logData.requestBody.biometricIdentity.faceId.c_str(),
        logData.requestBody.agentRole.c_str(),
        logData.response.code, logData.response.msg.c_str(),
        logData.turn.c_str(), logData.uuId.c_str(), logData.qaExtractDelay.c_str(), logData.qaSummaryDelay.c_str(),
        logData.ts.c_str(), logData.delay.c_str(), logData.sessionType.c_str());

    std::string dir = userId;
    std::string timestamp = logData.ts.empty() ? "0" : logData.ts;

    // 自动设置 sessionType
    WriteApiLog logDataWithSession = logData;
    logDataWithSession.sessionType = "write_api";

    // forceFlush模式下：先从缓存获取实例，如果不存在则创建，填充字段，写入文件，删除缓存
    if (forceFlush) {
        LOG_INFO("WriteWriteApiLog: force flush mode, dir: %s, contextId: %s, timestamp: %s, trun: %s",
            dir.c_str(), contextId.c_str(), timestamp.c_str(), logData.turn.c_str());

        WriteApiLog* cachedLog = nullptr;
        // 使用GetOrCreate获取缓存实例，如果不存在则创建
        if (!GetOrCreateWriteApiCache(dir, contextId, logData.turn, cachedLog)) {
            LOG_ERR("WriteWriteApiLog: force flush failed, cannot get or create cache, dir: %s, contextId: %s",
                dir.c_str(), contextId.c_str());
            return false;
        }

        // 填充字段
        MergeWriteApiLog(*cachedLog, logDataWithSession);

        // 写入文件
        FlushWriteApiToFile(dir, "OM_" + contextId, *cachedLog);

        // 删除缓存
        RemoveWriteApiCache(dir, contextId, logData.turn);

        LOG_INFO("WriteWriteApiLog: force flush success, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return true;
    }

    // 正常模式：检查必填参数是否填充完毕
    if (!CheckWriteApiLogComplete(logData)) {
        LOG_ERR("WriteApiLog check failed, not all params filled");
        return false;
    }

    // 使用缓存接口获取或创建缓存实例
    WriteApiLog* cachedLog = nullptr;
    if (!GetOrCreateWriteApiCache(dir, contextId, logData.turn, cachedLog)) {
        LOG_ERR("WriteWriteApiLog: failed to get or create cache, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return false;
    }

    // 遍历所有非空成员进行填充
    MergeWriteApiLog(*cachedLog, logDataWithSession);

    LOG_INFO("WriteWriteApiLog: filled cache, dir: %s, contextId: %s, timestamp: %s, turn: %s",
        dir.c_str(), contextId.c_str(), timestamp.c_str(), logData.turn.c_str());

    return true;
}

// 写入DeleteApi日志（带缓存机制）
bool ModelLogUtil::WriteDeleteApiLog(const std::string& userId, const std::string& serviceId,
    const std::string& contextId, const DeleteApiLog& logData, bool forceFlush)
{
    // 打印入口参数信息
    LOG_INFO("WriteDeleteApiLog ENTRY: userId=%s, serviceId=%s, contextId=%s, forceFlush=%d, "
        "requestBody={userId=%s, serviceId=%s, contextId=%s}, "
        "response={code=%d, msg=%s}, "
        "turn=%s, ts=%s, delay=%s, sessionType=%s",
        userId.c_str(), serviceId.c_str(), contextId.c_str(), forceFlush,
        logData.requestBody.userId.c_str(), logData.requestBody.serviceId.c_str(),
        logData.requestBody.contextId.c_str(),
        logData.response.code, logData.response.msg.c_str(),
        logData.turn.c_str(), logData.ts.c_str(), logData.delay.c_str(),
        logData.sessionType.c_str());

    std::string dir = userId;

    // 自动设置 sessionType
    DeleteApiLog logDataWithSession = logData;
    logDataWithSession.sessionType = "delete_api";

    // forceFlush模式下：先从缓存获取实例，如果不存在则创建，填充字段，写入文件，删除缓存
    if (forceFlush) {
        LOG_INFO("WriteDeleteApiLog: force flush mode, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());

        DeleteApiLog* cachedLog = nullptr;
        // 使用GetOrCreate获取缓存实例，如果不存在则创建
        if (!GetOrCreateDeleteApiCache(dir, contextId, cachedLog)) {
            LOG_ERR("WriteDeleteApiLog: force flush failed, cannot get or create cache, dir: %s, contextId: %s",
                dir.c_str(), contextId.c_str());
            return false;
        }

        // 填充字段
        MergeDeleteApiLog(*cachedLog, logDataWithSession);

        // 写入文件
        FlushDeleteApiToFile(dir, "OM_" + contextId, *cachedLog);

        // 删除缓存
        RemoveDeleteApiCache(dir, contextId);

        LOG_INFO("WriteDeleteApiLog: force flush success, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return true;
    }

    // 正常模式：检查必填参数是否填充完毕
    if (!CheckDeleteApiLogComplete(logData)) {
        LOG_ERR("DeleteApiLog check failed, not all params filled");
        return false;
    }

    // 使用缓存接口获取或创建缓存实例
    DeleteApiLog* cachedLog = nullptr;
    if (!GetOrCreateDeleteApiCache(dir, contextId, cachedLog)) {
        LOG_ERR("WriteDeleteApiLog: failed to get or create cache, dir: %s, contextId: %s",
            dir.c_str(), contextId.c_str());
        return false;
    }

    // 遍历所有非空成员进行填充
    MergeDeleteApiLog(*cachedLog, logDataWithSession);

    LOG_INFO("WriteDeleteApiLog: filled cache, dir: %s, contextId: %s",
        dir.c_str(), contextId.c_str());

    return true;
}

}  // namespace DMContext