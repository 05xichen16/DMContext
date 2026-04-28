/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#include "context_db_client.h"
#include "config/config_mgr.h"
#include "json_parser.h"
#include "logger.h"
#include "utils.h"
#include "database/rag/rag_mgr.h"
#include <http_client_factory.h>
#include <http_client_request.h>
#include <sstream>
#include <rapidjson/document.h>
#include "json/json.h"

using namespace DM::RAG;

namespace DMContext {

// 静态成员变量初始化
std::string ContextDbClient::s_kmmIpCache = "";
std::mutex ContextDbClient::s_cacheMutex;

// 获取KMM服务URL（带缓存）
std::string ContextDbClient::GetKMMUrl(const std::string &path)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);

    std::vector<std::vector<std::string>> queryResult;
    std::vector<std::string> fields{"content"};

    auto ragMgr = RagMgr::GetInstance();
    if (!ragMgr) {
        LOG_ERR("GetKMMUrl: RagMgr is null");
        return "";
    }

    auto retriever = ragMgr->GetRetriever();
    if (!retriever) {
        LOG_ERR("GetKMMUrl: Retriever is null");
        return "";
    }
    DM::RAG::FilterOption::Builder filterBuilder =
        DM::RAG::FilterOption::Builder().WithFieldName("user_id").WithEQ().WithValue(
            std::make_shared<DM::RAG::VarCharValue>("kmm_ip"));
    auto retCode = retriever->ScalarSearch("tbl_user_classified_persona",
                                           DM::RAG::QueryOption::Builder(DM::RAG::TableQueryType::SCALAR)
                                               .WithBaseSelectFields(fields)
                                               .WithBaseFilters(filterBuilder.Build())
                                               .Build(),
                                           queryResult);
    if (retCode != DBOperateCode::SUCCESS) {
        LOG_ERR("GetKMMUrl Query table failed");
    }

    if (!queryResult.empty() && !queryResult[0].empty()) {
        LOG_INFO("GetKMMUrl OK, update cache");
        s_kmmIpCache = queryResult[0][0];
    } else {
        LOG_INFO("GetKMMUrl Failed, use cached error value");
        s_kmmIpCache = "get_ip_error";
    }

    return "https://" + s_kmmIpCache + ":27203" + path;
}

ContextDbClient& ContextDbClient::GetInstance()
{
    static ContextDbClient instance;
    return instance;
}

std::pair<std::string, bool> ContextDbClient::SendHttpRequest(const std::string& url,
                                                               const std::string& body,
                                                               const std::string& method)
{
    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetReqUrl(url);
    request->AddBody(body);
    request->AddJSONContentTypeHeader();

    // 设置HTTP方法
    if (method == "PUT") {
        request->SetMethod(CMFrm::COM::PUT);
    } else {
        request->SetMethod(CMFrm::COM::POST);
    }

    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpClientFactory->GetHttp2Client("default");
    if (!http2Client) {
        LOG_ERR("ContextDbClient: Get http2 client failed");
        return {"", false};
    }

    auto http2Response = http2Client->Send(request);
    if (http2Response == nullptr) {
        LOG_ERR("ContextDbClient: http2Response is null");
        return {"", false};
    }
    if (!http2Response->Is2XXStatusCode()) {
        LOG_ERR("ContextDbClient: Http request failed, status code: %d", http2Response->GetStatusCode());
        return {"", false};
    }

    // 获取响应体
    std::string responseBody = http2Response->GetResponseBody();
    LOG_INFO("ContextDbClient: request success, method=%s, url=%s, response=%s",
             method.c_str(), url.c_str(), responseBody.c_str());

    return {responseBody, true};
}

