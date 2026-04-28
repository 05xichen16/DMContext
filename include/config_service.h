/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

 */

#ifndef CONFIG_SERVICE_H
#define CONFIG_SERVICE_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "singleton.h"
#include "service_router/route_context.h"

namespace DMContext {
class ConfigService : public Singleton<ConfigService> {
public:
    using Singleton<ConfigService>::GetInstance;

    static std::shared_ptr<ConfigService> CreateInstance();

    void GetConfigTypes(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void GetConfigByType(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void GetConfig(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void UpdateConfig(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void GetPrompt(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void UpdatePrompt(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    void BatchUpdatePrompts(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

private:
    std::vector<std::string> GetAllConfigTypes();
    std::map<std::string, std::string> GetConfigByType(const std::string& type);
    std::string GetConfig(const std::string& type, const std::string& name);
    bool UpdateConfigValue(const std::string& type, const std::string& name, const std::string& value);
    bool UpdateArrayConfigValue(const std::string& type, const std::string& jsonArray);
    bool SaveConfigToFile();
    std::string GetPromptByName(const std::string& name);
    bool UpdatePromptValue(const std::string& name, const std::string& value);
    bool BatchUpdatePromptsValue(const std::string& jsonContent);
    bool SavePromptsToFile();
    void SendResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
                      int statusCode, const std::string& response);
};
}

#endif