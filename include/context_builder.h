/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

 • Description: 上下文窗口控制算法 - 头文件

 • Author: Context Service Team

 • Create: 2026-03-15

 */

#ifndef CONTEXT_SERVICE_CONTEXT_BUILDER_H
#define CONTEXT_SERVICE_CONTEXT_BUILDER_H

#include <cstdint>
#include <string>
#include <map>
#include "rapidjson/document.h"

namespace DMContext {

// 统计信息结构
struct BuildStats {
    int32_t fromQaList = 0;
    int32_t fromAbstract = 0;
    int32_t totalMessages = 0;
    int32_t skippedDueToLimit = 0;
};

// Token估算
int32_t EstimateTokens(const std::string& text);

// 构建上下文消息
// 策略：只取最后一轮原始对话（如果token数<=200），其他全部使用压缩版本
// qaList: 原始QA列表（rapidjson::Value数组，按时间顺序，最新的在最后）
// abstractQa: 压缩后的QA列表（rapidjson::Value数组，按时间顺序，最新的在最后）
// contextMsg: 输出的上下文消息数组
// alloc: rapidjson内存分配器
// maxTokens: 最大token限制（默认3000）
// stats: 统计信息输出（可选）
void BuildContextMsgAdaptive(
    const rapidjson::Value& qaList,
    const rapidjson::Value& abstractQa,
    rapidjson::Value& contextMsg,
    rapidjson::Document::AllocatorType& alloc,
    int32_t maxTokens = 3000,
    BuildStats* stats = nullptr);

} // namespace DMContext

#endif // CONTEXT_SERVICE_CONTEXT_BUILDER_H