std::pair<std::string, bool> ContextDbClient::SendHttpRequest(const std::string& url,
                                                               const std::string& body,
                                                               const std::string& method,
                                                               const std::map<std::string, std::string>& headers)
{
    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetReqUrl(url);
    request->AddBody(body);
    request->AddJSONContentTypeHeader();

    for (const auto& header : headers) {
        request->AddHeader(header.first, header.second);
    }

    // 设置HTTP方法
    if (method == "PUT") {
        request->SetMethod(CMFrm::COM::PUT);
    } else {
        request->SetMethod(CMFrm::COM::POST);
    }

    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpClientFactory->GetHttp2Client("default");
    if (!http2Client) {
        LOG_ERR("ContextDbClient: Get http2 client failed");
        return {"", false};
    }

    auto http2Response = http2Client->Send(request);
    if (http2Response == nullptr) {
        LOG_ERR("ContextDbClient: http2Response is null");
        return {"", false};
    }
    if (!http2Response->Is2XXStatusCode()) {
        LOG_ERR("ContextDbClient: Http request failed, status code: %d", http2Response->GetStatusCode());
        return {"", false};
    }

    std::string responseBody = http2Response->GetResponseBody();
    LOG_INFO("ContextDbClient: request success, method=%s, url=%s, response=%s",
             method.c_str(), url.c_str(), responseBody.c_str());

    return {responseBody, true};
}

std::string ContextDbClient::FormatTimeToISO8601(const std::string& timeStr)
{
    // 如果时间格式已经是ISO8601格式，直接返回
    if (timeStr.find("T") != std::string::npos) {
        return timeStr;
    }
    // 否则返回原值，让上游处理
    return timeStr;
}

bool ContextDbClient::WriteConversations(const std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    if (conversations.empty()) {
        return true;
    }

    std::string url = GetKMMUrl(CONTEXT_DB_API_WRITE);
    if (url.empty()) {
        LOG_ERR("WriteConversations: GetKMMUrl failed, url is empty");
        return false;
    }

    // 构建请求JSON
    Json::Value root;
    Json::Value conversationsArray(Json::arrayValue);

    for (const auto& conv : conversations) {
        Json::Value item;
        item["id"] = conv->GetId();
        item["user_id"] = conv->GetUserId();
        item["session_id"] = conv->GetSessionId();
        item["context_id"] = conv->GetContextId();
        item["label"] = conv->GetLabel();
        item["history"] = conv->GetHistory();
        item["abstract_qa"] = conv->GetAbstractQA();
        item["abstract"] = conv->GetAbstract();
        item["rewrite_q"] = conv->GetRewriteQ();
        item["voiceprint_id"] = conv->GetVoiceprintId();
        item["face_id"] = conv->GetFaceId();
        item["meta_data"] = conv->GetMetaData();
        item["agent_role"] = conv->GetAgentRole();
        item["context_status"] = conv->GetContextStatus();
        item["enable_memory"] = conv->GetEnableMemory();
        item["create_date"] = FormatTimeToISO8601(conv->GetCreateDate());
        item["update_date"] = FormatTimeToISO8601(conv->GetUpdateDate());
        conversationsArray.append(item);
    }

    root["content"] = conversationsArray;

    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "";
    std::string requestBody = Json::writeString(writer, root);
    LOG_INFO("向kmm发送批量写入数据 requestBody: %s", requestBody.c_str());
    auto [response, success] = SendHttpRequest(url, requestBody, "POST");
    if (!success) {
        LOG_ERR("ContextDbClient: WriteConversations request failed");
        return false;
    }
    LOG_INFO("收到kmm响应 response: %s", response.c_str());
    // 解析响应
    rapidjson::Document doc;
    if (!JsonParser::Parse(response.c_str(), doc)) {
        LOG_ERR("ContextDbClient: Parse response failed");
        return false;
    }

    if (!doc.HasMember("RetCode") || doc["RetCode"].GetInt() != 200) {
        LOG_ERR("ContextDbClient: WriteConversations failed, response=%s", response.c_str());
        return false;
    }

    LOG_INFO("ContextDbClient: WriteConversations success");
    return true;
}

