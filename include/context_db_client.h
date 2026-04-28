/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#ifndef DMCONTEXTSERVICE_CONTEXT_DB_CLIENT_H
#define DMCONTEXTSERVICE_CONTEXT_DB_CLIENT_H

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include "bean/user_conversation.h"

namespace DMContext {

// ContextDB REST API 路径常量
const std::string CONTEXT_DB_API_WRITE = "/kmm/v1/user/memory/history/batch";
const std::string CONTEXT_DB_API_QUERY = "/kmm/v1/user/memories/history/query";
const std::string CONTEXT_DB_API_STATUS = "/kmm/v1/user/memories/history/update";
const std::string CONTEXT_DB_API_DELETE = "/api/v1/memory/history/delete";

class ContextDbClient {
public:
    static ContextDbClient& GetInstance();

    // 批量写入对话记录 (POST /api/v1/db/context/write)
    bool WriteConversations(const std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 按条件查询对话记录 (POST /api/v1/db/context/query)
    bool QueryConversations(const std::string& userId, const std::string& contextId,
                            const std::string& content, int32_t topK,
                            const std::string& contextStatus,
                            std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 更新上下文状态 (PUT /api/v1/db/context/status)
    bool UpdateContextStatus(const std::string& userId, const std::string& contextId,
                             const std::string& contextStatus);

    // 更新对话记录（用于更新abstract_qa、label和abstract字段）
    bool UpdateConversation(const std::shared_ptr<UserConversation>& conversation);

    bool DeleteAllMemory(const std::string& userId);

    static std::string GetKMMUrl(const std::string &path);

 private:
    ContextDbClient() = default;
    ContextDbClient(const ContextDbClient&) = delete;
    ContextDbClient& operator=(const ContextDbClient&) = delete;

    // KMM URL 缓存
    static std::string s_kmmIpCache;
    static std::mutex s_cacheMutex;

    // 发送HTTP请求（支持POST和PUT）
    std::pair<std::string, bool> SendHttpRequest(const std::string& url, const std::string& body,
                                                  const std::string& method);
    std::pair<std::string, bool> SendHttpRequest(const std::string& url, const std::string& body,
                                                  const std::string& method,
                                                  const std::map<std::string, std::string>& headers);

    // 解析时间格式为ISO8601
    static std::string FormatTimeToISO8601(const std::string& timeStr);
};
}  // namespace DMContext

#endif  // DMCONTEXTSERVICE_CONTEXT_DB_CLIENT_H