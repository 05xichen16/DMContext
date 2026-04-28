/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */


#include "config_mgr.h"
#include <json_parser.h>
#include <logger.h>
#include <string>
#include <map>
#include <filesystem>
#include "file_utils.h"
#include "scene_cfg.h"
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace DMContext {
static const std::string DMCONTEXTSERVICE_CONFIG_PATH = "dmcontextservice_config.json";
static const std::string COMMON_PARAMS = "com_params";
static const std::string AGING_POLICY = "aging_policy";
static const std::string MEM_TABLE = "memory_tbl_map";
static const std::string MEM_POLICY = "mem_policy";
static const std::string DATABASE = "database";
static const std::string MODEL = "model";
static const std::string MEM_SUMMARY = "memory_summary";
static const std::string MOVIE_CFG = "movie_config";
static const std::string MEM_PERSONA = "memory_persona";
static const std::string COMMON_EXTRACT = "common_extract";
static const std::string RERANK_CONFIG = "rerank_config";
static const std::string REWRITE_RULE_CONFIG = "rewrite_rule_config";
static const std::string CONTEXT_COMMON_CONFIG = "context_common_config";
static const std::string QUERY_CONFIG = "query_config";
static const std::string BLACK_LIST = "black_list";
static const std::string FORGET_MEMORY_COMMANDS = "forget_memory_commands";

std::shared_ptr<ConfigMgr> ConfigMgr::m_configMgrInstance = nullptr;
std::once_flag ConfigMgr::m_configMgrInstanceOnce;

std::shared_ptr<ConfigMgr> ConfigMgr::GetInstance()
{
    return Singleton<ConfigMgr>::GetInstance();
}

std::shared_ptr<ConfigMgr> ConfigMgr::CreateInstance()
{
    return std::make_shared<ConfigMgr>();
}

std::map<std::string, std::string> ConfigMgr::GetStringMap(const rapidjson::Value *memMapJson)
{
    std::map<std::string, std::string> mapRes;
    if (memMapJson && memMapJson->IsObject()) {
        for (auto it = memMapJson->MemberBegin(); it != memMapJson->MemberEnd(); ++it) {
            std::string key = it->name.GetString();
            std::string value = it->value.GetString();
            mapRes[key] = value;
        }
    }
    return mapRes;
}

std::map<std::string, std::vector<std::string>> GetVectorMap(const rapidjson::Value *memMapJson)
{
    std::map<std::string, std::vector<std::string>> mapRes;
    if (memMapJson && memMapJson->IsObject()) {
        for (auto it = memMapJson->MemberBegin(); it != memMapJson->MemberEnd(); ++it) {
            std::string key = it->name.GetString();
            std::vector<std::string> value;
            if (it->value.IsArray()) {
                auto v = it->value.GetArray();
                for (auto vIt = v.Begin(); vIt != v.End(); ++vIt) {
                    value.push_back(vIt->GetString());
                }
                mapRes[key] = value;
            }
        }
    }
    return mapRes;
}

void ConfigMgr::InitConfigMgr()
{
    LOG_INFO("%s ready to load dmcontextservice_config", __FUNCTION__);
    if (!FileUtils::LoadContentFromPath(ConfigMgr::GetInstance()->GetDMContextCfgRoot() + DMCONTEXTSERVICE_CONFIG_PATH,
                                        this->m_configJsonStr)) {
        LOG_INFO("load file from %s failed", DMCONTEXTSERVICE_CONFIG_PATH.c_str());
        return;
    }
    if (this->m_configJsonStr.empty()) {
        LOG_ERR("%s params is null", DMCONTEXTSERVICE_CONFIG_PATH.c_str());
    }

    rapidjson::Document cfgJson;
    JsonParser::Parse(m_configJsonStr.c_str(), cfgJson);

    //加载commonParams
    const rapidjson::Value *commonParamsJson = JsonParser::GetNode(cfgJson, COMMON_PARAMS);
    m_commonParams = GetStringMap(commonParamsJson);

    //加载tableName
    const rapidjson::Value *memMapJson = JsonParser::GetNode(cfgJson, MEM_TABLE);
    m_tableNameMap = GetStringMap(memMapJson);

    //初始化老化策略
    const rapidjson::Value *agingPolicyArray = JsonParser::GetNode(cfgJson, AGING_POLICY);
    if (agingPolicyArray == nullptr || !agingPolicyArray->IsArray()) {
        LOG_INFO("Geeting aging_policy error");
        return;
    }
    for (const auto &policyValue : agingPolicyArray->GetArray()) {
        if (policyValue.IsObject()) {
            m_agPolicyList.push_back(GetStringMap(&policyValue));
        }
    }

    //初始化数据库
    const rapidjson::Value *databaseSchemaJson = JsonParser::GetNode(cfgJson, DATABASE);
    if (databaseSchemaJson == nullptr || !databaseSchemaJson->IsArray()) {
        LOG_INFO("Geeting database schema error");
        return;
    }
    for (const auto &value : databaseSchemaJson->GetArray()) {
        if (value.IsObject()) {
            m_databaseSchemaMaps.push_back(GetStringMap(&value));
        }
    }

    //初始化模型url和prompt配置
    const rapidjson::Value *modelJson = JsonParser::GetNode(cfgJson, MODEL);
    m_modelMap = GetStringMap(modelJson);

    //加载改写配置文件
    LoadRewriteRuleConfig(cfgJson);

    // 加载查询配置
    LoadQueryConfig(cfgJson);

    // rerank模型在通用记忆召回场景的score门限设置
    const rapidjson::Value *rerankThresholdJson;
    rerankThresholdJson = JsonParser::GetNode(cfgJson, RERANK_CONFIG);
    m_rerankConfigParams = GetStringMap(rerankThresholdJson);

    // 重写规则配置加载
    const rapidjson::Value *rewriteRuleConfigJson;
    rewriteRuleConfigJson = JsonParser::GetNode(cfgJson, REWRITE_RULE_CONFIG);
    m_rewriteRuleConfigParams = GetStringMap(rewriteRuleConfigJson);

    // 加载忘记记忆命令配置
    const rapidjson::Value *forgetMemoryCommandsJson = JsonParser::GetNode(cfgJson, FORGET_MEMORY_COMMANDS);
    if (forgetMemoryCommandsJson != nullptr && forgetMemoryCommandsJson->IsArray()) {
        for (const auto &cmd : forgetMemoryCommandsJson->GetArray()) {
            if (cmd.IsString()) {
                m_forgetMemoryCommands.push_back(cmd.GetString());
            }
        }
    }
    LOG_INFO("forget_memory_commands count=%zu", m_forgetMemoryCommands.size());
}

std::string ConfigMgr::GetDMContextCfgRoot()
{
    auto res = getenv("DMCONTEXT_CONFIG_ROOT_PATH");
    if (res == nullptr) {
        return "/opt/coremind/conf/";
    } else {
        return res;
    }
}

std::map<std::string, std::string> ConfigMgr::GetModelMap()
{
    return m_modelMap;
}

std::vector<std::map<std::string, std::string>> ConfigMgr::GetDatabaseSchemaVector()
{
    return this->m_databaseSchemaMaps;
}

std::string ConfigMgr::GetCommonParamsByKey(const std::string &key)
{
    return this->m_commonParams[key];
}

std::string ConfigMgr::GetTabName(const std::string &tableType)
{
    auto res = this->m_tableNameMap.find(tableType);
    return res != this->m_tableNameMap.end() ? this->m_tableNameMap[tableType] : tableType;
}

std::list<std::map<std::string, std::string>> ConfigMgr::GetAgPolicyList()
{
    return this->m_agPolicyList;
}

std::map<std::string, std::string> ConfigMgr::GetRerankConfigParams()
{
    return this->m_rerankConfigParams;
}

std::map<std::string, std::string> ConfigMgr::GetRewriteRuleConfigParams()
{
    return m_rewriteRuleConfigParams;
}

void ConfigMgr::LoadRewriteRuleConfig(const rapidjson::Value& cfgJson)
{
    // rerank模型在通用记忆召回场景的score门限设置
    const rapidjson::Value *rewriteRuleConfigJson;
    rewriteRuleConfigJson = JsonParser::GetNode(cfgJson, REWRITE_RULE_CONFIG);
    if (rewriteRuleConfigJson == nullptr) {
        LOG_WARNING("rewrite_rule_config json node not found");
        return;
    }
    m_rewriteRuleConfigParams = GetStringMap(rewriteRuleConfigJson);
}

void ConfigMgr::LoadQueryConfig(const rapidjson::Value& cfgJson) {
    const rapidjson::Value *queryConfigJson;
    queryConfigJson = JsonParser::GetNode(cfgJson, QUERY_CONFIG);
    if (queryConfigJson == nullptr) {
        LOG_WARNING("query_config json node not found");
        return;
    }
    m_queryConfigParams = GetStringMap(queryConfigJson);
}

std::map<std::string, std::string> ConfigMgr::GetQueryConfigParams()
{
    return m_queryConfigParams;
}

std::vector<std::string> ConfigMgr::GetForgetMemoryCommands()
{
    return m_forgetMemoryCommands;
}

std::vector<std::string> ConfigMgr::GetAllConfigTypes()
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return {
        "com_params",
        "aging_policy",
        "memory_tbl_map",
        "model",
        "rerank_config",
        "rewrite_rule_config",
        "query_config",
        "forget_memory_commands"
    };
}

