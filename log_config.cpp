/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#include "log_config.h"

#include <rapidjson/document.h>

#include "config_mgr.h"
#include "file_utils.h"
#include "logger.h"

namespace DMContext {

static const std::string SERVICE_KEY = "DMContextService";
static const std::string LOG_CONFIG_KEY = "log_config";

std::shared_ptr<LogConfig> LogConfig::GetInstance()
{
    return Singleton<LogConfig>::GetInstance();
}

std::shared_ptr<LogConfig> LogConfig::CreateInstance()
{
    return std::make_shared<LogConfig>();
}

bool LogConfig::ParseSize(const std::string& sizeStr, size_t& outBytes)
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
            LOG_ERR("LogConfig: size '%s' missing unit (K/M/G)", sizeStr.c_str());
            return false;
    }
    std::string numPart = sizeStr.substr(0, sizeStr.size() - 1);
    try {
        long long n = std::stoll(numPart);
        if (n <= 0) {
            LOG_ERR("LogConfig: size '%s' non-positive", sizeStr.c_str());
            return false;
        }
        outBytes = static_cast<size_t>(n) * multiplier;
        return true;
    } catch (...) {
        LOG_ERR("LogConfig: size '%s' parse failed", sizeStr.c_str());
        return false;
    }
}

void LogConfig::Init()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto logMap = ConfigMgr::GetInstance()->GetLogMap();
    auto it = logMap.find(LOG_CONFIG_KEY);
    if (it == logMap.end() || it->second.empty()) {
        LOG_WARNING("LogConfig: log_config path not configured, use defaults (size=%zu, num=%d)",
                    m_maxFileSize, m_maxArchiveCount);
        return;
    }

    std::string fullPath = ConfigMgr::GetInstance()->GetDMContextCfgRoot() + it->second;
    std::string content;
    if (!FileUtils::LoadContentFromPath(fullPath, content)) {
        LOG_ERR("LogConfig: load %s failed, use defaults", fullPath.c_str());
        return;
    }

    rapidjson::Document doc;
    if (doc.Parse(content.c_str()).HasParseError() || !doc.IsObject()) {
        LOG_ERR("LogConfig: parse %s failed, use defaults", fullPath.c_str());
        return;
    }

    if (!doc.HasMember(SERVICE_KEY.c_str()) || !doc[SERVICE_KEY.c_str()].IsObject()) {
        LOG_WARNING("LogConfig: section '%s' missing in %s, use defaults",
                    SERVICE_KEY.c_str(), fullPath.c_str());
        return;
    }

    const auto& sec = doc[SERVICE_KEY.c_str()];

    if (sec.HasMember("path") && sec["path"].IsString()) {
        m_logPath = sec["path"].GetString();
    }

    if (sec.HasMember("size") && sec["size"].IsString()) {
        size_t parsed = 0;
        if (ParseSize(sec["size"].GetString(), parsed)) {
            m_maxFileSize = parsed;
        }
    }

    if (sec.HasMember("num")) {
        if (sec["num"].IsInt()) {
            int n = sec["num"].GetInt();
            if (n > 0) {
                m_maxArchiveCount = n;
            }
        } else if (sec["num"].IsString()) {
            try {
                int n = std::stoi(sec["num"].GetString());
                if (n > 0) {
                    m_maxArchiveCount = n;
                }
            } catch (...) {
                LOG_ERR("LogConfig: num '%s' parse failed", sec["num"].GetString());
            }
        }
    }

    LOG_INFO("LogConfig loaded: path=%s, maxFileSize=%zu bytes, maxArchiveCount=%d",
             m_logPath.c_str(), m_maxFileSize, m_maxArchiveCount);
}

size_t LogConfig::GetMaxFileSize() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxFileSize;
}

int LogConfig::GetMaxArchiveCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxArchiveCount;
}

std::string LogConfig::GetLogPath() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_logPath;
}

}  // namespace DMContext
