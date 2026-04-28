/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <json/json.h>
#include <memory>
#include <rapidjson/document.h>
#include <ratio>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "bean/qa_short_memory.h"
#include "common_define.h"
#include "context_service.h"
#include "context_builder.h"
#include "context_db_client.h"
#include "database/rag/rag_mgr.h"
#include "datatable/qa_short_memory_tbl.h"
#include "datatime_util.h"
#include "config/config_mgr.h"
#include "http_client_factory.h"
#include "http_client_request.h"
#include "http_helper.h"
#include "http_method.h"
#include "http_status.h"
#include "json_parser.h"
#include "json/reader.h"
#include "json/value.h"
#include "logger.h"
#include "mm_llm_component.h"
#include "mm_prompttmplt.h"
#include "model_infer.h"
#include "model_mgr.h"
#include "packet_manager.h"
#include "qa_short_memory_mgr.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "table_option.h"
#include "table_value.h"
#include "utils.h"
#include "snowflake.h"
#include "file_utils.h"
#include "bean/rule_example.h"
#include "rewrite_rules/rewrite_rule_service.h"
#include "singleton.h"
#include "llm_callback.h"
#include "model_log.h"

using namespace DMContext;
using namespace DM::RAG;
using namespace DMContext::FileUtils;
using namespace rapidjson;

// 前向声明
void buildAndWriteQueryLog(const ContextService::QueryParams& params,
        rapidjson::Value* responseJson,
        rapidjson::Value* abstractQAJson,
        rapidjson::Value* qaListJson,
        const std::string rewriteInput,
        const std::string rewrittenQuery,
        const std::string durationRewriteQuery,
        const std::string query,
        const std::string memoryResult,
        const std::string durationMemory,
        const std::string turnTimes,
        const std::string ts,
        const std::string durationDbQuery,
        const std::string delay,
        bool flag);

bool ParseAbstractQA(rapidjson::Value* abstractQAJson, AbstractQA& abstractQA);
bool ParseQAItem(rapidjson::Value* qaItemJson, QAItem& qaItem);
bool ParseResponseInfo(rapidjson::Value* responseDoc, ResponseInfo& responseInfo);

std::shared_ptr<CMFrm::ThreadPool::ThreadPool> ContextService::m_ThreadPool =
        std::make_shared<CMFrm::ThreadPool::ThreadPool>(32);

unsigned long long g_turn_timestamp = 0;

const std::string PROMPT_GENERATE_ABSTRACT_QA = "generate_abstract_qa";
const std::string PROMPT_GENERATE_ABSTRACT_CONTENT = "generate_abstract_content";
const std::string PROMPT_GENERATE_REWRITE_QA = "rewrite_query_system";


ContextService::QueryConfig& ContextService::GetQueryConfig() {
    static QueryConfig config;
    auto queryConfigParams = DMContext::ConfigMgr::GetInstance()->GetQueryConfigParams();

    auto itMode = queryConfigParams.find("mode");
    if (itMode != queryConfigParams.end()) {
        config.mode = itMode->second;
    }

    auto itMaxTurns = queryConfigParams.find("max_db_query_turns");
    if (itMaxTurns != queryConfigParams.end()) {
        config.maxDbQueryTurns = std::stoi(itMaxTurns->second);
    }

    return config;
}

// 将 qaFilter.query、qaList、abstractQA 写入文件的辅助函数
static std::string DumpHistoryData(
                            const rapidjson::Value& qaList,
                            const rapidjson::Value& abstractQA)
{
    Json::Value root;

    // 序列化 qaList
    if (qaList.Size() > 0) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        qaList.Accept(writer);
        std::string jsonStr = sb.GetString();

        Json::Value qaListRoot;
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;
        if (reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.size(), &qaListRoot, &errs)) {
            root["qaList"] = qaListRoot;
        }
    }

    // 序列化 abstractQA
    if (abstractQA.Size() > 0) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        abstractQA.Accept(writer);
        std::string jsonStr = sb.GetString();

        Json::Value abstractQARoot;
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;
        if (reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.size(), &abstractQARoot, &errs)) {
            root["abstractQA"] = abstractQARoot;
        }
    }

    Json::StreamWriterBuilder swBuilder;
    swBuilder.settings_["jsonObjectEscape"] = false;
    swBuilder.settings_["emitUTF8"] = true;
    std::string outputJson = Json::writeString(swBuilder, root);

    return outputJson;
}

std::string GetCurrentDateWithWeekday()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm *now_tm = std::localtime(&now_time_t);
    static const std::string weekdays_zh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    std::ostringstream oss;
    oss << std::put_time(now_tm, "%Y-%m-%d ") << weekdays_zh[now_tm->tm_wday];
    return oss.str();
}

bool GetLongMem(const std::string &url, const std::string &userId, const std::string &query,
                std::vector<std::string> &mems)
{
    rapidjson::StringBuffer bodyStr;
    rapidjson::Writer<rapidjson::StringBuffer> writer(bodyStr);
    writer.StartObject();
    writer.Key("query");
    writer.String(query.c_str());
    writer.Key("metadata");
    writer.String("{\"needTopicRetrival\":false,\"needSummary\":false}");
    writer.EndObject();
    std::string body = bodyStr.GetString();

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(url);
    request->AddBody(body);
    request->AddHeader("userId", userId);
    request->AddJSONContentTypeHeader();

    try {
        auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
        auto http2Client = httpClientFactory->GetHttp2Client("default");
        if (!http2Client) {
            LOG_ERR("GetLongMem - Get http2 client failed, userId=%s", userId.c_str());
            return false;
        }
        auto http2Response = http2Client->Send(request);
        if (http2Response == nullptr || !http2Response->Is2XXStatusCode()) {
            LOG_ERR("GetLongMem - http2Response failed, userId=%s", userId.c_str());
            return false;
        }
        std::string bodyString = http2Response->GetResponseBody();
        // parse kmm response
        rapidjson::Document doc;
        if (doc.Parse(bodyString.c_str()).HasParseError()) {
            LOG_ERR("parse kmm resp failed1 %s, userId=%s", bodyString.c_str(), userId.c_str());
            return false;
        }
        if (!doc.HasMember("data") || !doc["data"].IsArray()) {
            LOG_ERR("parse kmm resp failed2 %s, userId=%s", bodyString.c_str(), userId.c_str());
            return false;
        }
        for (auto &item : doc["data"].GetArray()) {
            if (item.HasMember("content") && item["content"].IsString()) {
                std::string memStr = item["content"].GetString();
                mems.push_back(memStr);
            }
        }
    } catch (...) {
        LOG_ERR("undefined failed, userId=%s", userId.c_str());
        return false;
    }
    LOG_INFO("get kmm long OK, userId=%s", userId.c_str());
    return true;
}

// 轻量级记忆检索接口 - 不进行改写，直接使用用户问题查询记忆
bool LightRetrievalMem(const std::string &url, const std::string &userId, const std::string &query,
                       std::vector<std::string> &mems)
{
    rapidjson::StringBuffer bodyStr;
    rapidjson::Writer<rapidjson::StringBuffer> writer(bodyStr);
    writer.StartObject();
    writer.Key("topK");
    writer.Int(20);
    writer.Key("query");
    writer.StartArray();
    writer.StartObject();
    writer.Key("question");
    writer.String(query.c_str());
    writer.EndObject();
    writer.EndArray();
    writer.EndObject();
    std::string body = bodyStr.GetString();

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(url);
    request->AddBody(body);
    request->AddHeader("userId", userId);
    request->AddJSONContentTypeHeader();

    try {
        auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
        auto http2Client = httpClientFactory->GetHttp2Client("default");
        if (!http2Client) {
            LOG_ERR("LightRetrievalMem - Get http2 client failed.");
            return false;
        }
        auto http2Response = http2Client->Send(request);
        if (http2Response == nullptr || !http2Response->Is2XXStatusCode()) {
            return false;
        }
        std::string bodyString = http2Response->GetResponseBody();
        // parse kmm response
        rapidjson::Document doc;
        if (doc.Parse(bodyString.c_str()).HasParseError()) {
            LOG_ERR("parse light retrieval resp failed1 %s", bodyString.c_str());
            return false;
        }
        if (!doc.HasMember("data") || !doc["data"].IsArray()) {
            LOG_ERR("parse light retrieval resp failed2 %s", bodyString.c_str());
            return false;
        }
        for (auto &item : doc["data"].GetArray()) {
            if (item.HasMember("answer") && item["answer"].IsArray()) {
                for (auto &answer : item["answer"].GetArray()) {
                    if (answer.IsString()) {
                        mems.push_back(answer.GetString());
                    }
                }
            }
        }
    } catch (...) {
        LOG_ERR("LightRetrievalMem undefined failed");
        return false;
    }
    LOG_INFO("light retrieval mem OK");
    return true;
}

Json::Value GenerateQA(const std::string &query, const std::string &answer)
{
    LOG_INFO("start... GenerateQA");
    Json::Value user;
    user["role"] = "user";
    user["content"] = query;
    Json::Value assistant;
    assistant["role"] = "assistant";
    assistant["content"] = answer;
    Json::Value qaArray;
    qaArray.append(user);
    qaArray.append(assistant);
    LOG_INFO("end... GenerateQA");
    return qaArray;
}

std::tuple<std::string, std::string> GenerateAbstractQA(std::string response)
{
    rapidjson::Document data;
    if (response.empty() || !JsonParser::Parse(response.c_str(), data)) {
        return std::make_tuple("", "");
    }
    std::string type = "";
    JsonParser::GetString(data, "tag", type);
    if (data.HasMember("abstract") && data["abstract"].IsArray()) {
        std::string strTmp;
        JsonParser::JsonToString(data["abstract"], strTmp);
        return std::make_tuple(strTmp, type);
    }
    return std::make_tuple("", type);
}

std::string ToRawString(const Json::Value &root)
{
    Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] = "";
    builder["emitUTF8"] = true;

    return Json::writeString(builder, root);
}

std::string GetQAContent(const Json::Value &qa, const std::string &targetRole)
{
    if (!qa.isArray()) {
        LOG_ERR("QA JSON is not an array");
        return "";
    }

    for (const auto &message : qa) {
        if (!message.isObject() || !message.isMember("role") || !message.isMember("content")) {
            continue;
        }
        const std::string &role = message["role"].asString();
        if (role == targetRole) {
            return message["content"].asString();
        }
    }

    return "";
}

void summaryContext(std::vector<std::shared_ptr<QAShortMemory>> &memories)
{
    for (auto &mem : memories) {
        LOG_INFO("for-summaryContext start...");
        std::string qa = mem->GetOriginalQA();
        std::string responseQA, responseAbstract;
        std::string taskType = "DMcontextserviceAbstract";

        bool successAbstract =
            ModelInfer::LLMModelInferSyncWithTaskType(qa, PROMPT_GENERATE_ABSTRACT_CONTENT, taskType, responseAbstract, "LLM_MODEL_32B");
        if (!successAbstract) {
            LOG_ERR("summaryContext successAbstract false");
        }

        mem->SetAbstract(responseAbstract);

        // 压缩逻辑判断：1.answer为空或QA总Token小于200不压缩；2.Query小于60Token，不对query压缩；3.answer小于150Token不压缩answer
        Json::Value userPrompt;
        if (!Json::Reader().parse(mem->GetOriginalQA(), userPrompt)) {
            continue;
        }
        auto query = GetQAContent(userPrompt, "user");
        auto answer = GetQAContent(userPrompt, "assistant");
        if (answer.empty() || calculate_estimated_tokens(qa) <= 200) {
            LOG_INFO("summaryContext token < 200");
            mem->SetAbstractQA(qa.c_str());
            return;
        }
        rapidjson::Document data;
        bool successQA = ModelInfer::LLMModelInferSyncWithTaskType(qa, PROMPT_GENERATE_ABSTRACT_QA, taskType, responseQA,"LLM_MODEL_32B");
        if (!successQA) {
            // 处理错误情况
            continue;
        }
        auto qaAbstract = GenerateAbstractQA(responseQA);

        if (!JsonParser::Parse(std::get<0>(qaAbstract).c_str(), data)) {
            LOG_ERR("summaryContext parse error");
            return;
        }
        if (!data.IsArray() || data.GetArray().Empty()) {
            mem->SetAbstractQA(qa.c_str());
            LOG_INFO("summaryContext: data is empty");
            return;
        }

        LOG_INFO("before calculate token query");
        if (calculate_estimated_tokens(query) <= 60) {
            LOG_INFO("summaryContext: calculate query res: %d", calculate_estimated_tokens(query));
            for (auto &item : data.GetArray()) {
                if (!item.IsObject() || !item.HasMember("role") || !item.HasMember("content")) {
                    LOG_INFO("invalid item");
                    continue;
                }
                if (strcmp(item["role"].GetString(), "user") == 0) {
                    item["content"].SetString(query.c_str(), query.size(), data.GetAllocator());
                }
            }
            std::string strTmp;
            JsonParser::JsonToString(data, strTmp);
            mem->SetAbstractQA(strTmp);
            return;
        }
        LOG_INFO("before calculate token query");
        if (calculate_estimated_tokens(answer) <= 150) {
            for (auto &item : data.GetArray()) {
                if (!item.IsObject() || !item.HasMember("role") || !item.HasMember("content")) {
                    LOG_INFO("invalid item");
                    continue;
                }
                if (strcmp(item["role"].GetString(), "assistant") == 0) {
                    item["content"].SetString(answer.c_str(), answer.size(), data.GetAllocator());
                    LOG_INFO("replace answer");
                }
            }
            std::string strTmp;
            JsonParser::JsonToString(data, strTmp);
            mem->SetAbstractQA(strTmp);
            return;
        }
        mem->SetAbstractQA(std::get<0>(qaAbstract));
        mem->SetType(std::get<1>(qaAbstract));

        LOG_INFO("summaryContext: after SetAbstractQA");
    }
}