bool ContextDbClient::QueryConversations(const std::string& userId, const std::string& contextId,
                                          const std::string& content, int32_t topK,
                                          const std::string& contextStatus,
                                          std::vector<std::shared_ptr<UserConversation>>& conversations)
{
    std::string url = GetKMMUrl(CONTEXT_DB_API_QUERY);
    if (url.empty()) {
        LOG_ERR("QueryConversations: GetKMMUrl failed, url is empty");
        return false;
    }

    // 构建请求JSON
    Json::Value root;
    Json::Value filter;
    filter["user_id"] = userId;
    if (!contextId.empty()) {
        filter["context_id"] = contextId;
    }
    if (!contextStatus.empty()) {
        filter["context_status"] = contextStatus;
    } else {
        filter["context_status"] = "active";
    }
    root["filter"] = filter;
    root["top_k"] = topK;

    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "";
    std::string requestBody = Json::writeString(writer, root);

    LOG_INFO("发送给kmm的查询请求 requestBody: %s", requestBody.c_str());
    auto [response, success] = SendHttpRequest(url, requestBody, "POST");
    if (!success) {
        LOG_ERR("ContextDbClient: QueryConversations request failed");
        return false;
    }
    LOG_INFO("收到kmm的响应 response: %s", response.c_str());
    // 解析响应
    rapidjson::Document doc;
    if (!JsonParser::Parse(response.c_str(), doc)) {
        LOG_ERR("ContextDbClient: Parse response failed");
        return false;
    }

    if (!doc.HasMember("RetCode") || doc["RetCode"].GetInt() != 200) {
        LOG_ERR("ContextDbClient: QueryConversations failed, response=%s", response.c_str());
        return false;
    }

    if (!doc.HasMember("data") || !doc["data"].IsArray()) {
        LOG_ERR("ContextDbClient: QueryConversations response format error: data is not array");
        return false;
    }

    const rapidjson::Value& conversationsArray = doc["data"];
    for (const auto& item : conversationsArray.GetArray()) {
        auto conv = std::make_shared<UserConversation>();
        if (item.HasMember("id")) conv->SetId(item["id"].GetString());
        if (item.HasMember("user_id")) conv->SetUserId(item["user_id"].GetString());
        if (item.HasMember("session_id")) conv->SetSessionId(item["session_id"].GetString());
        if (item.HasMember("context_id")) conv->SetContextId(item["context_id"].GetString());
        if (item.HasMember("label")) conv->SetLabel(item["label"].GetString());
        if (item.HasMember("history")) conv->SetHistory(item["history"].GetString());
        if (item.HasMember("abstract_qa")) conv->SetAbstractQA(item["abstract_qa"].GetString());
        if (item.HasMember("abstract")) conv->SetAbstract(item["abstract"].GetString());
        if (item.HasMember("rewrite_q")) conv->SetRewriteQ(item["rewrite_q"].GetString());
        if (item.HasMember("voiceprint_id")) conv->SetVoiceprintId(item["voiceprint_id"].GetString());
        if (item.HasMember("face_id")) conv->SetFaceId(item["face_id"].GetString());
        if (item.HasMember("meta_data")) conv->SetMetaData(item["meta_data"].GetString());
        if (item.HasMember("agent_role")) conv->SetAgentRole(item["agent_role"].GetString());
        if (item.HasMember("context_status")) conv->SetContextStatus(item["context_status"].GetString());
        if (item.HasMember("create_date")) conv->SetCreateDate(item["create_date"].GetString());
        if (item.HasMember("update_date")) conv->SetUpdateDate(item["update_date"].GetString());
        conversations.push_back(conv);
    }

    LOG_INFO("ContextDbClient: QueryConversations success, count=%d", conversations.size());
    return true;
}

bool ContextDbClient::UpdateContextStatus(const std::string& userId, const std::string& contextId,
                                           const std::string& contextStatus)
{
    std::string url = GetKMMUrl(CONTEXT_DB_API_STATUS);
    if (url.empty()) {
        LOG_ERR("UpdateContextStatus: GetKMMUrl failed, url is empty");
        return false;
    }

    // 构建请求JSON
    Json::Value root;
    Json::Value filter;
    filter["user_id"] = userId;
    if (!contextId.empty()) {
        filter["context_id"] = contextId;
    }
    Json::Value content;
    content["context_status"] = contextStatus;
    root["filter"] = filter;
    root["content"] = content;
    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "";
    std::string requestBody = Json::writeString(writer, root);
    LOG_INFO("向kmm发送更新requestBody: %s", requestBody.c_str());

    auto [response, success] = SendHttpRequest(url, requestBody, "POST");
    LOG_INFO("收到kmm更新响应response: %s", response.c_str());
    if (!success) {
        LOG_ERR("ContextDbClient: UpdateContextStatus request failed");
        return false;
    }

    // 解析响应
    rapidjson::Document doc;
    if (!JsonParser::Parse(response.c_str(), doc)) {
        LOG_ERR("ContextDbClient: Parse response failed");
        return false;
    }

    if (!doc.HasMember("RetCode") || doc["RetCode"].GetInt() != 200) {
        LOG_ERR("ContextDbClient: UpdateContextStatus failed, response=%s", response.c_str());
        return false;
    }

    LOG_INFO("ContextDbClient: UpdateContextStatus success");
    return true;
}

