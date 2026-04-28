/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <regex>

#include "common_define.h"
#include "llm_callback.h"
#include "model_infer.h"
#include "model_mgr.h"
#include "logger.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

#include "mm_sdk.h"
#include "mm_prompttmplt.h"
#include "string_parser.h"
#include "http_helper.h"
#include <service_router/route_context.h>
#include <json_parser.h>
#include "py_memory_service_mgr.h"

using namespace mmsdk;
namespace DMContext {

std::string ModelInfer::LLMModelInfer(const MMReasonParam &reasonParam, const MMUserIntention &query,
                                      const MMPromptTemplate prompt)
{
    MMLlmComponent llmComponent = MMLlmComponent();
    MMLLMRequest request = {{query}, {prompt}};

    int32_t ret = -1;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    std::condition_variable cond;
    std::atomic<bool> done{false};
    std::string queryRsp;
    llmComponent.RequestAsync(reasonParam, request, [&](int32_t errCode, const std::string &reponse) -> uint32_t {
        try {
            ret = errCode;
            queryRsp = reponse;
            done.store(true);
            cond.notify_one();
        } catch (...) {
            LOG_ERR("llmComponent RequestAsync exception.");
            ret = -1;
        }
        return 0;
    });

    cond.wait_for(lock, std::chrono::seconds(60), [&] { return done.load(); });
    if (ret != 0) {
        LOG_ERR("llmComponent.RequestAsync failed, ret=%u", ret);
        return "";
    }
    LOG_INFO("LLMModelInfer query ret:%d", ret);
    return queryRsp;
}

bool ModelInfer::LLMModelInferSyncWithTaskType(const std::string &query, const std::string &promptName, const std::string &taskType,
                                   std::string &outRsp, std::string model, std::string modelService)
{
    std::string modelName = std::getenv(model.c_str());
    std::string modelServiceName = std::getenv(modelService.c_str());

    MMUserIntention intention = {.query = query + " /no_think", .modaltype = MMLLM_MODAL_TYPE_TEXT};

    std::map<std::string, std::string> schedOption;
    schedOption["X-Task-Type"] = taskType;
    MMReasonParam reasonParam = {.modelServiceName = modelServiceName, .modelName = modelName, .temperature = 0.001,
        .schedoption = schedOption};

    auto prompt = mmsdk::MMPromptTemplate(mmsdk::MMPromptTmpltKey{
            .name = promptName,
            .owner = SERVICE_NAME,
    });
    prompt.SetContent(ModelMgr::GetInstance()->GetPrompt(promptName));
    prompt.RenderPrompt({});
    LOG_INFO("LLMModelInferSyncWithTaskType modelServiceName:%s, modelName:%s, taskType:%s, promptName:%s",
        modelServiceName.c_str(), modelName.c_str(), taskType.c_str(), prompt.GeKey().name.c_str());

    LOG_INFO("LLMModelInferSyncWithTaskType query size:%u", query.size());
    outRsp = LLMModelInfer(reasonParam, intention, prompt);
    if (outRsp.empty()) {
        LOG_WARNING("LLMModelInferSyncWithTaskType is empty");
        return false;
    }
    return true;
}
}