void summaryAbstractQAContext(const std::vector<std::shared_ptr<UserConversation>> &conversations)
{
    std::string taskType = "DMcontextserviceAbstract";
    for (const auto &conv : conversations) {
        LOG_INFO("for-summaryAbstractQAContext (UserConversation) start...");
        std::string qa = conv->GetHistory();
        std::string responseQA;

        // 压缩逻辑判断
        Json::Value userPrompt;
        Json::Reader reader;
        if (!reader.parse(conv->GetHistory(), userPrompt)) {
            continue;
        }
        auto query = GetQAContent(userPrompt, "user");
        auto answer = GetQAContent(userPrompt, "assistant");
        if (answer.empty() || calculate_estimated_tokens(qa) <= 200) {
            LOG_INFO("summaryAbstractQAContext token < 200");
            conv->SetAbstractQA(qa);
            continue;
        }
        rapidjson::Document data;
        LOG_INFO("summaryAbstractQAContext: before LLMModelInferSyncWithTaskType qa:%s", qa.c_str());
        bool successQA = ModelInfer::LLMModelInferSyncWithTaskType(qa, PROMPT_GENERATE_ABSTRACT_QA, taskType,
                                                                   responseQA, "LLM_MODEL_32B");
        if (!successQA) {
            continue;
        }
        auto qaAbstract = GenerateAbstractQA(responseQA);
        LOG_INFO("summaryAbstractQAContext: before GenerateAbstractQA responseQA: %s", responseQA.c_str());

        if (!JsonParser::Parse(std::get<0>(qaAbstract).c_str(), data)) {
            LOG_ERR("summaryAbstractQAContext parse error");
            continue;
        }
        LOG_INFO("summaryAbstractQAContext: after Parse qaAbstract:%s", std::get<0>(qaAbstract).c_str());
        if (!data.IsArray() || data.GetArray().Empty()) {
            conv->SetAbstractQA(qa);
            LOG_INFO("summaryAbstractQAContext: data is empty");
            continue;
        }

        LOG_INFO("before calculate token query");
        if (calculate_estimated_tokens(query) <= 60) {
            LOG_INFO("summaryAbstractQAContext: calculate query: %s", query.c_str());
            for (auto &item : data.GetArray()) {
                if (!item.IsObject() || !item.HasMember("role") || !item.HasMember("content")) {
                    continue;
                }
                if (strcmp(item["role"].GetString(), "user") == 0) {
                    item["content"].SetString(query.c_str(), query.size(), data.GetAllocator());
                }
            }
            std::string strTmp;
            JsonParser::JsonToString(data, strTmp);
            conv->SetAbstractQA(strTmp);
            continue;
        }
        if (calculate_estimated_tokens(answer) <= 150) {
            for (auto &item : data.GetArray()) {
                if (!item.IsObject() || !item.HasMember("role") || !item.HasMember("content")) {
                    continue;
                }
                if (strcmp(item["role"].GetString(), "assistant") == 0) {
                    item["content"].SetString(answer.c_str(), answer.size(), data.GetAllocator());
                }
            }
            std::string strTmp;
            JsonParser::JsonToString(data, strTmp);
            conv->SetAbstractQA(strTmp);
            continue;
        }
        conv->SetAbstractQA(std::get<0>(qaAbstract));
        conv->SetLabel(std::get<1>(qaAbstract));

        LOG_INFO("summaryAbstractQAContext: after SetAbstractQA");
    }
}

// UserConversation 版本的 summaryContext
void summaryAbstractContext(const std::vector<std::shared_ptr<UserConversation>> &conversations)
{
    std::string taskType = "DMcontextserviceAbstract";
    for (const auto &conv : conversations) {
        LOG_INFO("for-summaryAbstractContext (UserConversation) start...");
        std::string qa = conv->GetHistory();
        std::string responseQA, responseAbstract;
        bool successAbstract = ModelInfer::LLMModelInferSyncWithTaskType(qa, PROMPT_GENERATE_ABSTRACT_CONTENT, taskType,
                                                                         responseAbstract, "LLM_MODEL_32B");
        if (!successAbstract) {
            LOG_ERR("summaryAbstractContext successAbstract false");
        }

        conv->SetAbstract(responseAbstract);
        LOG_INFO("summaryAbstractContext: after SetAbstract");
    }
}

bool parseJsonToUserContext(const std::string &jsonStr, UserContext &userContext)
{
    Json::Value root;
    Json::Reader reader;

    if (!reader.parse(jsonStr, root)) {
        return false;
    }

    userContext.userId = root.get("user_id", "").asString();
    userContext.subUserId = root.get("sub_user_id", "").asString();
    userContext.sessionId = root.get("session_id", "").asString();
    userContext.agentName = root.get("agent_name", "").asString();
    userContext.label = root.get("label", "").asString();

    const Json::Value &contextArray = root["context"];
    for (const auto &item : contextArray) {
        ContextItem contextItem;
        contextItem.query = item.get("query", "").asString();
        contextItem.answer = item.get("answer", "").asString();
        contextItem.customSummary = item.get("custom_summary", "").asString();
        contextItem.reWriteQuery = item.get("re_write_query", "").asString();
        contextItem.date = item.get("date", "").asString();

        userContext.context.push_back(contextItem);
    }
    return true;
}

bool parseJsonToQAFilter(const std::string &jsonStr, QAFilter &qaFilter)
{
    Json::Value root;
    Json::Reader reader;

    if (!reader.parse(jsonStr, root)) {
        return false;
    }

    qaFilter.userId = root.get("user_id", "").asString();
    qaFilter.subUserId = root.get("sub_user_id", "").asString();
    qaFilter.sessionId = root.get("session_id", "").asString();
    qaFilter.currentAgent = root.get("current_agent", "").asString();
    qaFilter.limitQARounds = root.get("limit_qa_rounds", 100).asInt();
    qaFilter.maxQATokens = root.get("max_qa_tokens", 2000).asInt();
    qaFilter.query = root.get("query", "").asString();
    qaFilter.rewrite_query = root.get("rewrite_query", "").asString();
    qaFilter.queryType = root.get("query_type", "").asString();
    qaFilter.needLongMemory = root.get("need_long_memory", "").asString();
    qaFilter.needStartGetMemory = root.get("need_start_get_memory", "").asString();
    return true;
}

std::vector<std::string> convertToShortMemory(UserContext &userContext,
                                              std::vector<std::shared_ptr<QAShortMemory>> &shortMemories)
{
    std::string time = GetCurrentTime();
    LOG_INFO("convert-start...");
    std::vector<std::string> ids;
    for (auto &contextItem : userContext.context) {
        auto userId = userContext.userId;
        std::string originalQA = ToRawString(GenerateQA(contextItem.query, contextItem.answer));
        auto abstractQA = "";
        auto abstract = contextItem.customSummary;
        auto type = userContext.label;
        auto keys = "";
        auto rewriteQuestion = contextItem.reWriteQuery;
        auto shortMemory = std::make_shared<QAShortMemory>(userId, originalQA, abstractQA, abstract,
                                                           rewriteQuestion.c_str(), type, keys, time, time);
        shortMemory->SetSubUserId(userContext.subUserId.c_str());
        shortMemory->SetSessionId(userContext.sessionId.c_str());
        shortMemory->SetAgentName(userContext.agentName.c_str());
        ids.push_back(shortMemory->GetUuid());
        shortMemories.push_back(shortMemory);
    }
    LOG_INFO("convert-end..");

    return ids;
}

std::string convertIdsToStr(std::vector<std::string> ids)
{
    Json::Value root;
    Json::Value contextIdArray(Json::arrayValue);
    for (auto &id : ids) {
        contextIdArray.append(id);
    }
    root["context_id"] = contextIdArray;
    Json::StreamWriterBuilder writerBuilder;
    std::string idsString = Json::writeString(writerBuilder, root);
    return idsString;
}

using QABuffer = ContextServiceImpl::QABuffer;
using QACompleteCallback = ContextServiceImpl::QACompleteCallback;

static QACompleteCallback g_qaCallback;
static std::mutex g_mutex;
static std::map<std::string, QABuffer> g_bufferPool;

static std::string MakeKey(const std::string& userId, const std::string& contextId)
{
    return userId + ":" + contextId;
}

static std::string GetStringField(const Value& doc, const char* key)
{
    if (doc.HasMember(key) && doc[key].IsString()) {
        return doc[key].GetString();
    }
    return "";
}

static std::string BuildResultJson(const std::string& userId,
                                   const std::string& serviceId,
                                   const std::string& contextId,
                                   const std::string& errorMsg)
{
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    writer.StartObject();
    writer.Key("userId"); writer.String(userId.c_str());
    writer.Key("service_id"); writer.String(serviceId.c_str());
    writer.Key("context_id"); writer.String(contextId.c_str());
    writer.Key("messages"); writer.String("");
    writer.Key("biometric_identity");
    writer.StartObject();
    writer.Key("voiceprint_id"); writer.String("");
    writer.Key("face_id"); writer.String("");
    writer.EndObject();
    writer.Key("agent_role"); writer.String("");
    writer.Key("taskstatus"); writer.String("error");
    writer.Key("error_msg"); writer.String(errorMsg.c_str());
    writer.EndObject();
    return sb.GetString();
}

static std::string BuildSuccessJson(const std::string& userId,
                                    const std::string& serviceId,
                                    const std::string& contextId,
                                    const std::string& voiceprintId,
                                    const std::string& faceId,
                                    const std::string& agentRole,
                                    const std::string& taskStatus)
{
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    writer.StartObject();
    writer.Key("userId"); writer.String(userId.c_str());
    writer.Key("service_id"); writer.String(serviceId.c_str());
    writer.Key("context_id"); writer.String(contextId.c_str());
    writer.Key("messages"); writer.String("");
    writer.Key("biometric_identity");
    writer.StartObject();
    writer.Key("voiceprint_id"); writer.String(voiceprintId.c_str());
    writer.Key("face_id"); writer.String(faceId.c_str());
    writer.EndObject();
    writer.Key("agent_role"); writer.String(agentRole.c_str());
    writer.Key("taskstatus"); writer.String(taskStatus.c_str());
    writer.Key("error_msg"); writer.String("success");
    writer.EndObject();
    return sb.GetString();
}

static void ProcessMessages(QABuffer& buffer,
                            const Value& messages,
                            const std::string& userId,
                            const std::string& serviceId,
                            const std::string& contextId,
                            const std::string& voiceprintId,
                            const std::string& faceId,
                            const std::string& agentRole,
                            bool ignoreThisRound)
{
    if (messages.IsArray()) {
        for (const auto& msg : messages.GetArray()) {
            if (!msg.IsObject()) continue;  // 安全检查：跳过非对象元素
            std::string human = GetStringField(msg, "human");
            std::string ai = GetStringField(msg, "ai");
            if (!human.empty() || !ai.empty()) {
                if (g_qaCallback) {  // 安全检查：callback 可为空
                    buffer.Write(human, ai, userId, serviceId, contextId,
                                 voiceprintId, faceId, agentRole, ignoreThisRound, g_qaCallback);
                }
            }
        }
    } else if (messages.IsObject()) {
        std::string human = GetStringField(messages, "human");
        std::string ai = GetStringField(messages, "ai");
        if (!human.empty() || !ai.empty()) {
            if (g_qaCallback) {  // 安全检查：callback 可为空
                buffer.Write(human, ai, userId, serviceId, contextId,
                             voiceprintId, faceId, agentRole, ignoreThisRound, g_qaCallback);
            }
        }
    }
}

std::string ProcessMessage(const std::string& inputJson)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    Document doc;
    if (doc.Parse(inputJson.c_str()).HasParseError()) {
        return BuildResultJson("", "", "", "json_error");
    }

    // 校验必选参数
    if (!doc.HasMember("userId") || !doc.HasMember("service_id") ||
        !doc.HasMember("messages") || !doc.HasMember("context_id")) {
        return BuildResultJson("", "", "", "param_error");
    }

    std::string userId = GetStringField(doc, "userId");
    std::string serviceId = GetStringField(doc, "service_id");
    std::string contextId = GetStringField(doc, "context_id");

    if (userId.empty() || serviceId.empty() || contextId.empty()) {
        return BuildResultJson(userId, serviceId, contextId, "param_error");
    }

    std::string voiceprintId = "";
    std::string faceId = "";
    if (doc.HasMember("biometric_identity")) {
        voiceprintId = GetStringField(doc["biometric_identity"], "voiceprint_id");
        faceId = GetStringField(doc["biometric_identity"], "face_id");
    }
    std::string agentRole = GetStringField(doc, "agent_role");
    bool ignoreThisRound = doc.HasMember("ignore_this_round") && doc["ignore_this_round"].IsBool()
                           ? doc["ignore_this_round"].GetBool() : false;

    // 安全检查：验证 messages 格式
    if (!doc["messages"].IsArray() && !doc["messages"].IsObject()) {
        return BuildResultJson(userId, serviceId, contextId, "invalid_messages_format");
    }

    std::string key = MakeKey(userId, contextId);
    QABuffer& buffer = g_bufferPool[key];

    ProcessMessages(buffer, doc["messages"], userId, serviceId, contextId,
                    voiceprintId, faceId, agentRole, ignoreThisRound);

    if (buffer.GetState() == QABuffer::HUMAN_CACHED) {
        return BuildSuccessJson(userId, serviceId, contextId, voiceprintId, faceId, agentRole, "Pending");
    }

    return BuildSuccessJson(userId, serviceId, contextId, voiceprintId, faceId, agentRole, "Completed");
}

void SetQACompleteCallback(QACompleteCallback callback)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_qaCallback = callback;
}

void FlushAllBuffers()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& pair : g_bufferPool) {
        pair.second.Flush("", "", "", "", "", "", g_qaCallback);
    }
    g_bufferPool.clear();
}

