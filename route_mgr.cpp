/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */
#include "route_mgr.h"

#include <logger.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "context_service.h"
#include "singleton.h"
#include "qa_short_memory_service.h"
#include "rewrite_rules/rewrite_rule_service.h"
#include "config_service.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

using namespace CMFrm::ServiceRouter;
using namespace std;

namespace DMContext {
const static int ROUTE_MAX_QUERY_THEARD = 32;
const static int ROUTE_MAX_NORMAL_THEARD = 16;

std::string RouteMgr::GetAPIProvider()
{
    return "DMContextServiceApi";
}

std::shared_ptr<RouteMgr> RouteMgr::GetInstance()
{
    return Singleton<RouteMgr>::GetInstance();
}

std::shared_ptr<RouteMgr> RouteMgr::CreateInstance()
{
    return std::make_shared<RouteMgr>();
}

void RouteMgr::RegisterRouter(const UsrServiceRouter &router)
{
    lock_guard<mutex> myLock(m_routerLock);
    m_router.emplace_back(router);
}

std::vector<UsrServiceRouter> RouteMgr::URLPatterns()
{
    return m_router;
}

void RouteMgr::InitRestRouter()
{
    // 初始化框架 - 新接口格式：/context/write, /context/query, /context/delete
    std::vector<UsrServiceRouter> routers = {
        // AISF 内部接口
        {CMFrm::COM::POST, "/v1/user/context/add", CMFrm::COM::RAW, "default", -1, HandleContextAdd},
        {CMFrm::COM::POST, "/v1/user/context/query", CMFrm::COM::RAW, "default", -1, HandleContextQueryInternal},
        {CMFrm::COM::POST, "/v1/user/context/delete", CMFrm::COM::RAW, "default", -1, HandleContextDeleteInternal},

        {CMFrm::COM::POST, "/context/v1/write", CMFrm::COM::RAW, "ContextService", -1, HandleContextAdd},
        {CMFrm::COM::POST, "/context/v1/query", CMFrm::COM::RAW, "ContextService", -1, HandleContextQueryInternal},
        {CMFrm::COM::POST, "/context/v1/delete", CMFrm::COM::RAW, "ContextService", -1, HandleContextDeleteInternal},

        // 能力开发接口
        {CMFrm::COM::POST, "/api/v1/contexts/write", CMFrm::COM::RAW, "ContextService", -1,  HandleContextWrite},
        {CMFrm::COM::POST, "/api/v1/contexts/query", CMFrm::COM::RAW, "ContextService", -1,  HandleContextQueryX},
        {CMFrm::COM::POST, "/api/v1/contexts/delete", CMFrm::COM::RAW, "ContextService", -1, HandleContextDelete},

        // 查询改写规则管理接口
        {CMFrm::COM::POST, "/api/v1/contexts/rewrite-rules/import", CMFrm::COM::RAW, "ContextService", -1, HandleRewriteRulesImport},
        {CMFrm::COM::POST, "/api/v1/contexts/rewrite-rules/query", CMFrm::COM::RAW, "ContextService", -1,  HandleRewriteRuleQuery},

        // 获取日志文件列表接口
        {CMFrm::COM::GET, "/api/v1/contexts/files", CMFrm::COM::RAW, "ContextService", -1, HandleContextFiles},

        // 通用配置外部注入接口
        {CMFrm::COM::GET, "/api/v1/contexts/configs", CMFrm::COM::RAW, "ContextService", -1, HandleConfigGetTypes},
        {CMFrm::COM::GET, "/api/v1/contexts/configs/items", CMFrm::COM::RAW, "ContextService", -1, HandleConfigGetByType},
        {CMFrm::COM::GET, "/api/v1/contexts/configs/item", CMFrm::COM::RAW, "ContextService", -1, HandleConfigGet},
        {CMFrm::COM::PUT, "/api/v1/contexts/configs/item", CMFrm::COM::RAW, "ContextService", -1, HandleConfigUpdate},

        // Prompt外部注入接口（独立处理）
        {CMFrm::COM::GET, "/api/v1/contexts/prompts/item", CMFrm::COM::RAW, "ContextService", -1, HandlePromptGet},
        {CMFrm::COM::PUT, "/api/v1/contexts/prompts/item", CMFrm::COM::RAW, "ContextService", -1, HandlePromptUpdate},
        {CMFrm::COM::PUT, "/api/v1/contexts/prompts", CMFrm::COM::RAW, "ContextService", -1, HandlePromptBatchUpdate},
    };

    for (const auto &router : routers) {
        RegisterRouter(router);
    }

    m_queryThreadPool = std::make_shared<CMFrm::ThreadPool::ThreadPool>(ROUTE_MAX_QUERY_THEARD, "router_mem_query");
    m_threadPool = std::make_shared<CMFrm::ThreadPool::ThreadPool>(ROUTE_MAX_NORMAL_THEARD, "router_normal");
}

std::shared_ptr<CMFrm::ThreadPool::ThreadPool> RouteMgr::GetQueryThreadPool()
{
    return m_queryThreadPool;
}

std::shared_ptr<CMFrm::ThreadPool::ThreadPool> RouteMgr::GetCommThreadPool()
{
    return m_threadPool;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextWrite(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - new context/write interface", __FUNCTION__);
    auto start_AddContext = std::chrono::system_clock::now();
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context, start_AddContext]() {
        DMContext::ContextService::GetInstance().AddContextNew(context, "add");
        auto end_AddContext = std::chrono::system_clock::now();
        auto duration_Context =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_AddContext - start_AddContext);
        LOG_INFO("[DMContext TIME-CONSUMING] AddContextNew %d ms", duration_Context.count());
    });
    return CMFrm::COM::ASYNC;
}