bool ContextDbClient::UpdateConversation(const std::shared_ptr<UserConversation>& conversation)
{
    std::string url = GetKMMUrl(CONTEXT_DB_API_STATUS);
    if (url.empty()) {
        LOG_ERR("UpdateConversation: GetKMMUrl failed, url is empty");
        return false;
    }

    // 构建请求JSON
    Json::Value root;
    Json::Value filter;
    filter["user_id"] = conversation->GetUserId();
    std::string contextId = conversation->GetContextId();
    if (!contextId.empty()) {
        filter["context_id"] = contextId;
    }

    std::string id = conversation->GetId();
    if (!id.empty()) {
        filter["id"] = id;
    }
    Json::Value content;
    if (!conversation->GetAbstractQA().empty()) {
        content["abstract_qa"] = conversation->GetAbstractQA();
    }
    if (!conversation->GetLabel().empty()) {
        content["label"] = conversation->GetLabel();
    }

    if (!conversation->GetAbstract().empty()) {
        content["abstract"] = conversation->GetAbstract();
    }

    if (!conversation->GetHistory().empty()) {
        content["history"] = conversation->GetHistory();
    }
    content["update_date"] = FormatTimeToISO8601(conversation->GetUpdateDate());
    root["filter"] = filter;
    root["content"] = content;
    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "";
    std::string requestBody = Json::writeString(writer, root);
    LOG_INFO("向kmm发送更新对话记录requestBody: %s", requestBody.c_str());

    auto [response, success] = SendHttpRequest(url, requestBody, "POST");
    LOG_INFO("收到kmm更新响应response: %s", response.c_str());
    if (!success) {
        LOG_ERR("ContextDbClient: UpdateConversation request failed");
        return false;
    }

    // 解析响应
    rapidjson::Document doc;
    if (!JsonParser::Parse(response.c_str(), doc)) {
        LOG_ERR("ContextDbClient: Parse response failed");
        return false;
    }

    if (!doc.HasMember("RetCode") || doc["RetCode"].GetInt() != 200) {
        LOG_ERR("ContextDbClient: UpdateConversation failed, response=%s", response.c_str());
        return false;
    }

    LOG_INFO("ContextDbClient: UpdateConversation success");
    return true;
}

bool ContextDbClient::DeleteAllMemory(const std::string& userId)
{
    std::string url = GetKMMUrl(CONTEXT_DB_API_DELETE);
    if (url.empty()) {
        LOG_ERR("DeleteAllMemory: GetKMMUrl failed, url is empty");
        return false;
    }

    Json::Value root;
    root["type"] = "all";
    root["id"] = "all";

    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "";
    std::string requestBody = Json::writeString(writer, root);
    LOG_INFO("DeleteAllMemory requestBody: %s", requestBody.c_str());

    std::map<std::string, std::string> headers;
    headers["userId"] = userId;
    auto [response, success] = SendHttpRequest(url, requestBody, "POST", headers);
    if (!success) {
        LOG_ERR("ContextDbClient: DeleteAllMemory request failed");
        return false;
    }
    LOG_INFO("DeleteAllMemory response: %s", response.c_str());

    rapidjson::Document doc;
    if (!JsonParser::Parse(response.c_str(), doc)) {
        LOG_ERR("ContextDbClient: Parse response failed");
        return false;
    }

    if (!doc.HasMember("RetCode") || doc["RetCode"].GetInt() != 200) {
        LOG_ERR("ContextDbClient: DeleteAllMemory failed, response=%s", response.c_str());
        return false;
    }

    LOG_INFO("ContextDbClient: DeleteAllMemory success");
    return true;
}

}  // namespace DMContext