std::map<std::string, std::string> ConfigMgr::GetConfigByType(const std::string& type)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (type == "com_params") {
        return m_commonParams;
    } else if (type == "model") {
        return m_modelMap;
    } else if (type == "rerank_config") {
        return m_rerankConfigParams;
    } else if (type == "rewrite_rule_config") {
        return m_rewriteRuleConfigParams;
    } else if (type == "query_config") {
        return m_queryConfigParams;
    } else if (type == "memory_tbl_map") {
        return m_tableNameMap;
    } else if (type == "forget_memory_commands") {
        std::map<std::string, std::string> result;
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        writer.StartArray();
        for (const auto& cmd : m_forgetMemoryCommands) {
            writer.String(cmd.c_str(), cmd.length());
        }
        writer.EndArray();
        result["commands"] = sb.GetString();
        return result;
    } else if (type == "aging_policy") {
        std::map<std::string, std::string> result;
        rapidjson::Document doc(rapidjson::kArrayType);
        auto& allocator = doc.GetAllocator();
        for (const auto& policy : m_agPolicyList) {
            rapidjson::Value obj(rapidjson::kObjectType);
            for (const auto& pair : policy) {
                obj.AddMember(rapidjson::Value(pair.first.c_str(), allocator), rapidjson::Value(pair.second.c_str(), allocator), allocator);
            }
            doc.PushBack(obj, allocator);
        }
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        doc.Accept(writer);
        result["policies"] = sb.GetString();
        return result;
    } else if (type == "database") {
        std::map<std::string, std::string> result;
        rapidjson::Document doc(rapidjson::kArrayType);
        auto& allocator = doc.GetAllocator();
        for (const auto& db : m_databaseSchemaMaps) {
            rapidjson::Value obj(rapidjson::kObjectType);
            for (const auto& pair : db) {
                obj.AddMember(rapidjson::Value(pair.first.c_str(), allocator), rapidjson::Value(pair.second.c_str(), allocator), allocator);
            }
            doc.PushBack(obj, allocator);
        }
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        doc.Accept(writer);
        result["schemas"] = sb.GetString();
        return result;
    }
    return {};
}