// 兼容旧接口 - 保留以便迁移
CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextAdd(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - legacy interface", __FUNCTION__);
    auto start_AddContext = std::chrono::system_clock::now();
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context, start_AddContext]() {
        DMContext::ContextService::GetInstance().AddContext(context, "add");
        auto end_AddContext = std::chrono::system_clock::now();
        auto duration_Context =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_AddContext - start_AddContext);
        LOG_INFO("[DMContext TIME-CONSUMING] AddContext %d ms", duration_Context.count());
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextQueryInternal(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - new context/query interface", __FUNCTION__);
    auto start_GetContext = std::chrono::system_clock::now();
    RouteMgr::GetInstance()->GetQueryThreadPool()->Submit([context, start_GetContext]() {
        DMContext::ContextService::GetInstance().GetContext(context);
        auto end_GetContext = std::chrono::system_clock::now();
        auto duration_GetContext =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_GetContext - start_GetContext);
        LOG_INFO("[DMContext TIME-CONSUMING] GetContextNew %d ms", duration_GetContext.count());
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextDeleteInternal(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - new context/delete interface", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        auto start_DeleteContext = std::chrono::system_clock::now();
        DMContext::ContextService::GetInstance().DeleteContext(context);
        auto end_DeleteContext = std::chrono::system_clock::now();
        auto duration_DeleteContext =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_DeleteContext - start_DeleteContext);
        LOG_INFO("[DMContext TIME-CONSUMING] DeleteContextNew %d ms", duration_DeleteContext.count());
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextDelete(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - new context/delete interface", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        auto start_DeleteContext = std::chrono::system_clock::now();
        DMContext::ContextService::GetInstance().DeleteContextNew(context);
        auto end_DeleteContext = std::chrono::system_clock::now();
        auto duration_DeleteContext =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_DeleteContext - start_DeleteContext);
        LOG_INFO("[DMContext TIME-CONSUMING] DeleteContextNew %d ms", duration_DeleteContext.count());
    });
    return CMFrm::COM::ASYNC;
}

// 新版API接口实现（/api/v1/contexts/xxx）
CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextQueryX(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - new API v1 context/query interface", __FUNCTION__);
    auto start_GetContext = std::chrono::system_clock::now();
    RouteMgr::GetInstance()->GetQueryThreadPool()->Submit([context, start_GetContext]() {
        // 调用ContextService的GetContextNew，false表示外部/新版API调用
        DMContext::ContextService::GetInstance().GetContextNew(context, false, start_GetContext);
        auto end_GetContext = std::chrono::system_clock::now();
        auto duration_GetContext =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_GetContext - start_GetContext);
        LOG_INFO("[DMContext TIME-CONSUMING] GetContextNew V1 %d ms", duration_GetContext.count());
    });
    return CMFrm::COM::ASYNC;
}

