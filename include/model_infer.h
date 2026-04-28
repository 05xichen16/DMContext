/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */
#ifndef DMCONTEXT_SERVICE_INCLUDE_INFRASTRUCTURE_MODEL_INFER_H_
#define DMCONTEXT_SERVICE_INCLUDE_INFRASTRUCTURE_MODEL_INFER_H_

#include <string>
#include <memory>

#include "mm_llm_component.h"
#include "mm_prompttmplt.h"

using namespace mmsdk;
namespace DMContext {

// 静态接口类，对外提供模型访问接口&不同场景下模型访问原子能力接口
class ModelInfer {
public:
    // 模型访问原子能力接口, 非流式
    static std::string LLMModelInfer(const MMReasonParam &reasonParam, const MMUserIntention &query,
                              const MMPromptTemplate prompt);

    static bool LLMModelInferSyncWithTaskType(const std::string &query, const std::string &promptName,
        const std::string &taskType, std::string &outRsp, std::string model = "LLM_MODEL_32B",
        std::string modelService = "LLM_MODEL_SERVICE_NAME");
};
}

#endif  // DMCONTEXT_SERVICE_INCLUDE_INFRASTRUCTURE_MODEL_INFER_H_