std::string ConfigMgr::GetConfig(const std::string& type, const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    auto configMap = GetConfigByType(type);
    auto it = configMap.find(name);
    if (it != configMap.end()) {
        return it->second;
    }
    return "";
}

bool ConfigMgr::UpdateConfig(const std::string& type, const std::string& name, const std::string& value)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    if (type == "com_params") {
        m_commonParams[name] = value;
        return true;
    } else if (type == "model") {
        m_modelMap[name] = value;
        return true;
    } else if (type == "rerank_config") {
        m_rerankConfigParams[name] = value;
        return true;
    } else if (type == "rewrite_rule_config") {
        m_rewriteRuleConfigParams[name] = value;
        return true;
    } else if (type == "query_config") {
        m_queryConfigParams[name] = value;
        return true;
    } else if (type == "memory_tbl_map") {
        m_tableNameMap[name] = value;
        return true;
    }
    return false;
}

bool ConfigMgr::UpdateArrayConfig(const std::string& type, const std::string& jsonArray)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    rapidjson::Document doc;
    if (doc.Parse(jsonArray.c_str()).HasParseError() || !doc.IsArray()) {
        LOG_ERR("UpdateArrayConfig: parse json array failed, type=%s", type.c_str());
        return false;
    }

    if (type == "forget_memory_commands") {
        m_forgetMemoryCommands.clear();
        for (const auto& item : doc.GetArray()) {
            if (item.IsString()) {
                m_forgetMemoryCommands.push_back(item.GetString());
            }
        }
        return true;
    } else if (type == "aging_policy") {
        m_agPolicyList.clear();
        for (const auto& item : doc.GetArray()) {
            if (item.IsObject()) {
                std::map<std::string, std::string> policy;
                for (auto& member : item.GetObject()) {
                    if (member.value.IsString()) {
                        policy[member.name.GetString()] = member.value.GetString();
                    }
                }
                m_agPolicyList.push_back(policy);
            }
        }
        return true;
    } else if (type == "database") {
        m_databaseSchemaMaps.clear();
        for (const auto& item : doc.GetArray()) {
            if (item.IsObject()) {
                std::map<std::string, std::string> schema;
                for (auto& member : item.GetObject()) {
                    if (member.value.IsString()) {
                        schema[member.name.GetString()] = member.value.GetString();
                    }
                }
                m_databaseSchemaMaps.push_back(schema);
            }
        }
        return true;
    }
    return false;
}

