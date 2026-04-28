/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 *
 • Description: 上下文窗口控制算法实现

 • Author: Context Service Team

 • Create: 2024-03-15

 */

#include "context_builder.h"
#include "logger.h"
#include "utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <sstream>
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

namespace DMContext {

namespace {
    // 从rapidjson::Value中提取query或answer对象的JSON字符串
    std::string GetContentFromMessage(const rapidjson::Value& msg, const char* fieldName)
    {
        if (msg.HasMember(fieldName) && msg[fieldName].IsObject()) {
            const auto& field = msg[fieldName];
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            field.Accept(writer);
            std::string result = buffer.GetString();
            return result;
        }
        return "";
    }

    // 计算单个QA的token数（只统计query和answer的content）
    int32_t EstimateQaTokens(const rapidjson::Value& qa)
    {
        int32_t contentTokens = 0;

        // 提取query的content
        std::string queryContent = GetContentFromMessage(qa, "query");
        contentTokens += calculate_estimated_tokens(queryContent);

        // 提取answer的content
        std::string answerContent = GetContentFromMessage(qa, "answer");
        contentTokens += calculate_estimated_tokens(answerContent);

        return contentTokens;
    }
} // anonymous namespace

// 估算文本的token数量
// 直接使用 context_service.cpp 中的 calculate_estimated_tokens 函数
int32_t EstimateTokens(const std::string& text)
{
    return calculate_estimated_tokens(text);
}

// 构建上下文消息列表
// 策略：只取最后一轮原始对话（如果token数<=200），其他全部使用压缩版本
void BuildContextMsgAdaptive(
    const rapidjson::Value& qaList,
    const rapidjson::Value& abstractQa,
    rapidjson::Value& contextMsg,
    rapidjson::Document::AllocatorType& alloc,
    int32_t maxTokens,
    BuildStats* stats)
{
    LOG_INFO("BuildContextMsgAdaptive: start, maxTokens=%d", maxTokens);

    contextMsg.SetArray();

    BuildStats localStats;
    if (stats == nullptr) {
        stats = &localStats;
    }

    // 检查输入参数
    bool qaListValid = qaList.IsArray();
    bool abstractQaValid = abstractQa.IsArray();
    int32_t qaListSize = qaListValid ? static_cast<int32_t>(qaList.Size()) : 0;
    int32_t abstractQaSize = abstractQaValid ? static_cast<int32_t>(abstractQa.Size()) : 0;

    LOG_INFO("BuildContextMsgAdaptive: qaList valid=%s, size=%d; abstractQa valid=%s, size=%d",
             qaListValid ? "true" : "false", qaListSize,
             abstractQaValid ? "true" : "false", abstractQaSize);

    // 边界情况处理
    if (!qaListValid || !abstractQaValid) {
        LOG_WARNING("BuildContextMsgAdaptive: qaList is array=%s, abstractQa is array=%s",
                    qaListValid ? "true" : "false", abstractQaValid ? "true" : "false");
    }

    if (qaListSize == 0 && abstractQaSize == 0) {
        LOG_WARNING("BuildContextMsgAdaptive: both qaList and abstractQa are empty, qaListSize=%d, abstractQaSize=%d",
                    qaListSize, abstractQaSize);
        return;
    }

    // 使用pair存储(索引, 消息指针)
    std::vector<std::pair<int32_t, const rapidjson::Value*>> contextItems;
    int32_t usedTokens = 0;

    // 步骤1: 处理最后一轮（最新一轮）
    // 如果最后一轮原始对话token数<=200，使用原始版本；否则使用压缩版本
    constexpr int32_t LAST_ROUND_TOKEN_THRESHOLD = 1000;

    int32_t lastIndex = qaListSize - 1;

    if (qaListSize > 0) {
        const rapidjson::Value& lastMsg = qaList[lastIndex];
        int32_t lastMsgTokens = EstimateQaTokens(lastMsg);

        LOG_INFO("BuildContextMsgAdaptive: last round tokens=%d, threshold=%d", lastMsgTokens, LAST_ROUND_TOKEN_THRESHOLD);

        if (lastMsgTokens <= LAST_ROUND_TOKEN_THRESHOLD) {
            // 使用原始版本
            if (lastMsgTokens <= maxTokens) {
                contextItems.push_back({lastIndex, &lastMsg});
                usedTokens += lastMsgTokens;
                stats->fromQaList++;
                LOG_INFO("BuildContextMsgAdaptive: use original last round, usedTokens=%d", usedTokens);
            } else {
                // 单条超过限制，仍保留保证至少有一条
                contextItems.push_back({lastIndex, &lastMsg});
                usedTokens = lastMsgTokens;
                stats->fromQaList++;
                LOG_WARNING("BuildContextMsgAdaptive: last round exceeds maxTokens, still use it, usedTokens=%d", usedTokens);
            }
        } else {
            // 使用压缩版本
            if (lastIndex < abstractQaSize) {
                const rapidjson::Value& compressedMsg = abstractQa[lastIndex];
                int32_t compressedTokens = EstimateQaTokens(compressedMsg);

                LOG_INFO("BuildContextMsgAdaptive: compressed last round tokens=%d", compressedTokens);

                if (compressedTokens <= maxTokens) {
                    contextItems.push_back({lastIndex, &compressedMsg});
                    usedTokens += compressedTokens;
                    stats->fromAbstract++;
                    LOG_INFO("BuildContextMsgAdaptive: use compressed last round, usedTokens=%d", usedTokens);
                } else {
                    // 压缩版本也超过限制，仍保留
                    contextItems.push_back({lastIndex, &compressedMsg});
                    usedTokens = compressedTokens;
                    stats->fromAbstract++;
                    LOG_WARNING("BuildContextMsgAdaptive: compressed last round exceeds maxTokens, still use it, usedTokens=%d", usedTokens);
                }
            } else {
                LOG_ERR("BuildContextMsgAdaptive: lastIndex=%d >= abstractQaSize=%d, cannot find compressed version",
                        lastIndex, abstractQaSize);
            }
        }
    }

    // 步骤2: 填充其他历史内容（全部使用abstractQa压缩内容）
    // 从后向前遍历abstractQa，跳过已处理的最后一轮
    LOG_INFO("BuildContextMsgAdaptive: start filling history content");
    for (int32_t i = abstractQaSize - 1; i >= 0; --i) {
        // 跳过已处理的最后一轮
        if (i == lastIndex && qaListSize > 0) {
            continue;
        }

        const rapidjson::Value& msg = abstractQa[i];
        int32_t msgTokens = EstimateQaTokens(msg);

        if (usedTokens + msgTokens <= maxTokens) {
            contextItems.push_back({i, &msg});
            usedTokens += msgTokens;
            stats->fromAbstract++;
            LOG_INFO("BuildContextMsgAdaptive: add history round %d, tokens=%d, usedTokens=%d", i, msgTokens, usedTokens);
        } else {
            stats->skippedDueToLimit++;
            LOG_INFO("BuildContextMsgAdaptive: skip history round %d due to token limit, msgTokens=%d, usedTokens=%d",
                     i, msgTokens, usedTokens);
            break;
        }
    }

    // 步骤3: 按时间正序排序（旧的在前，新的在后）
    std::sort(contextItems.begin(), contextItems.end(),
        [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

    // 步骤4: 复制到输出结果
    LOG_INFO("BuildContextMsgAdaptive: start copying %zu items to contextMsg", contextItems.size());
    for (const auto& item : contextItems) {
        try {
            rapidjson::Value msgCopy;
            msgCopy.CopyFrom(*item.second, alloc);
            contextMsg.PushBack(msgCopy.Move(), alloc);
        } catch (const std::exception& e) {
            LOG_ERR("BuildContextMsgAdaptive: exception while copying item %d: %s", item.first, e.what());
        } catch (...) {
            LOG_ERR("BuildContextMsgAdaptive: unknown exception while copying item %d", item.first);
        }
    }

    stats->totalMessages = static_cast<int32_t>(contextMsg.Size());
    LOG_INFO("BuildContextMsgAdaptive: complete, fromQaList=%d, fromAbstract=%d, totalMessages=%d, skippedDueToLimit=%d, usedTokens=%d",
             stats->fromQaList, stats->fromAbstract, stats->totalMessages, stats->skippedDueToLimit, usedTokens);
}

} // namespace DMContext