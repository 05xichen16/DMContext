/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */


#ifndef CONFIGMGR_H
#define CONFIGMGR_H
#include <list>
#include <string>
#include <mutex>
#include <map>
#include <vector>
#include <rapidjson/document.h>
#include "singleton.h"

namespace DMContext {
class ConfigMgr {
public:
    static std::shared_ptr<ConfigMgr> GetInstance();
    static std::shared_ptr<ConfigMgr> CreateInstance();
    // 公共方法
    void InitConfigMgr();
    ConfigMgr(const ConfigMgr &) = delete;
    ConfigMgr &operator=(const ConfigMgr &) = delete;
    std::string GetCommonParamsByKey(const std::string &key);
    std::string GetTabName(const std::string &tableType);

    std::list<std::map<std::string, std::string>> GetAgPolicyList();
    std::vector<std::map<std::string, std::string>> GetDatabaseSchemaVector();
    std::map<std::string, std::string> GetModelMap();
    std::map<std::string, std::string> GetLogMap();
    std::string GetDMContextCfgRoot();

    std::string GetLabelPolicyByAgentType(const std::string &agentType);
    std::map<std::string, std::string> GetMemSummaryParams();
    std::map<std::string, std::string> GetClassifyPersonaParams();
    std::map<std::string, std::string> GetMemPersonaParams();
    std::map<std::string, std::string> GetCommonExtractParams();
    std::map<std::string, std::string> GetRerankConfigParams();
    std::map<std::string, std::string> GetRewriteRuleConfigParams();
    std::map<std::string, std::string> GetQueryConfigParams();
    std::string GetSceneCfgFile();
    std::vector<std::string> GetBlackList(const std::string &key);
    std::vector<std::string> GetForgetMemoryCommands();

    std::vector<std::string> GetAllConfigTypes();
    std::map<std::string, std::string> GetConfigByType(const std::string& type);
    std::string GetConfig(const std::string& type, const std::string& name);
    bool UpdateConfig(const std::string& type, const std::string& name, const std::string& value);
    bool UpdateArrayConfig(const std::string& type, const std::string& jsonArray);
    bool SaveConfigToFile();

    ConfigMgr() {}

private:
    std::map<std::string, std::string> GetStringMap(const rapidjson::Value *memMapJson);
    static std::shared_ptr<ConfigMgr> m_configMgrInstance;
    static std::once_flag m_configMgrInstanceOnce;
    std::string m_configJsonStr;
    std::map<std::string, std::string> m_commonParams;
    std::map<std::string, std::string> m_tableNameMap;
    std::list<std::map<std::string, std::string>> m_agPolicyList;
    std::vector<std::map<std::string, std::string>> m_databaseSchemaMaps;
    std::map<std::string, std::string> m_modelMap;
    std::map<std::string, std::string> m_logMap;
    std::map<std::string, std::string> m_rerankConfigParams;
    std::map<std::string, std::string> m_rewriteRuleConfigParams;
    std::map<std::string, std::string> m_queryConfigParams;
    std::vector<std::string> m_forgetMemoryCommands;

    std::mutex m_configMutex;

private:
    void LoadRewriteRuleConfig(const rapidjson::Value& cfgJson);
    void LoadQueryConfig(const rapidjson::Value& cfgJson);
};
}
#endif  //CONFIGMGR_H