/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.

 */
#ifndef DMCONTEXT_SERVICE_INCLUDE_COMMON_MODEL_LOG_H_
#define DMCONTEXT_SERVICE_INCLUDE_COMMON_MODEL_LOG_H_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace DMContext {

// QueryApi请求体结构
struct QueryApiRequest {
    std::string userId;
    std::string serviceId;
    std::string content;
    std::string contextId;
    std::string responseId;
};

// 历史对话内容
struct MessageContent {
    std::string content;
    std::string role;
};

// 摘要QA
struct AbstractQA {
    std::vector<MessageContent> content;
    std::string id;
};

// QA列表项
struct QAItem {
    std::vector<MessageContent> content;
    std::string id;
    std::string label;
};

// 历史数据结构
struct HistoryData {
    std::vector<AbstractQA> abstractQA;
    std::vector<QAItem> qaList;
};

// Rewrite结构
struct RewriteInfo {
    std::string modelInput;
    std::string modelOutput;
    std::string delay;
};

// Memory结构
struct MemoryInfo {
    std::string query;
    std::string memoryResult;
    std::string delay;
};

// 历史对话记录
struct HistoryRecord {
    std::string human;
    std::string ai;
};

// Response结构
struct ResponseInfo {
    int code;
    std::string msg;
    std::string memory;
    std::vector<HistoryRecord> history;
    std::string responseId;
    std::string rewrittenQuery;
    int totalToken;
    int historyTokenCount;
    int memoryTokenCount;
};

// 生物特征识别信息
struct BiometricIdentity {
    std::string voiceprintId;
    std::string faceId;
};

// 消息内容
struct Message {
    std::string human;
    std::string ai;
};

// WriteApi请求体结构
struct WriteApiRequest {
    std::string userId;
    std::string serviceId;
    std::vector<Message> messages;
    std::string contextId;
    BiometricIdentity biometricIdentity;
    std::string agentRole;
};

// WriteApi响应结构
struct WriteApiResponse {
    int code;
    std::string msg;
};

// DeleteApi请求体结构
struct DeleteApiRequest {
    std::string userId;
    std::string serviceId;
    std::string contextId;
};

// DeleteApi响应结构
struct DeleteApiResponse {
    int code;
    std::string msg;
};

// QueryApi完整结构（包含请求、history_data、rewrite、memory、response）
struct QueryApiLog {
    QueryApiRequest requestBody;
    HistoryData historyData;
    RewriteInfo rewrite;
    MemoryInfo memory;
    ResponseInfo response;
    std::string turn;
    std::string ts;
    std::string delay;
    std::string dbQueryDelay;
    std::string sessionType;
};

// WriteApi完整结构
struct WriteApiLog {
    WriteApiRequest requestBody;
    WriteApiResponse response;
    std::string turn;      // 对话轮次，用于标识同一轮对话
    std::string uuId;
    std::string qaExtractDelay;
    std::string qaSummaryDelay;
    std::string ts;
    std::string delay;
    std::string sessionType;  // 会话类型：write_api
};

// DeleteApi完整结构
struct DeleteApiLog {
    DeleteApiRequest requestBody;
    DeleteApiResponse response;
    std::string turn;      // 对话轮次，用于标识同一轮对话
    std::string ts;
    std::string delay;
    std::string sessionType;  // 会话类型：delete_api
};

// Turn信息（包含query_ts, query, answer）
struct TurnInfo {
    std::string queryTs;
    std::string query;
    std::string answer;
};

class ModelLogUtil {
public:
    // QueryApi日志接口
    // forceFlush: true-不校验实例是否填充完毕，直接从缓存获取、填充、写入文件、删除缓存
    static bool WriteQueryApiLog(const std::string& userId, const std::string& serviceId,
        const std::string& contextId, const QueryApiLog& logData, bool forceFlush = false);

    // WriteApi日志接口
    static bool WriteWriteApiLog(const std::string& userId, const std::string& serviceId,
        const std::string& contextId, const WriteApiLog& logData, bool forceFlush = false);

    // DeleteApi日志接口
    static bool WriteDeleteApiLog(const std::string& userId, const std::string& serviceId,
        const std::string& contextId, const DeleteApiLog& logData, bool forceFlush = false);

private:
    // 检查单个API日志是否所有参数都填充完毕
    static bool CheckQueryApiLogComplete(const QueryApiLog& logData);
    static bool CheckWriteApiLogComplete(const WriteApiLog& logData);
    static bool CheckDeleteApiLogComplete(const DeleteApiLog& logData);

    static std::string QueryApiLogToJson(const QueryApiLog& logData);
    static std::string WriteApiLogToJson(const WriteApiLog& logData);
    static std::string DeleteApiLogToJson(const DeleteApiLog& logData);

    // QueryApi缓存相关方法
    static std::string GetQueryApiCacheKey(const std::string& dir, const std::string& fileName,
        const std::string& timestamp);
    static bool IsQueryApiLogComplete(const QueryApiLog& logData);
    static void FlushQueryApiToFile(const std::string& dir, const std::string& fileName,
        const QueryApiLog& logData);
    static bool GetQueryApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp, QueryApiLog*& logData);
    static bool GetOrCreateQueryApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp, QueryApiLog*& logData);
    static bool RemoveQueryApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp);
    static void MergeQueryApiLog(QueryApiLog& target, const QueryApiLog& source);

    // WriteApi缓存相关方法
    static std::string GetWriteApiCacheKey(const std::string& dir, const std::string& fileName,
        const std::string& timestamp);
    static bool IsWriteApiLogComplete(const WriteApiLog& logData);
    static void FlushWriteApiToFile(const std::string& dir, const std::string& fileName,
        const WriteApiLog& logData);
    static bool GetWriteApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp, WriteApiLog*& logData);
    static bool GetOrCreateWriteApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp, WriteApiLog*& logData);
    static bool RemoveWriteApiCache(const std::string& dir, const std::string& fileName,
        const std::string& timestamp);
    static void MergeWriteApiLog(WriteApiLog& target, const WriteApiLog& source);

    // DeleteApi缓存相关方法
    static std::string GetDeleteApiCacheKey(const std::string& dir, const std::string& fileName);
    static bool IsDeleteApiLogComplete(const DeleteApiLog& logData);
    static void FlushDeleteApiToFile(const std::string& dir, const std::string& fileName,
        const DeleteApiLog& logData);
    static bool GetDeleteApiCache(const std::string& dir, const std::string& fileName,
        DeleteApiLog*& logData);
    static bool GetOrCreateDeleteApiCache(const std::string& dir, const std::string& fileName,
        DeleteApiLog*& logData);
    static bool RemoveDeleteApiCache(const std::string& dir, const std::string& fileName);
    static void MergeDeleteApiLog(DeleteApiLog& target, const DeleteApiLog& source);

    // 缓存容器
    // QueryApi缓存: dir -> (fileName -> (timestamp -> QueryApiLog))
    static std::map<std::string, std::map<std::string, std::map<std::string, QueryApiLog*>>> s_queryApiCache;
    // WriteApi缓存: dir -> (fileName -> (timestamp -> WriteApiLog))
    static std::map<std::string, std::map<std::string, std::map<std::string, WriteApiLog*>>> s_writeApiCache;
    // DeleteApi缓存: dir -> (fileName -> DeleteApiLog) - 每个contextId只有一个delete_api
    static std::map<std::string, std::map<std::string, DeleteApiLog*>> s_deleteApiCache;

    static std::mutex s_cacheMutex;
};

}  // namespace DMContext

#endif  // DMCONTEXT_SERVICE_INCLUDE_COMMON_MODEL_LOG_H_