bool ConfigMgr::SaveConfigToFile()
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    std::string configPath = GetDMContextCfgRoot() + DMCONTEXTSERVICE_CONFIG_PATH;

    rapidjson::Document doc;
    doc.Parse(m_configJsonStr.c_str());
    if (doc.HasParseError()) {
        LOG_ERR("SaveConfigToFile: parse original config failed");
        return false;
    }

    if (doc.HasMember("com_params") && doc["com_params"].IsObject()) {
        for (auto& pair : m_commonParams) {
            if (doc["com_params"].HasMember(pair.first.c_str())) {
                doc["com_params"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("model") && doc["model"].IsObject()) {
        for (auto& pair : m_modelMap) {
            if (doc["model"].HasMember(pair.first.c_str())) {
                doc["model"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("rerank_config") && doc["rerank_config"].IsObject()) {
        for (auto& pair : m_rerankConfigParams) {
            if (doc["rerank_config"].HasMember(pair.first.c_str())) {
                doc["rerank_config"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("rewrite_rule_config") && doc["rewrite_rule_config"].IsObject()) {
        for (auto& pair : m_rewriteRuleConfigParams) {
            if (doc["rewrite_rule_config"].HasMember(pair.first.c_str())) {
                doc["rewrite_rule_config"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("query_config") && doc["query_config"].IsObject()) {
        for (auto& pair : m_queryConfigParams) {
            if (doc["query_config"].HasMember(pair.first.c_str())) {
                doc["query_config"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("memory_tbl_map") && doc["memory_tbl_map"].IsObject()) {
        for (auto& pair : m_tableNameMap) {
            if (doc["memory_tbl_map"].HasMember(pair.first.c_str())) {
                doc["memory_tbl_map"][pair.first.c_str()].SetString(pair.second.c_str(), doc.GetAllocator());
            }
        }
    }

    if (doc.HasMember("forget_memory_commands") && doc["forget_memory_commands"].IsArray()) {
        doc["forget_memory_commands"].GetArray().Clear();
        for (const auto& cmd : m_forgetMemoryCommands) {
            doc["forget_memory_commands"].GetArray().PushBack(rapidjson::Value(cmd.c_str(), doc.GetAllocator()), doc.GetAllocator());
        }
    }

    if (doc.HasMember("aging_policy") && doc["aging_policy"].IsArray()) {
        doc["aging_policy"].GetArray().Clear();
        for (const auto& policy : m_agPolicyList) {
            rapidjson::Value obj(rapidjson::kObjectType);
            for (const auto& pair : policy) {
                obj.AddMember(rapidjson::Value(pair.first.c_str(), doc.GetAllocator()), rapidjson::Value(pair.second.c_str(), doc.GetAllocator()), doc.GetAllocator());
            }
            doc["aging_policy"].GetArray().PushBack(obj, doc.GetAllocator());
        }
    }

    if (doc.HasMember("database") && doc["database"].IsArray()) {
        doc["database"].GetArray().Clear();
        for (const auto& schema : m_databaseSchemaMaps) {
            rapidjson::Value obj(rapidjson::kObjectType);
            for (const auto& pair : schema) {
                obj.AddMember(rapidjson::Value(pair.first.c_str(), doc.GetAllocator()), rapidjson::Value(pair.second.c_str(), doc.GetAllocator()), doc.GetAllocator());
            }
            doc["database"].GetArray().PushBack(obj, doc.GetAllocator());
        }
    }

    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
    writer.SetIndent(' ', 4);
    doc.Accept(writer);

    std::string tempPath = configPath + ".tmp";
    if (!DMContext::FileUtils::WriteFile(tempPath, sb.GetString(), false, true)) {
        LOG_ERR("SaveConfigToFile: write temp file failed");
        return false;
    }

    if (std::rename(tempPath.c_str(), configPath.c_str()) != 0) {
        LOG_ERR("SaveConfigToFile: rename file failed");
        std::filesystem::remove(tempPath);
        return false;
    }

    m_configJsonStr = sb.GetString();
    LOG_INFO("SaveConfigToFile: success");
    return true;
}
}