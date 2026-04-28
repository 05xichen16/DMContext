/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

 */

#include "config_service.h"
#include "logger.h"
#include "config_mgr.h"
#include "model_mgr.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "file_utils.h"

namespace DMContext {

std::shared_ptr<ConfigService> ConfigService::CreateInstance()
{
    return std::make_shared<ConfigService>();
}

void ConfigService::SendResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
                                  int statusCode, const std::string& response)
{
    context->WriteStatueCode(static_cast<CMFrm::COM::HttpStatus>(statusCode));
    context->WriteJSONContentType();
    context->WriteResponseBody(response);
    context->WriteAsyncResponse();
}

std::vector<std::string> ConfigService::GetAllConfigTypes()
{
    return ConfigMgr::GetInstance()->GetAllConfigTypes();
}

std::map<std::string, std::string> ConfigService::GetConfigByType(const std::string& type)
{
    return ConfigMgr::GetInstance()->GetConfigByType(type);
}

std::string ConfigService::GetConfig(const std::string& type, const std::string& name)
{
    return ConfigMgr::GetInstance()->GetConfig(type, name);
}

bool ConfigService::UpdateConfigValue(const std::string& type, const std::string& name, const std::string& value)
{
    return ConfigMgr::GetInstance()->UpdateConfig(type, name, value);
}

bool ConfigService::UpdateArrayConfigValue(const std::string& type, const std::string& jsonArray)
{
    return ConfigMgr::GetInstance()->UpdateArrayConfig(type, jsonArray);
}

bool ConfigService::SaveConfigToFile()
{
    return ConfigMgr::GetInstance()->SaveConfigToFile();
}

std::string ConfigService::GetPromptByName(const std::string& name)
{
    return ModelMgr::GetInstance()->GetPrompt(name);
}

bool ConfigService::UpdatePromptValue(const std::string& name, const std::string& value)
{
    return ModelMgr::GetInstance()->UpdatePrompt(name, value);
}

bool ConfigService::BatchUpdatePromptsValue(const std::string& jsonContent)
{
    return ModelMgr::GetInstance()->BatchUpdatePrompts(jsonContent);
}

bool ConfigService::SavePromptsToFile()
{
    return ModelMgr::GetInstance()->SavePromptsToFile();
}

void ConfigService::GetConfigTypes(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    auto types = GetAllConfigTypes();
    rapidjson::Document doc(rapidjson::kObjectType);
    doc.AddMember("code", 200, doc.GetAllocator());
    rapidjson::Value dataArr(rapidjson::kArrayType);
    for (const auto& t : types) {
        dataArr.PushBack(rapidjson::Value(t.c_str(), doc.GetAllocator()), doc.GetAllocator());
    }
    doc.AddMember("data", dataArr, doc.GetAllocator());
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);

    SendResponse(context, 200, sb.GetString());
}

void ConfigService::GetConfigByType(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string type = context->GetParam("type");
    if (type.empty()) {
        std::stringstream ss;
        ss << R"({"code":400,"message":"type is required"})";
        SendResponse(context, 200, ss.str());
        return;
    }

    auto configMap = GetConfigByType(type);
    rapidjson::Document doc(rapidjson::kObjectType);
    doc.AddMember("code", 200, doc.GetAllocator());
    rapidjson::Value dataObj(rapidjson::kObjectType);
    for (const auto& pair : configMap) {
        dataObj.AddMember(
            rapidjson::Value(pair.first.c_str(), doc.GetAllocator()),
            rapidjson::Value(pair.second.c_str(), doc.GetAllocator()),
            doc.GetAllocator());
    }
    doc.AddMember("data", dataObj, doc.GetAllocator());
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);

    SendResponse(context, 200, sb.GetString());
}

void ConfigService::GetConfig(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string type = context->GetParam("type");
    std::string name = context->GetParam("name");
    if (type.empty() || name.empty()) {
        std::stringstream ss;
        ss << R"({"code":400,"message":"type and name are required"})";
        SendResponse(context, 200, ss.str());
        return;
    }

    std::string value = GetConfig(type, name);
    rapidjson::Document doc(rapidjson::kObjectType);
    if (value.empty()) {
        doc.AddMember("code", 404, doc.GetAllocator());
        doc.AddMember("message", rapidjson::Value("config not found", doc.GetAllocator()), doc.GetAllocator());
    } else {
        doc.AddMember("code", 200, doc.GetAllocator());
        doc.AddMember("data", rapidjson::Value(value.c_str(), doc.GetAllocator()), doc.GetAllocator());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);

    SendResponse(context, 200, sb.GetString());
}

