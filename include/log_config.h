/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#ifndef DMCONTEXT_LOG_CONFIG_H
#define DMCONTEXT_LOG_CONFIG_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include "singleton.h"

namespace DMContext {

// 日志归档配置：从 ConfigMgr 拿到 log.json 路径，加载本服务（DMContextService）段。
// 启动时调用 Init() 一次。当前不支持热加载（path 字段已加载但暂未生效，预留给后续热加载）。
class LogConfig {
public:
    static std::shared_ptr<LogConfig> GetInstance();
    static std::shared_ptr<LogConfig> CreateInstance();

    LogConfig() = default;
    LogConfig(const LogConfig&) = delete;
    LogConfig& operator=(const LogConfig&) = delete;

    void Init();

    size_t GetMaxFileSize() const;
    int GetMaxArchiveCount() const;
    std::string GetLogPath() const;

private:
    // 解析 "20K"/"20M"/"20G"（不接受纯数字）
    static bool ParseSize(const std::string& sizeStr, size_t& outBytes);

    mutable std::mutex m_mutex;
    size_t m_maxFileSize{20 * 1024 * 1024};
    int m_maxArchiveCount{10};
    std::string m_logPath{"/opt/coremind/logs/ModelLogs/"};
};

}  // namespace DMContext

#endif  // DMCONTEXT_LOG_CONFIG_H