// 查询改写规则管理接口实现
CMFrm::COM::ServerRespHandleMode RouteMgr::HandleRewriteRulesImport(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - rewrite rule batch update interface", __FUNCTION__);
    // 使用单例模式进行同步调用
    RewriteRuleService::GetInstance()->ImportRules(context);
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleRewriteRuleQuery(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - rewrite rule query interface", __FUNCTION__);
    // 使用单例模式进行同步调用
    RewriteRuleService::GetInstance()->QueryRules(context);
    return CMFrm::COM::SYNC;
}

// 获取日志文件列表接口实现
CMFrm::COM::ServerRespHandleMode RouteMgr::HandleContextFiles(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter - context files query interface", __FUNCTION__);

    // 获取查询参数 user_id 和 service_id
    std::string userId = context->GetParam("user_id");
    std::string serviceId = context->GetParam("service_id");

    if (userId.empty() || serviceId.empty()) {
        LOG_ERR("HandleContextFiles: user_id or service_id is empty");
        std::stringstream ss;
        ss << R"({"code":400,"data":[]})";
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
        context->WriteJSONContentType();
        context->WriteResponseBody(ss.str());
        context->WriteAsyncResponse();
        return CMFrm::COM::SYNC;
    }

    // 构建目录路径: /opt/coremind/logs/ModelLogs/{userId}_{serviceId}/
    std::string dir = serviceId + "_" + userId;
    std::string dirPath = "/opt/coremind/logs/ModelLogs/" + dir;

    LOG_INFO("HandleContextFiles: dirPath = %s", dirPath.c_str());

    // 检查目录是否存在
    if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath)) {
        LOG_ERR("HandleContextFiles: directory does not exist, dirPath: %s", dirPath.c_str());
        std::stringstream ss;
        ss << R"({"code":404,"data":[]})";
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
        context->WriteJSONContentType();
        context->WriteResponseBody(ss.str());
        context->WriteAsyncResponse();
        return CMFrm::COM::SYNC;
    }

    // 用于存储每个文件的解析结果，按 contextId 分组
    // 结构: contextId -> (turnKey -> sessionKey -> jsonString)
    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> contextMap;

    // 用于存储 query_api_1 的 query_ts 和 query，供后续使用
    std::map<std::string, std::map<std::string, std::pair<std::string, std::string>>> turnQueryInfo;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file()) {
                std::string fileName = entry.path().filename().string();
                LOG_INFO("handle file: %s", fileName.c_str());
                if (!(fileName.size() > 3 && fileName.compare(0, 3, "OM_") == 0)) {
                    LOG_INFO("break");
                    continue;
                }
                std::string filePath = entry.path().string();
                std::ifstream file(filePath);
                if (file.is_open()) {
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    std::string content = buffer.str();
                    file.close();

                    // 解析JSON（每行是一个JSON对象）
                    std::istringstream stream(content);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty()) {
                            rapidjson::Document doc;
                            if (!doc.Parse(line.c_str(), line.length()).HasParseError()) {
                                // 获取 context_id
                                if (!doc.IsObject()) {
                                    LOG_ERR("incorrect format, line: %s", line.c_str());
                                    continue;
                                }
                                std::string contextId;
                                if (doc.HasMember("request_body") && doc["request_body"].HasMember("context_id") &&
                                    doc["request_body"]["context_id"].IsString()) {
                                    contextId = doc["request_body"]["context_id"].GetString();
                                }

                                if (contextId.empty()) {
                                    continue;
                                }

                                // 获取 turn 字段
                                std::string turn;
                                if (doc.HasMember("turn") && doc["turn"].IsString()) {
                                    turn = doc["turn"].GetString();
                                }

                                // 获取 ts 字段
                                std::string ts;
                                if (doc.HasMember("ts") && doc["ts"].IsString()) {
                                    ts = doc["ts"].GetString();
                                }

                                // 获取 session_type 字段
                                std::string sessionType;
                                if (doc.HasMember("session_type") && doc["session_type"].IsString()) {
                                    sessionType = doc["session_type"].GetString();
                                }

                                // 构建 turn key: 直接使用 turn 字段，不需要拼接 ts
                                std::string turnKey = turn;

                                // 构建 session key:
                                // query_api_1, query_api_2, delete_api 不拼接时间戳
                                // write_api 需要拼接时间戳
                                std::string sessionKey;
                                if (sessionType == "write_api") {
                                    sessionKey = sessionType + "_" + ts;
                                } else {
                                    sessionKey = sessionType;
                                }

                                // 如果是 query_api_1，提取 query_ts 和 query
                                if (sessionType == "query_api_1") {
                                    std::string queryTs = ts;
                                    std::string query;
                                    if (doc.HasMember("request_body") && doc["request_body"].HasMember("content") &&
                                        doc["request_body"]["content"].IsString()) {
                                        query = doc["request_body"]["content"].GetString();
                                    }
                                    turnQueryInfo[contextId][turnKey] = std::make_pair(queryTs, query);
                                }

                                // 将解析后的JSON转换为字符串存储
                                rapidjson::StringBuffer sb;
                                rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
                                doc.Accept(writer);

                                // 存储到对应结构中
                                contextMap[contextId][turnKey][sessionKey] = sb.GetString();
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LOG_ERR("HandleContextFiles: filesystem error, msg: %s", e.what());
        std::stringstream ss;
        ss << R"({"code":500,"data":[]})";
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
        context->WriteJSONContentType();
        context->WriteResponseBody(ss.str());
        context->WriteAsyncResponse();
        return CMFrm::COM::SYNC;
    }

    // 构建返回的JSON响应
    // 格式: {"code":200,"data":[{"context_id":"xxx","turn_时间戳":{"query_api_1":{"...":"..."}}},{}]}
    std::stringstream ss;
    ss << R"({"code":200,"data":[)";

    bool isFirstContext = true;
    for (const auto& contextPair : contextMap) {
        if (!isFirstContext) {
            ss << ",";
        }
        isFirstContext = false;

        ss << "{\"context_id\":\"" << contextPair.first << "\",";

        bool isFirstTurn = true;
        for (const auto& turnPair : contextPair.second) {
            if (!isFirstTurn) {
                ss << ",";
            }
            isFirstTurn = false;

            // 获取当前 turnKey 对应的 query_ts 和 query
            auto it = turnQueryInfo.find(contextPair.first);
            std::string queryTs;
            std::string query;
            if (it != turnQueryInfo.end()) {
                auto it2 = it->second.find(turnPair.first);
                if (it2 != it->second.end()) {
                    queryTs = it2->second.first;
                    query = it2->second.second;
                }
            }

            ss << "\"" << turnPair.first << "\":{";
            // 如果有 query_ts 和 query，添加到 JSON 中
            if (!queryTs.empty() || !query.empty()) {
                ss << "\"query_ts\":\"" << queryTs << "\",";
                ss << "\"query\":\"" << query << "\",";
            }

            bool isFirstSession = true;
            for (const auto& sessionPair : turnPair.second) {
                if (!isFirstSession) {
                    ss << ",";
                }
                isFirstSession = false;

                ss << "\"" << sessionPair.first << "\":";
                ss << sessionPair.second;
            }

            ss << "}";
        }

        ss << "}";
    }

    ss << R"(]})";

    LOG_INFO("HandleContextFiles: success, context count: %zu", contextMap.size());

    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteJSONContentType();
    context->WriteResponseBody(ss.str());
    context->WriteAsyncResponse();

    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleConfigGetTypes(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->GetConfigTypes(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleConfigGetByType(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->GetConfigByType(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleConfigGet(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->GetConfig(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandleConfigUpdate(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->UpdateConfig(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandlePromptGet(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->GetPrompt(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandlePromptUpdate(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->UpdatePrompt(context);
    });
    return CMFrm::COM::ASYNC;
}

CMFrm::COM::ServerRespHandleMode RouteMgr::HandlePromptBatchUpdate(
    const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    LOG_INFO("%s enter", __FUNCTION__);
    RouteMgr::GetInstance()->GetCommThreadPool()->Submit([context]() {
        ConfigService::GetInstance()->BatchUpdatePrompts(context);
    });
    return CMFrm::COM::ASYNC;
}
}  // namespace DMContext