void ConfigService::UpdateConfig(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string type = context->GetParam("type");
    std::string name = context->GetParam("name");
    std::string body = context->GetRequestBody();

    rapidjson::Document respDoc(rapidjson::kObjectType);

    if (type.empty()) {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("type is required", respDoc.GetAllocator()), respDoc.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError()) {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("invalid json", respDoc.GetAllocator()), respDoc.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    bool success = false;
    if (doc.IsArray()) {
        success = UpdateArrayConfigValue(type, body);
    } else if (doc.HasMember("value") && doc["value"].IsString()) {
        if (name.empty()) {
            respDoc.AddMember("code", 400, respDoc.GetAllocator());
            respDoc.AddMember("message", rapidjson::Value("name is required for object type", respDoc.GetAllocator()), respDoc.GetAllocator());
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
            respDoc.Accept(writer);
            SendResponse(context, 200, sb.GetString());
            return;
        }
        std::string value = doc["value"].GetString();
        success = UpdateConfigValue(type, name, value);
    } else {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("invalid request body", respDoc.GetAllocator()), respDoc.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    if (success) {
        SaveConfigToFile();
    }

    rapidjson::Document respDoc2(rapidjson::kObjectType);
    if (success) {
        respDoc2.AddMember("code", 200, respDoc2.GetAllocator());
        respDoc2.AddMember("message", rapidjson::Value("updated successfully", respDoc2.GetAllocator()), respDoc2.GetAllocator());
    } else {
        respDoc2.AddMember("code", 404, respDoc2.GetAllocator());
        respDoc2.AddMember("message", rapidjson::Value("config not found or not updatable", respDoc2.GetAllocator()), respDoc2.GetAllocator());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    respDoc2.Accept(writer);

    SendResponse(context, 200, sb.GetString());
}

void ConfigService::GetPrompt(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string name = context->GetParam("name");
    rapidjson::Document respDoc(rapidjson::kObjectType);
    if (name.empty()) {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("name is required", respDoc.GetAllocator()), respDoc.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    std::string promptValue = GetPromptByName(name);
    rapidjson::Document respDoc2(rapidjson::kObjectType);
    if (promptValue.empty()) {
        respDoc2.AddMember("code", 404, respDoc2.GetAllocator());
        respDoc2.AddMember("message", rapidjson::Value("prompt not found", respDoc2.GetAllocator()), respDoc2.GetAllocator());
    } else {
        respDoc2.AddMember("code", 200, respDoc2.GetAllocator());
        respDoc2.AddMember("data", rapidjson::Value(promptValue.c_str(), respDoc2.GetAllocator()), respDoc2.GetAllocator());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    respDoc2.Accept(writer);
    SendResponse(context, 200, sb.GetString());
}

void ConfigService::UpdatePrompt(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string name = context->GetParam("name");
    std::string body = context->GetRequestBody();

    rapidjson::Document respDoc(rapidjson::kObjectType);
    if (name.empty()) {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("name is required", respDoc.GetAllocator()), respDoc.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError()) {
        rapidjson::Document respDoc2(rapidjson::kObjectType);
        respDoc2.AddMember("code", 400, respDoc2.GetAllocator());
        respDoc2.AddMember("message", rapidjson::Value("invalid json", respDoc2.GetAllocator()), respDoc2.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc2.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    std::string promptValue;
    if (doc.HasMember("value") && doc["value"].IsString()) {
        promptValue = doc["value"].GetString();
    } else if (doc.HasMember("prompt") && doc["prompt"].IsString()) {
        promptValue = doc["prompt"].GetString();
    } else {
        rapidjson::Document respDoc2(rapidjson::kObjectType);
        respDoc2.AddMember("code", 400, respDoc2.GetAllocator());
        respDoc2.AddMember("message", rapidjson::Value("value or prompt field is required", respDoc2.GetAllocator()), respDoc2.GetAllocator());
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        respDoc2.Accept(writer);
        SendResponse(context, 200, sb.GetString());
        return;
    }

    bool success = UpdatePromptValue(name, promptValue);
    if (success) {
        SavePromptsToFile();
    }

    rapidjson::Document respDoc3(rapidjson::kObjectType);
    if (success) {
        respDoc3.AddMember("code", 200, respDoc3.GetAllocator());
        respDoc3.AddMember("message", rapidjson::Value("updated successfully", respDoc3.GetAllocator()), respDoc3.GetAllocator());
    } else {
        respDoc3.AddMember("code", 404, respDoc3.GetAllocator());
        respDoc3.AddMember("message", rapidjson::Value("prompt not found", respDoc3.GetAllocator()), respDoc3.GetAllocator());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    respDoc3.Accept(writer);
    SendResponse(context, 200, sb.GetString());
}

void ConfigService::BatchUpdatePrompts(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    LOG_INFO("%s enter", __FUNCTION__);

    std::string body = context->GetRequestBody();
    bool success = BatchUpdatePromptsValue(body);
    if (success) {
        SavePromptsToFile();
    }

    rapidjson::Document respDoc(rapidjson::kObjectType);
    if (success) {
        respDoc.AddMember("code", 200, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("batch updated successfully", respDoc.GetAllocator()), respDoc.GetAllocator());
    } else {
        respDoc.AddMember("code", 400, respDoc.GetAllocator());
        respDoc.AddMember("message", rapidjson::Value("batch update failed", respDoc.GetAllocator()), respDoc.GetAllocator());
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    respDoc.Accept(writer);
    SendResponse(context, 200, sb.GetString());
}
}