void DMContext::ContextService::AddContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                           std::string opType)
{
    auto body = context->GetRequestBody();
    UserContext userContext;
    LOG_INFO("parseJsonToUserContext start...");
    if (!parseJsonToUserContext(body, userContext)) {
        HttpHelper::WriteResponse(context, "Parse request body failed", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    if (userContext.userId.empty() || userContext.context.empty()) {
        HttpHelper::WriteResponse(context, "user_id or  context is empty", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    std::vector<std::shared_ptr<QAShortMemory>> shortMemories;
    rapidjson::Document doc(rapidjson::kObjectType);
    if (!JsonParser::Parse(body.c_str(), doc)) {
        LOG_INFO("Parse request body failed");
        HttpHelper::WriteResponse(context, "Parse request body failed", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    LOG_INFO("Start... convertToShortMemory");
    auto ids = convertToShortMemory(userContext, shortMemories);
    LOG_INFO("End... convertToShortMemory");

    if (ids.empty()) {
        LOG_INFO("Parse short memory failed");
        HttpHelper::WriteResponse(context, "Parse short memory failed", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    QAMemoryAgingPolicy policy = QAMemoryAgingPolicy();
    auto agingPolicy = JsonParser::GetNode(doc, "agingPolicy");
    if (agingPolicy != nullptr && agingPolicy->IsObject()) {
        JsonParser::GetInt(agingPolicy->GetObject(), "maxRecords", policy.maxRecords);
        JsonParser::GetInt(agingPolicy->GetObject(), "agingTime", policy.agingTime);
    }

    if (opType == "update") {
        HttpHelper::WriteResponse(context, "No supported operations.", CMFrm::COM::HttpStatus::HTTP400);
    } else {
        LOG_INFO("add qa pair list");
        auto notEmptyMemory = filterNotEmptyMemory(shortMemories);
        addQAPairsAndSummary(context, ids, notEmptyMemory, userContext);
        LOG_INFO("End... Add & Summary");
    }
}

std::vector<std::shared_ptr<QAShortMemory>> DMContext::ContextService::filterNotEmptyMemory(
    const std::vector<std::shared_ptr<QAShortMemory>>& shortMemories)
{
    std::vector<std::shared_ptr<QAShortMemory>> notEmptyMemory;
    for (auto& mem : shortMemories) {
        Json::Value userPrompt;
        Json::Reader reader;
        if (!reader.parse(mem->GetOriginalQA(), userPrompt)) {
            continue;
        }
        auto query = GetQAContent(userPrompt, "user");
        auto answer = GetQAContent(userPrompt, "assistant");

        if (mem->GetAbstractQA().empty()) {
            mem->SetAbstractQA(mem->GetOriginalQA());
        }

        if (!answer.empty()) {
            notEmptyMemory.push_back(mem);
        }
    }
    return notEmptyMemory;
}

void DMContext::ContextService::addQAPairsAndSummary(
    const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
    const std::vector<std::string>& ids,
    std::vector<std::shared_ptr<QAShortMemory>>& notEmptyMemory,
    const UserContext& userContext)
{
    if (!QAShortMemoryMgr::GetInstance()->AddQAPairs(notEmptyMemory)) {
        HttpHelper::WriteResponse(context, "Interact with database failed", CMFrm::COM::HttpStatus::HTTP400);
    } else {
        HttpHelper::WriteResponse(context, convertIdsToStr(ids), CMFrm::COM::HttpStatus::HTTP200);
    }

    LOG_INFO("start... summaryContext");
    if (!userContext.context.empty() && userContext.context[0].customSummary.empty()) {
        summaryContext(notEmptyMemory);
        LOG_INFO("end... summaryContext");
        for (auto& abstractQA : notEmptyMemory) {
            QAShortMemoryMgr::GetInstance()->UpdateQAPair(abstractQA->GetUuid(), abstractQA);
        }
    }
}

void DMContext::ContextService::GetContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    auto body = context->GetRequestBody();
    QAFilter qaFilter;
    if (!parseJsonToQAFilter(body, qaFilter)) {
        HttpHelper::WriteResponse(context, "Parse request body failed", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    std::string userid = qaFilter.userId;
    if (userid.empty()) {
        HttpHelper::WriteResponse(context, "userid invalid", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    int limit = qaFilter.limitQARounds <= 0 ? 20 : qaFilter.limitQARounds;
    if (qaFilter.needStartGetMemory == "true") {
        submitAsyncMemoryTask(qaFilter, limit);
    }
    
    std::string res;
    QAShortMemoryMgr::GetInstance()->QueryTopKQA(qaFilter, limit, res);
    LOG_INFO("QueryTopKQA end");
    if (res.empty()) {
        HttpHelper::WriteResponse(context, "Query failed or result empty", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }
    HttpHelper::WriteResponse(context, res, CMFrm::COM::HttpStatus::HTTP200);
}

void DMContext::ContextService::submitAsyncMemoryTask(const QAFilter& qaFilter, int limit)
{
    if (!TryAcquireAddingStatus(qaFilter.userId)) {
        return;
    }
    ClearUserCache(qaFilter.userId);
    m_ThreadPool->Submit([this, qaFilter, limit]() {
        LOG_INFO("Start async task for sysAgent rewrite query...");

        std::string rewriteQuery = qaFilter.rewrite_query.empty() ? qaFilter.query : qaFilter.rewrite_query;
        auto qaFuture = std::async(std::launch::async, [qaFilter, rewriteQuery, limit]() -> std::vector<std::string> {
            std::vector<std::string> ids;
            LOG_INFO("QueryRelatedQAPairs start...");
            if (!QAShortMemoryMgr::GetInstance()->QueryRelatedQAPairs(qaFilter.userId, rewriteQuery, limit, ids)) {
                LOG_ERR("QueryRelatedQAPairs from database failed");
            }
            LOG_INFO("QueryRelatedQAPairs end, get ids size: %d", ids.size());
            return ids;
        });

        auto longMemFuture = std::async(std::launch::async,
            [qaFilter, rewriteQuery]() -> std::tuple<std::vector<std::string>, long long> {
            std::vector<std::string> longMem;
            auto taskStart = std::chrono::steady_clock::now();
             std::string kmmUrl = ContextDbClient::GetInstance().GetKMMUrl("/kmm/v1/user/memories/get");
             if (kmmUrl.empty()) {
                  LOG_ERR("GetKMMUrl failed, url is empty");
                  return std::make_tuple(longMem, 0);
              }
            LOG_INFO("Get KMM URL %s", kmmUrl.c_str());
            GetLongMem(kmmUrl, qaFilter.userId, rewriteQuery, longMem);
            auto taskEnd = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(taskEnd - taskStart);
            return std::make_tuple(longMem, duration.count());
        });

        auto ids = qaFuture.get();
        auto longMemResult = longMemFuture.get();
        auto longMem = std::get<0>(longMemResult);
        auto longMemDuration = std::get<1>(longMemResult);
        
        LOG_INFO("longMemFuture execution time: %lld ms, longMem size: %d", 
                 longMemDuration, longMem.size());

        LOG_INFO("Parallel tasks completed, executing AddMem...");
        AddMem(qaFilter.userId, qaFilter.rewrite_query, qaFilter.rewrite_query, ids, longMem);
        LOG_INFO("Async task for sysAgent rewrite query completed.");
        ReleaseAddingStatus(qaFilter.userId);
    });
}

ContextService &ContextService::GetInstance()
{
    static ContextService instance;
    static bool initialized = []() {
        instance.StartCacheTimer();
        instance.SetCallBackFunc();
        return true;
    }();
    (void)initialized;
    return instance;
}

ContextService::~ContextService()
{
    StopCacheTimer();
    FlushAllBuffers();
}

void DMContext::ContextService::SetCallBackFunc()
{
    LOG_INFO("SetCallBackFunc enter");
    SetQACompleteCallback([this](const std::string& json) {
        ProcessQACompleteCallback(json);
    });
}

// 新增：启动缓存相关定时任务
void DMContext::ContextService::StartCacheTimer()
{
    // 启动定时刷新缓存到数据库任务（每500ms执行一次）
    auto writeCacheTimer = std::make_shared<CMFrm::Timer::TimerTask>(
            "FlushWriteCache", 0, 100, [this]() { this->FlushWriteCache(); }, 1);
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(writeCacheTimer);

    // m_writeCacheRunning = true;
    // m_writeCacheThread = std::thread([this]() {
    //     while (m_writeCacheRunning) {
    //         std::this_thread::sleep_for(std::chrono::milliseconds(500));
    //         FlushWriteCache();
    //     }
    // });

    // 启动定时清理过期上下文缓存任务
    InitCleanExpiredContextCacheTimer();
}

// 新增：停止缓存相关定时任务
void DMContext::ContextService::StopCacheTimer()
{
    m_writeCacheRunning = false;
    if (m_writeCacheThread.joinable()) {
        m_writeCacheCv.notify_one();
        m_writeCacheThread.join();
    }
    // 最后刷新一次剩余缓存
    FlushWriteCache();
}

// 新增：添加到写入缓存
void DMContext::ContextService::AddToWriteCache(
        const std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    std::lock_guard<std::mutex> lock(m_writeCacheMutex);
    // 直接插入到 unordered_map，每个 conversation 以其 ID 作为 key
    for (const auto& conv : conversations) {
        m_writeCache[conv->GetId()] = conv;
    }
    LOG_INFO("AddToWriteCache: added %zu conversations to cache, current cache size=%zu",
             conversations.size(), m_writeCache.size());
}

// 新增：更新写入缓存中的数据（根据GetId查找，缓存中有则更新abstract_qa、label和abstract，没有返回false）
bool DMContext::ContextService::UpdateInWriteCache(const std::string& convId,
                                                   const std::shared_ptr<UserConversation>& conversation)
{
    std::lock_guard<std::mutex> lock(m_writeCacheMutex);
    // 使用 unordered_map 直接 O(1) 查找
    auto it = m_writeCache.find(convId);
    if (it != m_writeCache.end()) {
        // 只更新abstract_qa、label和abstract字段，保留其他字段
        it->second->SetAbstractQA(conversation->GetAbstractQA());
        it->second->SetLabel(conversation->GetLabel());
        it->second->SetAbstract(conversation->GetAbstract());
        LOG_INFO("UpdateInWriteCache: updated cache, convId=%s, abstract_qa=%s, label=%s, abstract=%s",
                 convId.c_str(), it->second->GetAbstractQA().c_str(), it->second->GetLabel().c_str(), it->second->GetAbstract().c_str());
        return true;
    }
    LOG_INFO("UpdateInWriteCache: not found in cache, convId=%s", convId.c_str());
    return false;
}

// 新增：刷新缓存到数据库
void DMContext::ContextService::FlushWriteCache()
{
    std::unordered_map<std::string, std::shared_ptr<UserConversation>> cacheToFlush;
    {
        std::lock_guard<std::mutex> lock(m_writeCacheMutex);
        if (m_writeCache.empty()) {
            return;
        }
        cacheToFlush = std::move(m_writeCache);
        m_writeCache.clear();
    }

    // 直接从 unordered_map 获取所有值进行保存
    if (!cacheToFlush.empty()) {
        std::vector<std::shared_ptr<UserConversation>> allConversations;
        allConversations.reserve(cacheToFlush.size());
        for (auto& pair : cacheToFlush) {
            allConversations.push_back(pair.second);
        }
        if (!SaveToDatabase(allConversations)) {
            LOG_ERR("FlushWriteCache: save to database failed, total %zu conversations", allConversations.size());
        } else {
            LOG_INFO("FlushWriteCache: successfully saved %zu conversations to database in one batch",
                     allConversations.size());
        }
    }
}

// 新增：添加上下文缓存
void DMContext::ContextService::AddContextCache(const std::string& responseId,
                                                const std::shared_ptr<ContextCache>& cache)
{
    std::lock_guard<std::shared_mutex> lock(m_contextCacheMutex);
    m_contextCacheMap[responseId] = cache;
    LOG_INFO("AddContextCache: responseId=%s, current cache size=%zu",
             responseId.c_str(), m_contextCacheMap.size());
}

// 新增：获取上下文缓存
std::shared_ptr<DMContext::ContextCache> DMContext::ContextService::GetContextCache(const std::string& responseId)
{
    std::shared_lock<std::shared_mutex> lock(m_contextCacheMutex);
    auto it = m_contextCacheMap.find(responseId);
    if (it != m_contextCacheMap.end()) {
        LOG_INFO("GetContextCache: hit, responseId=%s", responseId.c_str());
        return it->second;
    }
    LOG_INFO("GetContextCache: miss, responseId=%s", responseId.c_str());
    return nullptr;
}

// 新增：清理过期缓存
void DMContext::ContextService::CleanExpiredCache(int64_t expireSeconds)
{
    std::lock_guard<std::shared_mutex> lock(m_contextCacheMutex);
    int64_t now = time(nullptr);
    auto it = m_contextCacheMap.begin();
    while (it != m_contextCacheMap.end()) {
        if (now - it->second->timestamp > expireSeconds) {
            LOG_INFO("CleanExpiredCache: remove expired cache, responseId=%s", it->first.c_str());
            it = m_contextCacheMap.erase(it);
        } else {
            ++it;
        }
    }
}

// 新增：根据responseId删除单个上下文缓存
void DMContext::ContextService::RemoveContextCache(const std::string& responseId)
{
    std::lock_guard<std::shared_mutex> lock(m_contextCacheMutex);
    auto it = m_contextCacheMap.find(responseId);
    if (it != m_contextCacheMap.end()) {
        LOG_INFO("RemoveContextCache: remove cache, responseId=%s", responseId.c_str());
        m_contextCacheMap.erase(it);
    }
}

// 新增：定时清理过期上下文缓存任务
void DMContext::ContextService::InitCleanExpiredContextCacheTimer()
{
    // 参数: 任务名, 首次延迟(5秒), 间隔(5分钟), 执行函数, 重复次数(-1表示无限)
    m_contextCacheCleanTimer = std::make_shared<CMFrm::Timer::TimerTask>(
            "CleanExpiredContextCache", 5000, 5 * 60 * 1000, [this]() { this->CleanExpiredCache(); }, 1);
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(m_contextCacheCleanTimer);
    LOG_INFO("InitCleanExpiredContextCacheTimer: started, interval=5min");
}


// 构建RewriteQuery上下文的辅助方法
// 从 JSON 消息数组中提取 role 和 content
static void ExtractMessagesFromHistory(const Json::Value& historyArray,
                                       std::vector<std::map<std::string, std::string>>& memories)
{
    // 用于临时存储解析出的消息
    std::map<std::string, std::string> userMsg;
    std::map<std::string, std::string> assistantMsg;

    for (Json::ArrayIndex i = 0; i < historyArray.size(); ++i) {
        const Json::Value& msg = historyArray[i];
        std::string role;
        std::string content;

    // 提取role，如果不存在则用空格代替
        if (msg.isMember("role") && msg["role"].isString()) {
            role = msg["role"].asString();
        } else {
            role = "done";
        }

        // 提取content，支持字符串、数组格式，如果不存在则用空格代替
        if (msg.isMember("content")) {
            if (msg["content"].isString()) {
                // 字符串类型
                content = msg["content"].asString();
            } else if (msg["content"].isArray()) {
                // 数组类型，转换为字符串（提取数组中的有效内容）
                Json::StreamWriterBuilder writer;
                writer.settings_["jsonObjectEscape"] = false;
                writer.settings_["emitUTF8"] = true;
                content = Json::writeString(writer, msg["content"]);
            } else {
                // 其他类型（非字符串非数组），用空格代替
                content = "done";
            }
        } else {
            // content 字段不存在，用空格代替
            content = "done";
        }

        // 按 role 分类存储
        if (role == "user") {
            userMsg = {{"role", role}, {"content", content}};
        } else if (role == "assistant") {
            assistantMsg = {{"role", role}, {"content", content}};
        }
    }

    // 补全逻辑：确保有 user 和 assistant 各一条，不足的用空格补全
    if (userMsg.empty()) {
        userMsg = {{"role", "user"}, {"content", "done"}};
    }
    if (assistantMsg.empty()) {
        assistantMsg = {{"role", "assistant"}, {"content", "done"}};
    }

    // 按顺序加入：先 user，再 assistant
    memories.push_back(userMsg);
    memories.push_back(assistantMsg);
}

static void BuildRewriteContext(const std::vector<std::shared_ptr<UserConversation>>& conversations,
                                std::vector<std::map<std::string, std::string>>& memories)
{
    if (conversations.empty()) {
        return;
    }

    size_t totalSize = conversations.size();
    constexpr size_t maxTotalTurns = 5;
    constexpr size_t recentRawTurns = 1;

    // 用于存储两部分数据，最后合并
    std::vector<std::map<std::string, std::string>> recentMemories;
    std::vector<std::map<std::string, std::string>> abstractMemories;

    // 1. 取最近1轮原始对话 (history 字段)
    if (totalSize >= recentRawTurns) {
        size_t recentStartIdx = totalSize - recentRawTurns;
        for (size_t idx = recentStartIdx; idx < totalSize; ++idx) {
            const auto& conv = conversations[idx];
            std::string history = conv->GetHistory();
            if (history.empty()) {
                continue;
            }

            Json::Value originQA;
            Json::Reader reader;
            if (!reader.parse(history, originQA) || !originQA.isArray()) {
                continue;
            }

            ExtractMessagesFromHistory(originQA, recentMemories);
        }
    }

    // 2. 取前面N轮摘要，总共5轮，所以摘要取 maxTotalTurns - recentRawTurns = 4 轮
    constexpr size_t abstractTurns = maxTotalTurns - recentRawTurns;
    if (totalSize > recentRawTurns) {
        size_t abstractStartIdx = (totalSize > recentRawTurns + abstractTurns) 
                                    ? (totalSize - recentRawTurns - abstractTurns) : 0;
        size_t abstractEndIdx = totalSize - recentRawTurns;

        for (size_t idx = abstractStartIdx; idx < abstractEndIdx; ++idx) {
            const auto& conv = conversations[idx];
            std::string abstractQA = conv->GetAbstractQA();
            if (abstractQA.empty()) {
                continue;
            }

            Json::Value abstractDoc;
            Json::Reader reader;
            if (!reader.parse(abstractQA, abstractDoc) || !abstractDoc.isArray()) {
                continue;
            }

            ExtractMessagesFromHistory(abstractDoc, abstractMemories);
        }
    }

    // 3. 合并：先加摘要(旧)，再加最近1轮(新)，最新的在后面
    memories.insert(memories.end(), abstractMemories.begin(), abstractMemories.end());
    memories.insert(memories.end(), recentMemories.begin(), recentMemories.end());
}

// 执行RewriteQuery的大模型推理
static std::string DoRewriteInfer(const std::string& query, const mmsdk::MMPromptTemplate& sysPrompt,
                                  std::vector<std::map<std::string, std::string>> mixedContext)
{
    std::promise<std::pair<int32_t, std::string>> resultPromise;
    std::future<std::pair<int32_t, std::string>> resultFuture = resultPromise.get_future();

    auto callback = std::make_shared<LLMCallback>(
        [&resultPromise](int32_t errorCode, const std::string& response) {
            resultPromise.set_value({errorCode, response});
        });

    MMUserIntention intention = {.query = query,
            .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto req = mmsdk::MMLLMRequest{
            .intentions = {intention},
            .prompt = {sysPrompt}};

    std::string modelName = std::getenv("LLM_MODEL_32B");
    MMReasonParam reasonParam = {
        .modelServiceName = SERVICE_NAME, .modelName = modelName, .temperature = 0.001,
        .schedoption = {{"X-Task-Type", "DMcontextserviceRewriteQuery"}}};
    reactLLMomponent->CreateTask(reasonParam, callback,
                                 [req, mixedContext, callback](uint32_t errorCode, const std::string& taskId) -> uint32_t {
        CustomedLLMComponent->UpdateUserContext(taskId, mixedContext, [=](uint32_t errorCode) -> uint32_t {
            reactLLMomponent->RequestAsync(taskId, req, {});
            return 0;
        });
        return 0;
    });

    // 等待 LLM 返回结果
    auto result = resultFuture.get();
    if (result.first != 0) {
        LOG_ERR("DoRewriteInfer failed with errorCode:%d", result.first);
        return "";
    }
    return result.second;
}

// 构建模型输入：将上下文和当前问题拼接在一起
static std::string BuildModelInput(const std::vector<std::map<std::string, std::string>>& memories)
{
    // 将 memories 转换为字符串格式
    std::string context;
    for (const auto& mem : memories) {
        auto itRole = mem.find("role");
        auto itContent = mem.find("content");
        if (itRole != mem.end() && itContent != mem.end()) {
            context += itRole->second + ": " + itContent->second + "\n";
        }
    }

    LOG_INFO("BuildModelInput input: %s", context.c_str());
    return context;
}

// 从改写结果中提取用户意图（前置声明）
static std::string ExtractIntentFromRewrittenResult(const std::string& rewrittenResult);

std::string ContextService::RewriteQueryFromUserConversation(
        const std::vector<std::shared_ptr<UserConversation>>& conversations,
        const std::string& query,
        std::string& input)
{
    LOG_INFO("RewriteQueryFromUserConversation start");
    if (conversations.empty()) {
        LOG_INFO("RewriteQueryFromUserConversation - conversations为空");
        return query;
    }
    LOG_INFO("成功获取非空conversations用作改写, size=%d", conversations.size());

    // 1. 构建上下文（用于后续构建 model 输入）
    std::vector<std::map<std::string, std::string>> memories;
    BuildRewriteContext(conversations, memories);

    // 2. 根据改写规则表，获取原始query得到的示例对(可能包含历史示例)，得到的结果为示例
    LOG_INFO("RewriteQueryFromUserConversation to get query: %s", query.c_str());
    std::vector<std::shared_ptr<DMContext::RuleExample>> ruleExamples;

    // 首先进行标量检索（精确匹配）
    std::vector<std::shared_ptr<RuleExample>> scalarResults;
    if (RewriteRuleService::GetInstance()->ScalarSearch(query, scalarResults)) {
        // 标量检索精确命中，直接返回结果，不需要进行近似检索
        LOG_INFO("Recall: scalar search hit, query=%s, result_count=%zu",
                 query.c_str(), scalarResults.size());
        return query;
    }
    RewriteRuleService::GetInstance()->Recall(query, ruleExamples);

    // 3. Recall -> SelectPrompt -> AssemblePrompt 形成闭环
    auto selectResult = RewriteRuleService::GetInstance()->SelectPrompt(ruleExamples);
    std::pair<std::string, std::string> fullSysPrompt = RewriteRuleService::GetInstance()->AssemblePrompt(query,
        selectResult.promptLevel, selectResult.filteredExamples);

    LOG_INFO("RewriteQueryFromUserConversation - memories size:%d", memories.size());
    input = BuildModelInput(memories);
    LOG_INFO("RewriteQueryFromUserConversation - 用户问题:%s, 上下文: %s", query.c_str(), input.c_str());

    // 4. 执行推理
    const std::string promptName = "rewrite_query";
    auto sysPrompt = mmsdk::MMPromptTemplate(mmsdk::MMPromptTmpltKey{
            .name = promptName,
            .owner = SERVICE_NAME,
    });
    sysPrompt.SetContent(fullSysPrompt.second);
    // 拼接做输入
    input = "上下文:\n" + input + "\n" + fullSysPrompt.first + "\n======================\n" + sysPrompt.GetContent();

    std::string rewrittenResult = DoRewriteInfer(fullSysPrompt.first, sysPrompt, memories);
    LOG_INFO("query %s, and userPrompt test result :%s", query.c_str(), fullSysPrompt.first.c_str());
    LOG_INFO("query %s, and sysPrompt test result :%s", query.c_str(), sysPrompt.GetContent().c_str());

    // 5. 解析JSON格式的推理结果，提取实际的用户意图
    // 返回格式可能为: rewritten:{"用户完整意图":"今天怎么样？"} 或 {"用户完整意图":"今天怎么样？"}
    std::string extractedQuery = ExtractIntentFromRewrittenResult(rewrittenResult);
    return extractedQuery.empty() ? query : extractedQuery;
}

// 从改写结果中提取用户意图
/**
 • @param rewrittenResult

格式一：
{"need_rewrite":0}
格式二：
{"need_rewrite":1,"rewritten_query":"..."}
 • @return "" or rewritten_query

 */
static std::string ExtractIntentFromRewrittenResult(const std::string& rewrittenResult)
{
    LOG_INFO("ExtractIntentFromRewrittenResult rewrittenResult is : %s", rewrittenResult.c_str());
    if (rewrittenResult.empty()) {
        return "";
    }
    rapidjson::Document doc;
    if (doc.Parse(rewrittenResult.c_str()).HasParseError() || !doc.IsObject()) {
        LOG_ERR("ExtractIntentFromRewrittenResult: failed to parse JSON, result=%s", rewrittenResult.c_str());
        return "";
    }

    // 优先检查新格式: {"意图":"..."}
    if (doc.HasMember("意图") && doc["意图"].IsString()) {
        std::string rewrittenQuery = doc["意图"].GetString();
        LOG_INFO("ExtractIntentFromRewrittenResult: extracted from 最新新格式, 意图=%s", rewrittenQuery.c_str());
        return rewrittenQuery;
    }

    // 优先检查新格式: {"用户完整意图":"..."}
    if (doc.HasMember("用户完整意图") && doc["用户完整意图"].IsString()) {
        std::string rewrittenQuery = doc["用户完整意图"].GetString();
        LOG_INFO("ExtractIntentFromRewrittenResult: extracted from 新格式, 用户完整意图=%s", rewrittenQuery.c_str());
        return rewrittenQuery;
    }

    // 检查旧格式 need_rewrite 字段
    if (!doc.HasMember("need_rewrite") || !doc["need_rewrite"].IsInt()) {
        LOG_ERR("ExtractIntentFromRewrittenResult: need_rewrite field not found or invalid");
        return "";
    }

    int needRewrite = doc["need_rewrite"].GetInt();
    if (needRewrite == 0) {
        // 格式一: {"need_rewrite":0} - 不需要重写，返回空
        return "";
    }

    // 格式二: {"need_rewrite":1,"rewritten_query":"..."}
    if (!doc.HasMember("rewritten_query") || !doc["rewritten_query"].IsString()) {
        LOG_ERR("ExtractIntentFromRewrittenResult: rewritten_query field not found or invalid");
        return "";
    }

    std::string rewrittenQuery = doc["rewritten_query"].GetString();
    LOG_INFO("ExtractIntentFromRewrittenResult: extracted rewritten_query=%s", rewrittenQuery.c_str());
    return rewrittenQuery;
}

void DMContext::ContextService::DeleteContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    auto body = context->GetRequestBody();
    QAFilter qaFilter;
    if (!parseJsonToQAFilter(body, qaFilter)) {
        HttpHelper::WriteResponse(context, "Parse request body failed or user_id miss", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    std::string userid = qaFilter.userId;
    if (userid.empty()) {
        HttpHelper::WriteResponse(context, "userid invalid", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }
    bool res = QAShortMemoryMgr::GetInstance()->DeleteQAPairs(qaFilter);
    if (!res) {
        HttpHelper::WriteResponse(context, "Delete failed", CMFrm::COM::HttpStatus::HTTP400);
    }

    HttpHelper::WriteResponse(context, "Delete sussces", CMFrm::COM::HttpStatus::HTTP200);
}

// ============ GetContextNew 相关实现 ============

bool DMContext::ContextService::ParseQueryParams(const rapidjson::Document& doc, QueryParams& params)
{
    if (!doc.HasMember("userId") || !doc["userId"].IsString()) {
        LOG_ERR("ParseQueryParams: missing userId");
        return false;
    }
    params.userId = doc["userId"].GetString();
    params.originalUserId = params.userId;

    // 新增：serviceId 用于与 userId 组合形成新的 userId
    if (!doc.HasMember("service_id") || !doc["service_id"].IsString()) {
        LOG_ERR("ParseQueryParams: missing service_id");
        return false;
    }

    params.srvId = doc["service_id"].GetString();
    params.userId = params.srvId + "_" + params.userId;

    if (!doc.HasMember("context_id") || !doc["context_id"].IsString()) {
        LOG_ERR("ParseQueryParams: missing context_id");
        return false;
    }
    params.contextId = doc["context_id"].GetString();

    if (!doc.HasMember("content") || !doc["content"].IsString()) {
        LOG_ERR("ParseQueryParams: missing content");
        return false;
    }
    params.content = doc["content"].GetString();

    params.agentRole = doc.HasMember("agent_role") && doc["agent_role"].IsString()
                       ? doc["agent_role"].GetString() : "";
    params.responseId = doc.HasMember("response_id") && doc["response_id"].IsString()
                        ? doc["response_id"].GetString() : "";

    // 解析 biometric_identity 中的 voiceprint_id 和 face_id
    if (doc.HasMember("biometric_identity") && doc["biometric_identity"].IsObject()) {
        const auto& bio = doc["biometric_identity"];
        params.voiceprintId = bio.HasMember("voiceprint_id") && bio["voiceprint_id"].IsString()
                              ? bio["voiceprint_id"].GetString() : "";
        params.faceId = bio.HasMember("face_id") && bio["face_id"].IsString()
                        ? bio["face_id"].GetString() : "";
    }

    params.mode = doc.HasMember("mode") && doc["mode"].IsString() ? doc["mode"].GetString() :
            ContextService::GetQueryConfig().mode;
    params.queryType = doc.HasMember("query_type") && doc["query_type"].IsString()
                       ? doc["query_type"].GetString() : "";

    // 支持 rewrite_query（新API bool）或 rewrite_content（旧API string）
    if (doc.HasMember("rewrite_query") && doc["rewrite_query"].IsBool()) {
        params.isRewritequery = doc["rewrite_query"].GetBool();
    } else {
        params.isRewritequery = false;
    }
    params.reWriteContent = doc.HasMember("rewrite_content") && doc["rewrite_content"].IsString()
                            ? doc["rewrite_content"].GetString() : "";

    // enable_memory: 默认true
    if (doc.HasMember("enable_memory") && doc["enable_memory"].IsBool()) {
        params.enableMemory = doc["enable_memory"].GetBool();
    } else {
        params.enableMemory = true;
    }

    // 兼容 coversion_turns（旧API）和 conversation_turns（新API）
    if (doc.HasMember("conversation_turns") && doc["conversation_turns"].IsInt()) {
        params.conversationTurns = doc["conversation_turns"].GetInt();
    } else if (doc.HasMember("coversion_turns") && doc["coversion_turns"].IsInt()) {
        // 兼容旧参数名 coversion_turns
        params.conversationTurns = doc["coversion_turns"].GetInt();
    } else if (!params.responseId.empty()) {
        params.conversationTurns = 4;
    } else {
        params.conversationTurns = 3;
    }

    // 解析 memory_count 和 memory_token_budget
    // 当两者同时存在时，以 memory_count 优先
    params.memoryCount = doc.HasMember("memory_count") && doc["memory_count"].IsInt()
                         ? doc["memory_count"].GetInt() : 20;
    params.memoryTokenBudget = doc.HasMember("memory_token_budget") && doc["memory_token_budget"].IsInt()
                               ? doc["memory_token_budget"].GetInt() : 8192;

    if (doc.HasMember("token_budget") && doc["token_budget"].IsInt()) {
        params.tokenBudget = doc["token_budget"].GetInt();
    } else {
        params.tokenBudget = 8192;
    }

    params.outputType = doc.HasMember("output_type") && doc["output_type"].IsString()
                        ? doc["output_type"].GetString() : "";

    // 检查是否是忘记记忆命令
    params.isForgetMemory = false;
    auto forgetCommands = ConfigMgr::GetInstance()->GetForgetMemoryCommands();
    if (!params.content.empty() && std::find(forgetCommands.begin(), forgetCommands.end(), params.content) != forgetCommands.end()) {
        params.isForgetMemory = true;
        params.enableMemory = false;
        params.isRewritequery = false;
        LOG_INFO("ParseQueryParams: forget memory command detected, content=%s", params.content.c_str());
    }

    return true;
}

QAFilter DMContext::ContextService::BuildQAFilter(const QueryParams& params)
{
    QAFilter filter;
    filter.userId = params.userId;
    filter.ctxId = params.contextId;
    filter.query = params.content;
    filter.rewrite_query = params.reWriteContent;
    filter.queryType = params.queryType;
    return filter;
}

std::string DMContext::ContextService::GenerateResponseId(const std::string& userId, const std::string& contextId)
{
    return "resp_" + std::to_string(std::hash<std::string>{}(userId + contextId + std::to_string(time(nullptr))));
}

void DMContext::ContextService::ProcessHistoryData(
        const std::vector<std::shared_ptr<UserConversation>>& conversations,
        rapidjson::Value& qaList,
        rapidjson::Value& abstractQA,
        const std::string& query,
        rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& alloc)
{
    for (auto& conv : conversations) {
        rapidjson::Value qaItem(rapidjson::kObjectType);
        qaItem.AddMember("id", rapidjson::Value(conv->GetId().c_str(), alloc), alloc);

        // qaList 使用 history 字段 - 原始QA对（JSON数组），直接解析成 JSON 数组
        // 使用统一的 allocator，避免内存分配器不匹配问题
        rapidjson::Document historyDoc(rapidjson::kArrayType, &alloc);
        if (historyDoc.Parse(conv->GetHistory().c_str()).HasParseError()) {
            LOG_INFO("Parse history failed, id=%s", conv->GetId().c_str());
            continue;
        }
        rapidjson::Value historyCopy;
        historyCopy.CopyFrom(historyDoc, alloc);
        qaItem.AddMember("content", historyCopy, alloc);
        qaItem.AddMember("label", rapidjson::Value(conv->GetLabel().c_str(), alloc), alloc);
        qaList.PushBack(qaItem.Move(), alloc);

        // abstractQA 使用 abstract_qa 字段 - 完整QA对JSON，需要解析成 JSON 对象
        if (!conv->GetAbstractQA().empty()) {
            rapidjson::Value abstractItem(rapidjson::kObjectType);
            // 使用统一的 allocator，避免内存分配器不匹配问题
            rapidjson::Document abstractDoc(rapidjson::kObjectType, &alloc);
            if (abstractDoc.Parse(conv->GetAbstractQA().c_str()).HasParseError()) {
                LOG_INFO("Parse abstractQA failed, id=%s", conv->GetId().c_str());
                continue;
            }
            abstractItem.AddMember("id", rapidjson::Value(conv->GetId().c_str(), alloc), alloc);
            // 与 qaList 保持一致，使用 "content" 字段存储摘要内容
            rapidjson::Value contentCopy;
            contentCopy.CopyFrom(abstractDoc, alloc);
            abstractItem.AddMember("content", contentCopy, alloc);
            abstractQA.PushBack(abstractItem.Move(), alloc);
        }
    }
    LOG_INFO("ProcessHistoryData: qaList size=%d, abstractQA size=%d", qaList.Size(), abstractQA.Size());
}

void DMContext::ContextService::QueryHistoryData(const QAFilter& qaFilter, int32_t limit,
                                                 std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    UserConversationFilter filter;
    filter.userId = qaFilter.userId;
    filter.contextId = qaFilter.ctxId;

    bool ret = ContextDbClient::GetInstance().QueryConversations(
            qaFilter.userId, qaFilter.ctxId, "", limit, "active", conversations);
    if (!ret) {
        LOG_ERR("QueryHistoryData: QueryConversations failed");
        return;
    }
    std::sort(conversations.begin(), conversations.end(),
              [](const std::shared_ptr<UserConversation>& a, const std::shared_ptr<UserConversation>& b) {
                  return a->GetCreateDate() < b->GetCreateDate();
              });
    auto agingPeriod = ConfigMgr::GetInstance()->GetCommonParamsByKey("aging_period");
    int age = std::atoi(agingPeriod.c_str());
    LOG_INFO("QueryHistoryData agingPeriod %s, age:%d", agingPeriod.c_str(), age);
    conversations.erase(
            std::remove_if(conversations.begin(), conversations.end(),
                           [age](const std::shared_ptr<UserConversation>& conv) {
                               bool isTimeout = DMContext::IsTimeout(conv->GetCreateDate(), age);
                               if (isTimeout) {
                                   LOG_INFO("QueryHistoryData Filter out obsolete data %s", conv->GetCreateDate().c_str());
                               }
                               return isTimeout;
                           }),
            conversations.end());
}

std::string DMContext::ContextService::BuildMemoryStr(int32_t memoryCount, int32_t memoryTokenBudget,
                                                      const std::vector<std::string>& longMem)
{
    std::string memoryStr;
    if (memoryCount > 0) {
        // 使用 memory_count 控制记忆条数
        int32_t count = 0;
        for (const auto& mem : longMem) {
            if (count >= memoryCount) {
                break;
            }
            memoryStr += mem;
            count++;
        }
        LOG_INFO("BuildMemoryStr: using memoryCount, count=%d, memoryCount=%d", count, memoryCount);
    } else if (memoryTokenBudget > 0) {
        // 使用 memory_token_budget 控制字符数
        int32_t totalChars = 0;
        for (const auto& mem : longMem) {
            if (totalChars + (int32_t)mem.size() > memoryTokenBudget) {
                break;
            }
            memoryStr += mem;
            totalChars += mem.size();
        }
        LOG_INFO("BuildMemoryStr: using memoryTokenBudget, chars=%d, budget=%d", totalChars, memoryTokenBudget);
    } else {
        // 默认使用全部记忆
        for (const auto& mem : longMem) {
            memoryStr += mem;
        }
    }
    return memoryStr;
}

// ============ AddContextNew 相关实现 ============

bool DMContext::ContextService::ParseWriteParams(const rapidjson::Document& doc, WriteParams& params)
{
    if (!doc.HasMember("userId") || !doc["userId"].IsString()) {
        LOG_ERR("ParseWriteParams: missing userId");
        return false;
    }
    params.userId = doc["userId"].GetString();

    // 新增：serviceId 用于与 userId 组合形成新的 userId
    params.srvId = doc.HasMember("service_id") && doc["service_id"].IsString()
                   ? doc["service_id"].GetString() : "";

    if (!doc.HasMember("context_id") || !doc["context_id"].IsString()) {
        LOG_ERR("ParseWriteParams: missing context_id");
        return false;
    }
    params.contextId = doc["context_id"].GetString();

    params.agentRole = doc.HasMember("agent_role") && doc["agent_role"].IsString()
                       ? doc["agent_role"].GetString() : "";

    // 解析 biometric_identity 中的 voiceprint_id 和 face_id
    if (doc.HasMember("biometric_identity") && doc["biometric_identity"].IsObject()) {
        const auto& bio = doc["biometric_identity"];
        params.voiceprintId = bio.HasMember("voiceprint_id") && bio["voiceprint_id"].IsString()
                              ? bio["voiceprint_id"].GetString() : "";
        params.faceId = bio.HasMember("face_id") && bio["face_id"].IsString()
                        ? bio["face_id"].GetString() : "";
    }

    // task_status/taskstatus: Completed 表示完整QA，其他值表示不完整
    if (doc.HasMember("task_status") && doc["task_status"].IsString()) {
        params.taskStatus = doc["task_status"].GetString();
    } else if (doc.HasMember("taskstatus") && doc["taskstatus"].IsString()) {
        params.taskStatus = doc["taskstatus"].GetString();
    } else {
        params.taskStatus = "Completed";
    }

    // enable_memory: 是否开启长期记忆检索，默认true
    if (doc.HasMember("enable_memory") && doc["enable_memory"].IsBool()) {
        params.enableMemory = doc["enable_memory"].GetBool();
    } else {
        params.enableMemory = true;
    }

    params.opType =  doc.HasMember("opType") && doc["opType"].IsString()
        ? doc["opType"].GetString() : "";

    params.uuid =  doc.HasMember("uuid") && doc["uuid"].IsString()
                    ? doc["uuid"].GetString() : "";

    // 处理 srvId + userId 组合形成新的 userId
    if (!params.srvId.empty()) {
        params.userId = params.srvId + "_" + params.userId;
    }

    // ignore_this_round: 是否忽略本轮处理，默认false
    params.ignoreThisRound = doc.HasMember("ignore_this_round") && doc["ignore_this_round"].IsBool()
                             ? doc["ignore_this_round"].GetBool() : false;

    return true;
}

// 创建 UserConversation 对象的辅助函数，减少代码重复
std::shared_ptr<UserConversation> DMContext::ContextService::CreateUserConversation(
        const WriteParams& params, const std::string& role,
        const std::string& content, const std::string& metaData, const std::string& time)
{
    auto conv = std::make_shared<UserConversation>();
    conv->SetId(!params.uuid.empty() ? params.uuid : std::to_string(Snowflake::GetInstance()->Generate()));
    conv->SetUserId(params.userId);
    conv->SetContextId(params.contextId);
    conv->SetAgentRole(params.agentRole);
    conv->SetVoiceprintId(params.voiceprintId);
    conv->SetFaceId(params.faceId);
    conv->SetRole(role);
    conv->SetContent(content);
    conv->SetLabel("");
    conv->SetCreateDate(time);
    conv->SetUpdateDate(time);
    conv->SetMetaData(metaData);
    conv->SetEnableMemory(params.enableMemory);
    if (params.taskStatus == "Completed") {
        conv->SetContextStatus("active");
    } else {
        conv->SetContextStatus("inactive");
    }
    return conv;
}

bool DMContext::ContextService::ParseMessagesToConversations(const rapidjson::Document& doc,
                                                             const WriteParams& params,
                                                             std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    if (!doc.HasMember("messages")) {
        LOG_ERR("ParseMessagesToConversations: missing messages");
        return false;
    }

    const auto& messages = doc["messages"];
    std::string time = GetCurrentTime();

    // 支持两种格式：
    // 1. 对象格式（新API）: {"messages": {"human": "...", "ai": "..."}}
    // 2. 数组格式（旧API）: {"messages": [{"user": "...", "assistant": [...]}, ...]}
    if (messages.IsObject()) {
        // 新版API格式: {"human": "...", "ai": "..."}
        std::string humanContent;
        std::string aiContent;

        if (messages.HasMember("human") && messages["human"].IsString()) {
            humanContent = messages["human"].GetString();
        }
        if (messages.HasMember("ai") && messages["ai"].IsString()) {
            aiContent = messages["ai"].GetString();
        }

        // 创建用户消息 (human)
        if (!humanContent.empty()) {
            auto userConv = CreateUserConversation(params, "user", humanContent, "", time);
            conversations.push_back(userConv);
        }

        // 创建助手消息 (ai)
        if (!aiContent.empty()) {
            auto assConv = CreateUserConversation(params, "assistant", aiContent, "", time);
            conversations.push_back(assConv);
        }
    } else if (messages.IsArray()) {
        // 旧版API格式: [{"user": "...", "assistant": [...]}, ...]
        for (auto& msg : messages.GetArray()) {
            if (!msg.IsObject()) {
                continue;
            }

            // 新版API格式: [{"human": "...", "ai": "..."}]
            std::string humanContent;
            std::string aiContent;

            if (msg.HasMember("human") && msg["human"].IsString()) {
                humanContent = msg["human"].GetString();
            }
            if (msg.HasMember("ai") && msg["ai"].IsString()) {
                aiContent = msg["ai"].GetString();
            }

            // 创建用户消息 (human)
            if (!humanContent.empty()) {
                auto userConv = CreateUserConversation(params, "user", humanContent, "", time);
                conversations.push_back(userConv);
            }

            // 创建助手消息 (ai)
            if (!aiContent.empty()) {
                auto assConv = CreateUserConversation(params, "assistant", aiContent, "", time);
                conversations.push_back(assConv);
            }
        }
    } else {
        LOG_ERR("ParseMessagesToConversations: messages is not an object or array");
        return false;
    }

    return true;
}

std::vector<std::shared_ptr<UserConversation>> DMContext::ContextService::FilterValidConversations(
        const std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    std::vector<std::shared_ptr<UserConversation>> validConvs;
    for (size_t i = 0; i < conversations.size(); ++i) {
        const auto& conv = conversations[i];
        // user 消息必须有 answer（下一条 assistant 的 content）
        if (conv->GetRole() == "user" && i + 1 < conversations.size()) {
            const auto& nextConv = conversations[i + 1];
            if (nextConv->GetRole() == "assistant" && !nextConv->GetContent().empty()) {
                // 生成 history（正确的JSON数组格式）
                Json::Value historyArray(Json::arrayValue);
                Json::Value userMsg;
                userMsg["role"] = "user";
                userMsg["content"] = conv->GetContent();
                Json::Value assistantMsg;
                assistantMsg["role"] = "assistant";
                assistantMsg["content"] = nextConv->GetContent();
                historyArray.append(userMsg);
                historyArray.append(assistantMsg);
                Json::StreamWriterBuilder writer;
                writer.settings_["jsonObjectEscape"] = false;
                writer.settings_["emitUTF8"] = true;
                std::string history = Json::writeString(writer, historyArray);
                // history 字段也存储相同的内容（原始QA对）
                conv->SetHistory(history);
                conv->SetAbstractQA(history);
                validConvs.push_back(conv);
            }
        }
    }
    return validConvs;
}

bool DMContext::ContextService::SaveToDatabase(const std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    if (conversations.empty()) {
        LOG_INFO("SaveToDatabase: no valid conversations to save");
        return true;
    }

    LOG_INFO("SaveToDatabase: writing data via ContextDbClient");
    bool ret = ContextDbClient::GetInstance().WriteConversations(conversations);
    if (!ret) {
        LOG_ERR("SaveToDatabase: WriteConversations failed");
        return false;
    }
    LOG_INFO("SaveToDatabase: successfully saved %u conversations", conversations.size());
    return true;
}

void DMContext::ContextService::BuildWriteResponse(std::string& responseJson, int code, std::string& message)
{
    Json::Value root;
    root["code"] = code;
    root["message"] = message;

    Json::StreamWriterBuilder writerBuilder;
    responseJson = Json::writeString(writerBuilder, root);
}

void DMContext::ContextService::AsyncGenerateSummary(const std::vector<std::shared_ptr<UserConversation>>& conversations, WriteParams params)
{
    auto flagMutex = std::make_shared<std::shared_mutex>();
    auto flag = std::make_shared<bool>(false);
    LOG_INFO("AddContextNew: start async AsyncGenerateSummary");
    m_ThreadPool->Submit([conversations, params, flagMutex, flag]() {
        auto start_time = std::chrono::system_clock::now();
        summaryAbstractQAContext(conversations);
        // 更新数据库中的 abstract_qa、label 和 abstract 字段
        for (auto &conv : conversations) {
            bool ret = ContextDbClient::GetInstance().UpdateConversation(conv);
            if (!ret) {
                LOG_ERR("AsyncGenerateSummary: UpdateConversation failed");
            }
        }
        auto duration_total = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - start_time);
        LOG_INFO("[DMContext TIME-CONSUMING] AsyncGenerateSummary summaryAbstractQAContext total %d ms",
            duration_total.count());
        buildAndWriteWriteLog(params.userId, params.srvId, params.contextId, params.uuid, "", "", "", std::to_string(duration_total.count()),"", "", "", true);
    });

    m_ThreadPool->Submit([conversations, params, flagMutex, flag]() {
        auto start_time = std::chrono::system_clock::now();
        summaryAbstractContext(conversations);
        // 更新数据库中的 abstract_qa、label 和 abstract 字段
        for (auto &conv : conversations) {
            bool ret = ContextDbClient::GetInstance().UpdateConversation(conv);
            if (!ret) {
                LOG_ERR("AsyncGenerateSummary: UpdateConversation failed");
            }
        }
        auto duration_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - start_time);
        LOG_INFO("[DMContext TIME-CONSUMING] AsyncGenerateSummary summaryAbstractContext total %d ms",
            duration_total.count());
        buildAndWriteWriteLog(params.userId, params.srvId, params.contextId, params.uuid, "", "", "", "", std::to_string(duration_total.count()), "", "", true);
    });
}

std::vector<std::shared_ptr<UserConversation>> DMContext::ContextService::GetConversations(
        const std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    // 不完整QA：直接写入所有数据，不过滤
    // 但仍需要设置 history 字段
    std::vector<std::shared_ptr<UserConversation>> validConvs;
    for (const auto& conv : conversations) {
        Json::Value historyArray(Json::arrayValue);
        Json::Value msg;
        msg["role"] = conv->GetRole();
        msg["content"] = conv->GetContent();
        historyArray.append(msg);
        Json::StreamWriterBuilder writer;
        writer.settings_["jsonObjectEscape"] = false;
        writer.settings_["emitUTF8"] = true;
        std::string history = Json::writeString(writer, historyArray);
        conv->SetHistory(history);
        conv->SetAbstractQA(history);
        validConvs.push_back(conv);
    }
    return validConvs;
}

static bool CheckMessagesHasHumanOnly(const std::string& body)
{
    Document bodyDoc;
    if (bodyDoc.Parse(body.c_str()).HasParseError()) {
        return false;
    }
    if (!bodyDoc.HasMember("messages")) {
        return false;
    }
    const auto& messages = bodyDoc["messages"];
    if (messages.IsArray()) {
        for (const auto& msg : messages.GetArray()) {
            std::string human = GetStringField(msg, "human");
            std::string ai = GetStringField(msg, "ai");
            if (!human.empty() && ai.empty()) {
                return true;
            }
        }
    } else if (messages.IsObject()) {
        std::string human = GetStringField(messages, "human");
        std::string ai = GetStringField(messages, "ai");
        if (!human.empty() && ai.empty()) {
            return true;
        }
    }
    return false;
}

void DMContext::ContextService::AddContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                              std::string opType)
{
    auto begin = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    auto body = context->GetRequestBody();
    LOG_INFO("AddContextNew: body:%s", body.c_str());
    std::string responseJson;
    std::string message;
    CMFrm::COM::HttpStatus status = CMFrm::COM::HttpStatus::HTTP200;
    
    if (CheckMessagesHasHumanOnly(body)) {
        LOG_INFO("AddContextNew: messages contains human only, return directly");
        status = CMFrm::COM::HttpStatus::HTTP200;
        message = "success";
        BuildWriteResponse(responseJson, 200, message);
        HttpHelper::WriteResponse(context, responseJson, status);
        return;
    }
    
    auto res = ProcessMessage(body);
    LOG_INFO("AddContextNew: ProcessMessage res:%s", res.c_str());
    Document doc;
    if (doc.Parse(res.c_str()).HasParseError()) {
        message = "服务内部异常";
        BuildWriteResponse(responseJson, 500, message);
        status = CMFrm::COM::HttpStatus::HTTP400;
        HttpHelper::WriteResponse(context, responseJson, status);
        return;
    }

    std::string userId = doc["userId"].GetString();
    std::string serviceId = doc["service_id"].GetString();
    std::string contextId = doc["context_id"].GetString();
    std::string errorMsg = doc["error_msg"].GetString();
    std::string uuId = "";
    if (doc.HasMember("uu_id") && doc["uu_id"].IsString()) {
        uuId = doc["uu_id"].GetString();
    }

    if (errorMsg != "success") {
        BuildWriteResponse(responseJson, 400, errorMsg);
        status = CMFrm::COM::HttpStatus::HTTP400;
    } else {
        status = CMFrm::COM::HttpStatus::HTTP200;
        message = "success";
        BuildWriteResponse(responseJson, 200, message);
    }

    HttpHelper::WriteResponse(context, responseJson, status);

    auto end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto duration = end - begin;
    buildAndWriteWriteLog(serviceId + "_" +userId, serviceId, contextId, uuId, body, responseJson, std::to_string(g_turn_timestamp), "", "", std::to_string(begin), std::to_string(duration), true);
    std::string input  = "===========\nAddContextNew body\n" + body + "\nResponse:\n" + responseJson;
    WriteModelLogX(serviceId + "_" + userId, contextId + ".log", input);
}

void DMContext::buildAndWriteWriteLog(const std::string userId,
    const std::string serviceId,
    const std::string contextId,
    const std::string uuId,
    const std::string body,
    const std::string responseJson,
    const std::string turn,
    const std::string qaExtractDelay,
    const std::string qaSummaryDelay,
    const std::string ts,
    const std::string delay,
    bool  flag
    )
{
    LOG_INFO("buildAndWriteQueryLog begin, user_id: %s, service_id: %s, context_id: %s, turnTimes: %s, ts: %s, flag: %s",userId.c_str(), serviceId.c_str(), contextId.c_str(), turn.c_str(), ts.c_str(), flag ? "true" : "false");
    WriteApiRequest writeRequest{};
    ParseWriteApiRequest(body, writeRequest);

    WriteApiResponse writeResponse{};
    ParseWriteApiResponse(responseJson, writeResponse);

    WriteApiLog WriteLog{};
    WriteLog.requestBody = writeRequest;
    WriteLog.response = writeResponse;
    WriteLog.turn = turn;
    WriteLog.uuId = uuId;
    WriteLog.qaExtractDelay = qaExtractDelay;
    WriteLog.qaSummaryDelay = qaSummaryDelay;
    WriteLog.ts = ts;
    WriteLog.delay = delay;

    ModelLogUtil::WriteWriteApiLog(userId, serviceId, contextId, WriteLog, flag);

}

bool DMContext::ParseWriteApiRequest(const std::string& jsonStr, WriteApiRequest& request) {
    rapidjson::Document doc;
    doc.Parse(jsonStr.c_str());

    if (doc.HasParseError()) {
        return false;
    }

    if (!doc.IsObject()) {
        return false;
    }

    // 解析userId
    if (doc.HasMember("userId") && doc["userId"].IsString()) {
        request.userId = doc["userId"].GetString();
    }

    // 解析service_id
    if (doc.HasMember("service_id") && doc["service_id"].IsString()) {
        request.serviceId = doc["service_id"].GetString();
    }

    // 解析messages
    if (doc.HasMember("messages") && doc["messages"].IsArray()) {
        const rapidjson::Value& messagesArray = doc["messages"];
        for (const auto& messageItem : messagesArray.GetArray()) {
            if (!messageItem.IsObject()) {
                continue;
            }

            Message message;
            if (messageItem.HasMember("human") && messageItem["human"].IsString()) {
                message.human = messageItem["human"].GetString();
            }

            if (messageItem.HasMember("ai") && messageItem["ai"].IsString()) {
                message.ai = messageItem["ai"].GetString();
            }

            request.messages.push_back(message);
        }
    }

    // 解析context_id
    if (doc.HasMember("context_id") && doc["context_id"].IsString()) {
        request.contextId = doc["context_id"].GetString();
    }

    // 解析biometric_identity
    if (doc.HasMember("biometric_identity") && doc["biometric_identity"].IsObject()) {
        if (!ParseBiometricIdentity(doc["biometric_identity"], request.biometricIdentity)) {
            LOG_INFO("biometric_identity parse fail");
        }
    }

    // 解析agent_role
    if (doc.HasMember("agent_role") && doc["agent_role"].IsString()) {
        request.agentRole = doc["agent_role"].GetString();
    }

    return true;
}

bool DMContext::ParseBiometricIdentity(const rapidjson::Value& biometricIdentityJson, BiometricIdentity& biometricIdentity) {
    if (!biometricIdentityJson.IsObject()) {
        return false;
    }

    // 解析voiceprint_id
    if (biometricIdentityJson.HasMember("voiceprint_id") && biometricIdentityJson["voiceprint_id"].IsString()) {
        biometricIdentity.voiceprintId = biometricIdentityJson["voiceprint_id"].GetString();
    }

    // 解析face_id
    if (biometricIdentityJson.HasMember("face_id") && biometricIdentityJson["face_id"].IsString()) {
        biometricIdentity.faceId = biometricIdentityJson["face_id"].GetString();
    }

    return true;
}


bool DMContext::ParseWriteApiResponse(const std::string& jsonStr, WriteApiResponse& response) {
    rapidjson::Document doc;
    doc.Parse(jsonStr.c_str());

    // 检查解析是否成功
    if (doc.HasParseError()) {
        return false;
    }

    // 检查是否为对象类型
    if (!doc.IsObject()) {
        return false;
    }

    // 解析code字段
    if (doc.HasMember("code") && doc["code"].IsInt()) {
        response.code = doc["code"].GetInt();
    } else {
        return false;
    }

    // 解析msg字段
    if (doc.HasMember("msg") && doc["msg"].IsString()) {
        response.msg = doc["msg"].GetString();
    } else {
        return false;
    }

    return true;
}

void DMContext::ContextService::ProcessQACompleteCallback(const std::string& body)
{
    LOG_INFO("ProcessQACompleteCallback: body:%s", body.c_str());
    // 1. 解析请求参数
    rapidjson::Document doc(rapidjson::kObjectType);
    if (!JsonParser::Parse(body.c_str(), doc)) {
        LOG_INFO("ProcessQACompleteCallback: Parse request body failed");
        return;
    }

    WriteParams params;
    if (!ParseWriteParams(doc, params)) {
        LOG_INFO("ProcessQACompleteCallback: missing required fields userId:%s", params.userId.c_str());
        return;
    }

    if (params.ignoreThisRound) {
        LOG_INFO("ProcessQACompleteCallback userId:%s, uuid:%s ignore_this_round is true, skip processing",
                 params.userId.c_str(), params.uuid.c_str());
        return;
    }

    LOG_INFO("ProcessQACompleteCallback userId:%s, uuid:%s", params.userId.c_str(), params.uuid.c_str());
    // 2. 解析 messages 转换为 conversations（统一接口，支持对象和数组两种格式）
    std::vector<std::shared_ptr<UserConversation>> conversations;
    if (!ParseMessagesToConversations(doc, params, conversations)) {
        LOG_INFO("ProcessQACompleteCallback: parse messages failed userId:%s", params.userId.c_str());
        return;
    }

    std::vector<std::shared_ptr<UserConversation>> validConversations;
    // 完整QA：过滤有效消息
    validConversations = FilterValidConversations(conversations);
    if (validConversations.empty()) {
        LOG_INFO("ProcessQACompleteCallback: no valid conversations userId:%s", params.userId.c_str());
        validConversations = GetConversations(conversations);
    }
    LOG_INFO("ProcessQACompleteCallback: validConversations size: %zu", validConversations.size());

    LOG_INFO("ProcessQACompleteCallback opType:%s userId:%s", params.opType.c_str(), params.userId.c_str());
    if (params.opType == "Completed") {
        // 异步生成摘要
        AsyncGenerateSummary(validConversations, params);
    } else if (params.opType == "write") {
        // 4. 写入缓存（每500ms批量写入数据库）
        //AddToWriteCache(validConversations);  //存在问题，先注释
        SaveToDatabase(validConversations);
    } else if (params.opType == "update") {
        for (const auto& conv : validConversations) {
            bool ret = ContextDbClient::GetInstance().UpdateConversation(conv);
            if (!ret) {
                LOG_ERR("ProcessQACompleteCallback: UpdateConversation failed userId:%s", params.userId.c_str());
            }
        }
        
    } else {
        LOG_INFO("ProcessQACompleteCallback unknown opType:%s userId:%s", params.opType.c_str(), params.userId.c_str());
    }
}

void DMContext::ContextService::DeleteContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    auto begin = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto body = context->GetRequestBody();
    LOG_INFO("DeleteContextNew body:%s", body.c_str());

    rapidjson::Document doc(rapidjson::kObjectType);
    if (!JsonParser::Parse(body.c_str(), doc)) {
        HttpHelper::WriteResponse(context, "Parse request body failed", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }

    std::string userId;
    std::string srvId;  // 新增：服务ID
    std::string contextId;

    // 新旧API统一使用 userId
    if (!doc.HasMember("userId") || !doc["userId"].IsString()) {
        HttpHelper::WriteResponse(context, "missing userId", CMFrm::COM::HttpStatus::HTTP400);
        return;
    }
    userId = doc["userId"].GetString();

    // 解析 service_id 参数
    srvId = doc.HasMember("service_id") && doc["service_id"].IsString()
            ? doc["service_id"].GetString() : "";

    // srvId + userId 组合形成新的 userId
    if (!srvId.empty()) {
        userId = srvId + "_" + userId;
    }
    LOG_INFO("DeleteContextNew userId:%s", userId.c_str());
    contextId = doc.HasMember("context_id") && doc["context_id"].IsString()
                ? doc["context_id"].GetString() : "";

    // 逻辑删除：将 context_status 修改为 inactive
    LOG_INFO("DeleteContextNew: updating context status via ContextDbClient");
    bool ret = ContextDbClient::GetInstance().UpdateContextStatus(userId, contextId, "inactive");
    if (!ret) {
        LOG_ERR("DeleteContextNew: UpdateContextStatus failed");
    }

    std::string input  = "=========\nDeleteContextNew body\n" + body;
    WriteModelLogX(userId, contextId + ".log", input);

    auto end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto duration = end - begin;
    buildAndWriteDeleteLog(userId, srvId, contextId, ret, std::to_string(g_turn_timestamp), std::to_string(begin), std::to_string(duration), true);
    if (!ret) {
        HttpHelper::WriteResponse(context, "delete failed", CMFrm::COM::HttpStatus::HTTP500);
        return;
    }

    HttpHelper::WriteResponse(context, "{\"code\":0,\"message\":\"success\"}", CMFrm::COM::HttpStatus::HTTP200);
}

void DMContext::buildAndWriteDeleteLog(const std::string userId,
    const std::string serviceId,
    const std::string contextId,
    bool ret,
    std::string turn,     // 对话轮次，用于标识同一轮对话
    std::string ts,
    std::string delay,
    bool  flag
    )
{
    LOG_INFO("buildAndWriteQueryLog begin, user_id: %s, service_id: %s, context_id: %s, turnTimes: %s, ts: %s, flag: %s",userId.c_str(), serviceId.c_str(), contextId.c_str(), turn.c_str(), ts.c_str(), flag ? "true" : "false");
    DeleteApiRequest deleteRequest {
        .userId =  userId,
        .serviceId =  serviceId,
        .contextId =  contextId,
    };

    DeleteApiResponse deleteResponse{};
    if (ret) {
        deleteResponse.code = 0;
        deleteResponse.msg = "success";
    } else {
        deleteResponse.code = 1;
        deleteResponse.msg = "failed";
    }

    DeleteApiLog deleteLog{};
    deleteLog.requestBody = deleteRequest;
    deleteLog.response = deleteResponse;
    deleteLog.turn = turn;
    deleteLog.ts = ts;
    deleteLog.delay = delay;

    ModelLogUtil::WriteDeleteApiLog(userId, serviceId, contextId, deleteLog, flag);

}

// 新增：处理记忆并返回结果
void DMContext::ContextService::ProcessMemory(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        const std::string& responseId,
        const long begin)
{
    LOG_INFO("ProcessMemory responseId:%s", responseId.c_str());
    if (!TryAcquireAddingStatus(responseId)) {
        return;
    }
    m_ThreadPool->Submit([this, params, responseId, rewrittenQuery, begin]() {
        auto start_time = std::chrono::system_clock::now();
        try {
            std::vector<std::string> longMem;
            std::vector<std::string> ids;

            RetrieveMemory(params, rewrittenQuery, longMem);

            auto duration_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - start_time);

            AddMem(responseId, params.content, rewrittenQuery, ids, longMem, duration_total.count());

            std::string memoryResult = std::accumulate(longMem.begin(), longMem.end(), std::string(""));
            rapidjson::Value responseDoc;
            std::shared_lock<std::shared_mutex> ilock(m_processMemoryMutex);
            buildAndWriteQueryLog(params, nullptr, nullptr, nullptr, "", "", "", rewrittenQuery, memoryResult,
                    std::to_string(duration_total.count()), std::to_string(g_turn_timestamp), std::to_string(begin),
                    "", "", true);
            LOG_INFO("[DMContext TIME-CONSUMING] ProcessMemory total %d ms", duration_total.count());
        } catch (...) {
            LOG_ERR("ProcessMemory exception");
        }

        ReleaseAddingStatus(responseId);
    });
}

bool DMContext::ContextService::TryAcquireAddingStatus(const std::string& responseId)
{
    std::unique_lock<std::mutex> map_lock(_map_mtx);
    auto& status = _adding_map[responseId];
    map_lock.unlock();

    std::unique_lock<std::mutex> status_lock(status.mtx);
    if (status.is_adding) {
        return false;
    }
    status.is_adding = true;
    return true;
}

void DMContext::ContextService::ReleaseAddingStatus(const std::string& responseId)
{
    std::unique_lock<std::mutex> map_lock(_map_mtx);
    auto it = _adding_map.find(responseId);
    if (it == _adding_map.end()) {
        return;
    }
    auto& status = it->second;
    map_lock.unlock();

    std::unique_lock<std::mutex> status_lock(status.mtx);
    status.is_adding = false;
    status.cv.notify_all();
}

void DMContext::ContextService::RetrieveMemory(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        std::vector<std::string>& longMem)
{
    bool useLightRetrieval = (params.mode == "fast");
    if (useLightRetrieval) {
        RetrieveMemoryByLightMode(params, rewrittenQuery, longMem);
    } else {
        RetrieveMemoryByNormalMode(params, rewrittenQuery, longMem);
    }
}

void DMContext::ContextService::RetrieveMemoryByLightMode(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        std::vector<std::string>& longMem)
{
    std::string kmmUrl = ContextDbClient::GetInstance().GetKMMUrl("/kmm/v1/user/memory/light_retrieval");
    if (kmmUrl.empty()) {
        LOG_ERR("GetKMMUrl failed for light retrieval, url is empty");
        return;
    }

    LightRetrievalMem(kmmUrl, params.userId, rewrittenQuery, longMem);
    LOG_INFO("ProcessMemory: using light retrieval for userId=%s, query=%s",
             params.userId.c_str(), rewrittenQuery.c_str());
}

void DMContext::ContextService::RetrieveMemoryByNormalMode(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        std::vector<std::string>& longMem)
{
    std::string kmmUrl = ContextDbClient::GetInstance().GetKMMUrl("/kmm/v1/user/memories/get");
    if (kmmUrl.empty()) {
        LOG_ERR("GetKMMUrl failed, url is empty");
        return;
    }

    GetLongMem(kmmUrl, params.userId, rewrittenQuery, longMem);
    LOG_INFO("ProcessMemory: using normal retrieval for userId=%s, query=%s",
             params.userId.c_str(), rewrittenQuery.c_str());
}

std::string BuildProcessMessageBody(const DMContext::ContextService::QueryParams& params, const std::string& rewrittenQuery, bool isForgetMemory)
{
    rapidjson::Document bodyDoc(rapidjson::kObjectType);
    auto& bodyAlloc = bodyDoc.GetAllocator();

    if (!params.originalUserId.empty()) {
        bodyDoc.AddMember("userId", rapidjson::Value(params.originalUserId.c_str(), params.originalUserId.length()), bodyAlloc);
    }
    if (!params.srvId.empty()) {
        bodyDoc.AddMember("service_id", rapidjson::Value(params.srvId.c_str(), params.srvId.length()), bodyAlloc);
    }
    if (!params.contextId.empty()) {
        bodyDoc.AddMember("context_id", rapidjson::Value(params.contextId.c_str(), params.contextId.length()), bodyAlloc);
    }

    rapidjson::Value messages(rapidjson::kArrayType);
    rapidjson::Value msg(rapidjson::kObjectType);
    std::string humanContent = rewrittenQuery.empty() ? params.content : rewrittenQuery;
    if (!humanContent.empty()) {
        msg.AddMember("human", rapidjson::Value(humanContent.c_str(), humanContent.length()), bodyAlloc);
        messages.PushBack(msg, bodyAlloc);
    }
    if (!messages.Empty()) {
        bodyDoc.AddMember("messages", messages, bodyAlloc);
    }

    if (!params.agentRole.empty()) {
        bodyDoc.AddMember("agent_role", rapidjson::Value(params.agentRole.c_str(), params.agentRole.length()), bodyAlloc);
    }

    if (!params.voiceprintId.empty() || !params.faceId.empty()) {
        rapidjson::Value biometricIdentity(rapidjson::kObjectType);
        if (!params.voiceprintId.empty()) {
            biometricIdentity.AddMember("voiceprint_id", rapidjson::Value(params.voiceprintId.c_str(), params.voiceprintId.length()), bodyAlloc);
        }
        if (!params.faceId.empty()) {
            biometricIdentity.AddMember("face_id", rapidjson::Value(params.faceId.c_str(), params.faceId.length()), bodyAlloc);
        }
        bodyDoc.AddMember("biometric_identity", biometricIdentity, bodyAlloc);
    }

    if (isForgetMemory) {
        bodyDoc.AddMember("ignore_this_round", rapidjson::Value(true), bodyAlloc);
    } else {
        bodyDoc.AddMember("ignore_this_round", rapidjson::Value(false), bodyAlloc);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    bodyDoc.Accept(writer);
    return buffer.GetString();
}

// 新增：执行第二次查询的完整流程
void DMContext::ContextService::ExecuteSecondQuery(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
        const QueryParams& params,
        const QAFilter& qaFilter,
        const std::chrono::system_clock::time_point& start_GetContext)
{
    // 使用 Document 的 allocator，确保生命周期与 doc 一致
    rapidjson::Document doc(rapidjson::kObjectType);
    auto alloc = doc.GetAllocator();

    rapidjson::Value qaList(rapidjson::kArrayType);
    rapidjson::Value abstractQA(rapidjson::kArrayType);

    // 查询历史对话数据
    auto startQueryHistory = std::chrono::system_clock::now();
    std::vector<std::shared_ptr<UserConversation>> conversations;

    // 从 ContextCache 获取缓存的 rewrittenQuery
    std::string rewriteQ;
    auto ctxCache = GetContextCache(params.responseId);
    if (ctxCache != nullptr) {
        rewriteQ = ctxCache->rewrittenQuery;
        conversations = ctxCache->conversations;
        RemoveContextCache(params.responseId);
    } else {
        int limit = ContextService::GetQueryConfig().maxDbQueryTurns;
        QueryHistoryData(qaFilter, limit, conversations);
    }
    auto endQueryHistory = std::chrono::system_clock::now();
    auto durationDbQuery = std::chrono::duration_cast<std::chrono::milliseconds>(
            endQueryHistory - startQueryHistory).count();

    ProcessHistoryData(conversations, qaList, abstractQA, qaFilter.query, alloc);

    // 构建记忆字符串：根据 memoryCount 或 memoryTokenBudget 控制记忆内容
    std::string memory;
    std::string memoryDurationStr;
    auto memCache = FindMem(params.responseId, params.content);
    if (memCache != nullptr) {
        memory = BuildMemoryStr(params.memoryCount, params.memoryTokenBudget, memCache->longMem);
        memoryDurationStr = std::to_string(memCache->duration_total);
    }

    int32_t memoryTokenCount = EstimateTokens(memory);

    //    组装
    auto body = context->GetRequestBody();
    HistoryBuildOptions opts;
    opts.conversationTurns = params.conversationTurns;
    opts.tokenBudget = params.tokenBudget;
    opts.newestFirst = false;
    opts.useMerge = true;
    opts.currentUserQuery = params.content;  // 合并段的 human 字段

    HistoryResultMeta meta;
    meta.responseId = params.responseId;
    meta.memory = memory;
    meta.memoryTokenCount = memoryTokenCount;
    meta.rewrittenQuery = rewriteQ;
    std::string responseJson = BuildHistoryResult(qaList, abstractQA, opts, meta, alloc);
    rapidjson::Value responseDoc = BuildHistoryResultForJson(qaList, abstractQA, opts, meta, alloc);

    HttpHelper::WriteResponse(context, responseJson, CMFrm::COM::HttpStatus::HTTP200);

    // 写入调试信息到文件
    std::string HistoryData = DumpHistoryData(qaList, abstractQA);
    auto endTimePoint = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimePoint - start_GetContext);
    std::string duration_str = std::to_string(duration.count());
    buildAndWriteQueryLog(params, &responseDoc, &abstractQA, &qaList, "", "", "",
            "", "", "", std::to_string(g_turn_timestamp),
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    start_GetContext.time_since_epoch()).count()),
            std::to_string(durationDbQuery), duration_str, true);
    std::string input = "=========\nSecondRequest\n" + body + "\nSecondQueryRes\n" + responseJson +
            "\nHistoryData\n" + HistoryData + "\nmemory_duration:" + memoryDurationStr +
            "\nsecondQueryDuration:" + duration_str + "\ndbQueryDuration:" + std::to_string(durationDbQuery);
    WriteModelLogX(params.userId, params.contextId + ".log", input);
}

// 新增：执行第一次查询的完整流程
void DMContext::ContextService::ExecuteFirstQuery(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
        const QueryParams& params,
        const QAFilter& qaFilter,
        std::string& responseId,
        const std::chrono::system_clock::time_point& start_GetContext)
{
    g_turn_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            start_GetContext.time_since_epoch()).count();
    // 使用 Document 的 allocator，确保生命周期与 doc 一致
    rapidjson::Document doc(rapidjson::kObjectType);
    auto alloc = doc.GetAllocator();

    rapidjson::Value qaList(rapidjson::kArrayType);
    rapidjson::Value abstractQA(rapidjson::kArrayType);

    // 查询历史对话数据
    auto startQueryHistory = std::chrono::system_clock::now();

    std::vector<std::shared_ptr<UserConversation>> conversations;
    int limit = ContextService::GetQueryConfig().maxDbQueryTurns;
    QueryHistoryData(qaFilter, limit, conversations);
    auto endQueryHistory = std::chrono::system_clock::now();
    auto durationDbQuery = std::chrono::duration_cast<std::chrono::milliseconds>(
            endQueryHistory - startQueryHistory);
    LOG_INFO("[DMContext TIME-CONSUMING] QueryHistoryData %d ms", durationDbQuery.count());

    ProcessHistoryData(conversations, qaList, abstractQA, qaFilter.query, alloc);

    //改写
    std::string rewriteInput;
    std::string rewrittenQuery = params.isForgetMemory ? "已处理完成。" : params.content;
    std::chrono::system_clock::time_point start_RewriteQuery;
    std::chrono::system_clock::time_point end_RewriteQuery;
    std::string duration_RewriteQuery_str = "";
    if (params.isRewritequery) {
        start_RewriteQuery = std::chrono::system_clock::now();
        rewrittenQuery = RewriteQueryFromUserConversation(conversations, params.content, rewriteInput);
        end_RewriteQuery = std::chrono::system_clock::now();
        auto duration_RewriteQuery =
                std::chrono::duration_cast<std::chrono::milliseconds>(end_RewriteQuery - start_RewriteQuery);
        duration_RewriteQuery_str = std::to_string(duration_RewriteQuery.count());
        LOG_INFO("[DMContext TIME-CONSUMING] RewriteQueryFromUserConversation %d ms", duration_RewriteQuery.count());
    }

    //    组装
    std::string memory;
    HistoryBuildOptions opts;
    opts.conversationTurns = params.conversationTurns;
    opts.tokenBudget = params.tokenBudget;
    opts.newestFirst = false;
    opts.useMerge = false;
    opts.currentUserQuery = params.content;  // 合并段的 human 字段

    HistoryResultMeta meta;
    meta.rewrittenQuery = rewrittenQuery;
    meta.responseId = responseId;
    meta.memory = memory;
    meta.memoryTokenCount = 0;
    auto queryBody = context->GetRequestBody();
    std::string responseJson = BuildHistoryResult(qaList, abstractQA, opts, meta, alloc);

    HttpHelper::WriteResponse(context, responseJson, CMFrm::COM::HttpStatus::HTTP200);

    // 写入改写后的q,到数据库
    std::string body = BuildProcessMessageBody(params, rewrittenQuery, params.isForgetMemory);
    auto res = ProcessMessage(body);
    LOG_INFO("ProcessMessage result: %s", res.c_str());
    Document doc1;
    if (doc1.Parse(res.c_str()).HasParseError()) {
        LOG_INFO("ProcessMessage result parse fail, errorCode: %d", doc1.GetParseError());
        return;
    }

    std::string uuId = "";
    if (doc.HasMember("uu_id") && doc["uu_id"].IsString()) {
        uuId = doc["uu_id"].GetString();
    }

    // 缓存 rewrittenQuery 供第二次查询使用
    auto cache = std::make_shared<ContextCache>();
    cache->responseId = responseId;
    cache->rewrittenQuery = rewrittenQuery;
    cache->conversations = conversations;
    AddContextCache(responseId, cache);

    std::shared_lock<std::shared_mutex> ilock(m_processMemoryMutex);
    // 异步查询记忆
    if (params.enableMemory) {
        ProcessMemory(params, rewrittenQuery, responseId, g_turn_timestamp);
    }

    if (params.isForgetMemory) {
        ContextDbClient::GetInstance().DeleteAllMemory(params.userId);
    }

    std::string HistoryData = DumpHistoryData(qaList, abstractQA);
    auto endTimePoint = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimePoint - start_GetContext);
    std::string duration_str = std::to_string(duration.count());
    rapidjson::Value responseDoc = BuildHistoryResultForJson(qaList, abstractQA, opts, meta, alloc);
    buildAndWriteQueryLog(params, &responseDoc, &abstractQA, &qaList, rewriteInput, rewrittenQuery,
            duration_RewriteQuery_str, "", "", "", std::to_string(g_turn_timestamp),
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    start_GetContext.time_since_epoch()).count()),
            std::to_string(durationDbQuery.count()), duration_str, !params.enableMemory);

    std::string input = "==========\nFirstQueryRequest\n" + queryBody + "\nRewriteContext\n" + rewriteInput +
            "\nrewriteQuery_duration:" + duration_RewriteQuery_str + "\nFirstQueryRes\n" + responseJson +
            "\nHistoryData\n" + HistoryData + "\nFirstQueryDuration:" + duration_str +
            "\ndbQueryDuration:" + std::to_string(durationDbQuery.count());
    WriteModelLogX(params.userId, params.contextId + ".log", input);

    // 写入结果打印
    std::string writeResponseJson;
    std::string message = "success";
    BuildWriteResponse(writeResponseJson, 200, message);
    buildAndWriteWriteLog(params.userId, params.contextId, params.contextId, uuId, body, writeResponseJson, std::to_string(g_turn_timestamp), "", "", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(start_GetContext.time_since_epoch()).count()), std::to_string(0), true);
    input  = "===========\nAddContextNew body\n" + body + "\nResponse:\n" + writeResponseJson;
    WriteModelLogX(params.userId, params.contextId + ".log", input);
}

void DMContext::ContextService::GetContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                              bool isInternal,
                                              const std::chrono::system_clock::time_point& start_GetContext)
{
    auto body = context->GetRequestBody();
    std::string responseJson;
    std::string message;
    CMFrm::COM::HttpStatus status;
    // 1. 解析请求参数
    rapidjson::Document doc(rapidjson::kObjectType);
    if (!JsonParser::Parse(body.c_str(), doc)) {
        message = "Parse request body failed";
        BuildWriteResponse(responseJson, 400, message);
        status = CMFrm::COM::HttpStatus::HTTP400;
        HttpHelper::WriteResponse(context, responseJson, status);
        return;
    }

    QueryParams params;
    if (!ParseQueryParams(doc, params)) {
        message = "参数错误：缺少必填字段";
        BuildWriteResponse(responseJson, 400, message);
        status = CMFrm::COM::HttpStatus::HTTP400;
        HttpHelper::WriteResponse(context, responseJson, status);
        return;
    }
    doc.SetNull(); // 释放解析用的 doc，供后续复用

    LOG_INFO("GetContextNew userId:%s, responseId:%s", params.userId.c_str(), params.responseId.c_str());

    // 2. 构建 QAFilter
    QAFilter qaFilter = BuildQAFilter(params);

    // 3. 生成 responseId（如果需要）
    std::string responseId = params.responseId;
    if (params.responseId.empty()) {
        responseId = GenerateResponseId(params.userId, params.contextId);
    }

    // === 第二次查询： ===
    if (!params.responseId.empty()) {
        ExecuteSecondQuery(context, params, qaFilter, start_GetContext);
        return;
    }

    // === 第一次查询：正常处理并异步缓存 ===
    // 在 ExecuteFirstQuery 内部创建 doc 和 allocator
    ExecuteFirstQuery(context, params, qaFilter, responseId, start_GetContext);
}

void DMContext::buildAndWriteQueryLog(const ContextService::QueryParams& params,
        rapidjson::Value* responseJson,
        rapidjson::Value* abstractQAJson,
        rapidjson::Value* qaListJson,
        const std::string rewriteInput,
        const std::string rewrittenQuery,
        const std::string durationRewriteQuery,
        const std::string query,
        const std::string memoryResult,
        const std::string durationMemory,
        const std::string turnTimes,
        const std::string ts,
        const std::string durationDbQuery,
        const std::string delay,
        bool flag)
{
    LOG_INFO("buildAndWriteQueryLog begin, user_id: %s, service_id: %s, context_id: %s, turnTimes: %s, ts: %s, flag: %s",params.userId.c_str(), params.srvId.c_str(), params.contextId.c_str(), turnTimes.c_str(), ts.c_str(), flag ? "true" : "false");
    QueryApiRequest query_api_1 = {
        .userId = params.userId,
        .serviceId = params.srvId,
        .content = params.content,
        .contextId = params.contextId,
        .responseId = params.responseId,
    };
    // 解析 AbstractQA
    AbstractQA abstractQA{};
    QAItem qaList{};
    ResponseInfo response{};

    ParseAbstractQA(abstractQAJson, abstractQA);
    ParseQAItem(qaListJson, qaList);
    ParseResponseInfo(responseJson, response);

    HistoryData hisData{};
    hisData.abstractQA.push_back(abstractQA);
    hisData.qaList.push_back(qaList);

    RewriteInfo rewrite{};
    rewrite.modelInput = rewriteInput;
    rewrite.modelOutput = rewrittenQuery;
    rewrite.delay = durationRewriteQuery;

    MemoryInfo memory{};
    memory.query = query;
    memory.memoryResult = memoryResult;
    memory.delay = durationMemory;

    QueryApiLog queryLog1{};
    queryLog1.requestBody = query_api_1;
    queryLog1.historyData = hisData;
    queryLog1.rewrite = rewrite;
    queryLog1.memory = memory;
    queryLog1.response = response;
    queryLog1.turn = turnTimes;
    queryLog1.ts = ts;
    queryLog1.delay = delay;
    queryLog1.dbQueryDelay = durationDbQuery;


    ModelLogUtil::WriteQueryApiLog(params.userId, params.srvId, params.contextId, queryLog1, flag);
}

bool DMContext::ParseAbstractQA(rapidjson::Value* abstractQAJson, AbstractQA& abstractQA) {
    if (abstractQAJson == nullptr || !abstractQAJson->IsObject()) {
        return false;
    }

    // 解析content数组
    if (abstractQAJson->HasMember("content") && (*abstractQAJson)["content"].IsArray()) {
        const rapidjson::Value& contentArray = (*abstractQAJson)["content"];
        for (const auto& contentItem : contentArray.GetArray()) {
            if (!contentItem.IsObject()) {
                continue;
            }

            MessageContent msg;
            if (contentItem.HasMember("content") && contentItem["content"].IsString()) {
                msg.content = contentItem["content"].GetString();
            }

            if (contentItem.HasMember("role") && contentItem["role"].IsString()) {
                msg.role = contentItem["role"].GetString();
            }

            abstractQA.content.push_back(msg);
        }
    }

    // 解析id
    if (abstractQAJson->HasMember("id") && (*abstractQAJson)["id"].IsString()) {
        abstractQA.id = (*abstractQAJson)["id"].GetString();
    }
    return true;
}


bool DMContext::ParseQAItem(rapidjson::Value* qaItemJson, QAItem& qaItem) {
    if (qaItemJson == nullptr || !qaItemJson->IsObject()) {
        return false;
    }

    // 解析content数组
    if (qaItemJson->HasMember("content") && (*qaItemJson)["content"].IsArray()) {
        const rapidjson::Value& contentArray = (*qaItemJson)["content"];
        for (const auto& contentItem : contentArray.GetArray()) {
            if (!contentItem.IsObject()) {
                continue;
            }

            MessageContent msg;
            if (contentItem.HasMember("content") && contentItem["content"].IsString()) {
                msg.content = contentItem["content"].GetString();
            }

            if (contentItem.HasMember("role") && contentItem["role"].IsString()) {
                msg.role = contentItem["role"].GetString();
            }

            qaItem.content.push_back(msg);
        }
    }

    // 解析id
    if (qaItemJson->HasMember("id") && (*qaItemJson)["id"].IsString()) {
        qaItem.id = (*qaItemJson)["id"].GetString();
    }

    // 解析label
    if (qaItemJson->HasMember("label") && (*qaItemJson)["label"].IsString()) {
        qaItem.label = (*qaItemJson)["label"].GetString();
    }
    return true;
}

// 将rapidjson::Document解析为ResponseInfo结构体
bool DMContext::ParseResponseInfo(rapidjson::Value* responseDoc, ResponseInfo& responseInfo) {
    if (responseDoc == nullptr || !responseDoc->IsObject()) {
        return false;
    }

    // 解析code
    if (responseDoc->HasMember("code") && (*responseDoc)["code"].IsInt()) {
        responseInfo.code = (*responseDoc)["code"].GetInt();
    }

    // 解析msg
    if (responseDoc->HasMember("msg") && (*responseDoc)["msg"].IsString()) {
        responseInfo.msg = (*responseDoc)["msg"].GetString();
    }

    // 解析memory
    if (responseDoc->HasMember("memory") && (*responseDoc)["memory"].IsString()) {
        responseInfo.memory = (*responseDoc)["memory"].GetString();
    }

    // 解析history
    if (responseDoc->HasMember("history") && (*responseDoc)["history"].IsArray()) {
        const rapidjson::Value& historyArray = (*responseDoc)["history"];
        for (const auto& historyItem : historyArray.GetArray()) {
            if (!historyItem.IsObject()) {
                continue;
            }

            HistoryRecord record;
            if (historyItem.HasMember("human") && historyItem["human"].IsString()) {
                record.human = historyItem["human"].GetString();
            }

            if (historyItem.HasMember("ai") && historyItem["ai"].IsString()) {
                record.ai = historyItem["ai"].GetString();
            }

            responseInfo.history.push_back(record);
        }
    }

    // 解析response_id
    if (responseDoc->HasMember("response_id") && (*responseDoc)["response_id"].IsString()) {
        responseInfo.responseId = (*responseDoc)["response_id"].GetString();
    }

    // 解析rewritten_query
    if (responseDoc->HasMember("rewritten_query") && (*responseDoc)["rewritten_query"].IsString()) {
        responseInfo.rewrittenQuery = (*responseDoc)["rewritten_query"].GetString();
    }

    // 解析total_token
    if (responseDoc->HasMember("total_token") && (*responseDoc)["total_token"].IsInt()) {
        responseInfo.totalToken = (*responseDoc)["total_token"].GetInt();
    }

    // 解析history_token_count
    if (responseDoc->HasMember("history_token_count") && (*responseDoc)["history_token_count"].IsInt()) {
        responseInfo.historyTokenCount = (*responseDoc)["history_token_count"].GetInt();
    }

    // 解析memory_token_count
    if (responseDoc->HasMember("memory_token_count") && (*responseDoc)["memory_token_count"].IsInt()) {
        responseInfo.memoryTokenCount = (*responseDoc)["memory_token_count"].GetInt();
    }

    return true;
}

rapidjson::Value DMContext::BuildHistoryResultForJson(
        const rapidjson::Value& qaList,
        const rapidjson::Value& abstractQA,
        const HistoryBuildOptions& opts,
        const HistoryResultMeta& meta,
        rapidjson::Document::AllocatorType& alloc) {

    // 调用内部函数构建 history
    rapidjson::Value inner = opts.useMerge
                             ? BuildHistoryMerge(qaList, abstractQA, opts, alloc)
                             : BuildHistory(qaList, abstractQA, opts.conversationTurns, opts.tokenBudget, alloc, opts.newestFirst);

    rapidjson::Value history(inner["history"], alloc);  // deep copy
    int32_t historyTokenCount = inner["token_count"].GetInt();

    // 组装最终结构
    rapidjson::Value doc(rapidjson::kObjectType);
    doc.AddMember("code", 0, alloc);
    doc.AddMember("msg", rapidjson::Value("success", alloc), alloc);
    doc.AddMember("memory", rapidjson::Value(meta.memory.c_str(), alloc), alloc);
    doc.AddMember("history", history, alloc);
    doc.AddMember("response_id", rapidjson::Value(meta.responseId.c_str(), alloc), alloc);
    doc.AddMember("rewritten_query", rapidjson::Value(meta.rewrittenQuery.c_str(), alloc), alloc);
    doc.AddMember("total_token", historyTokenCount + meta.memoryTokenCount, alloc);
    doc.AddMember("history_token_count", historyTokenCount, alloc);
    doc.AddMember("memory_token_count", meta.memoryTokenCount, alloc);

    return doc;
}