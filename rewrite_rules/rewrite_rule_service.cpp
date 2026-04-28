/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

 */

#include "rewrite_rules/rewrite_rule_service.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <memory>
#include <mutex>
#include <unordered_set>

// 相关头文件
#include "logger.h"
#include <chrono>
#include <ctime>
#include "common_define.h"
#include "http_helper.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/pointer.h"
#include "rapidjson/error/en.h"
#include "singleton.h"
#include "database/rag/rag_mgr.h"
#include "datatable/rule_example_tbl.h"
#include "bean/rule_example.h"
#include "concurrent_executor.h"
#include "bean/rule_example.h"
#include "config/config_mgr.h"
#include "datatime_util.h"
#include "config_mgr.h"
#include "model_mgr.h"

using namespace std;
using namespace rapidjson;
using namespace DM::RAG;

namespace DMContext {

static const std::string REWRITE_RULE_REQUEST_RULES = "rules";
static const std::string REWRITE_RULE_REQUEST_HUMAN = "human";
static const std::string REWRITE_RULE_REQUEST_AI = "ai";
static const std::string REWRITE_RULE_REQUEST_HISTORY = "history";

// Prompt三级映射：level -> (desc, examples, output)
// Prompt级别映射: 每个级别包含 (system_prompt_key, user_prompt_key)
static const std::unordered_map<std::string, std::tuple<std::string, std::string>> PROMPT_LEVEL_MAP = {
    {"default", {"rewrite_query_system", ""}},
    {"first_level", {"first_level_rewrite_query_system", "first_level_rewrite_query_user"}},
    {"second_level", {"second_level_rewrite_query_system", "second_level_rewrite_query_user"}}
};

std::shared_ptr<RewriteRuleService> RewriteRuleService::CreateInstance()
{
    return std::make_shared<RewriteRuleService>();
}

// 规则批量更新接口
void RewriteRuleService::ImportRules(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context) {
    LOG_INFO("%s enter", __FUNCTION__);
    
    auto start = chrono::system_clock::now();
    auto body = context->GetRequestBody();
    
    // 解析请求体
    vector<RewriteRuleData> rules;
    if (!ParseReq4ImportRules(body, rules)) {
        string response = BuildErrorResponse("400", "invalid request: failed to parse request body");
        SendResponse(context, response);
        return;
    }
    
    // 参数校验
    BatchUpdateResult result;
    result.total = rules.size();
    result.inserted = 0;
    result.updated = 0;
    result.failed = 0;
    
    for (int i = 0; i < rules.size(); ++i) {
        string errorMsg;
        if (!ValidateRuleData(rules[i], errorMsg)) {
            // 校验失败，立即停止批次的处理
            result.failed++;
            RuleProcessResult item;
            item.index = i;
            item.status = "error";
            item.id = "";
            item.human = rules[i].human;
            item.error_msg = errorMsg;
            result.items.push_back(item);
            
            LOG_ERR("Batch processing stopped at rule[%d] due to validation error: %s", i, errorMsg.c_str());
            break;
        }
        
        std::string id, processResult;
        std::string historyContext = BuildHistoryContextJson(rules[i].history);
        if (RuleExampleTbl::InsertOrUpdateWithDedupe(rules[i].human, rules[i].ai, historyContext, id, processResult)) {
            if (processResult == "inserted") {
                result.inserted++;
            } else if (processResult == "updated") {
                result.updated++;
            }
            RuleProcessResult item;
            item.index = i;
            item.status = processResult;
            item.id = id;
            item.human = rules[i].human;
            item.error_msg = ""; // 成功操作没有错误信息
            result.items.push_back(item);
        } else {
            // 数据库操作失败，也立即停止批次的处理
            result.failed++;
            RuleProcessResult item;
            item.index = i;
            item.status = "error";
            item.id = "";
            item.human = rules[i].human;
            item.error_msg = "database operation failed";
            result.items.push_back(item);
            
            LOG_ERR("Batch processing stopped at rule[%d] due to database operation failure", i);
            break;
        }
    }
    
    // 构建响应
    string response = BuildBatchUpdateResponse(result);
    SendResponse(context, response);
    
    auto end = chrono::system_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    LOG_INFO("[RewriteRule TIME-CONSUMING] ImportRules %d ms, rules=%d, inserted=%d, updated=%d, failed=%d",
             duration.count(), result.total, result.inserted, result.updated, result.failed);
}

// 规则查询接口
void RewriteRuleService::QueryRules(const shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    LOG_INFO("%s enter", __FUNCTION__);

    auto start = chrono::system_clock::now();
    auto body = context->GetRequestBody();

    // 解析请求体
    RewriteRuleQueryParams params;
    if (!ParseQueryRequest(body, params)) {
        string response = BuildErrorResponse("400", "invalid request: failed to parse request body");
        SendResponse(context, response);
        return;
    }

    // 如果 query_type 为 "recall"，则调用召回服务
    if (params.query_type == "recall") {
        LOG_INFO("QueryRules: using recall service, query=%s, top_k=%d", params.human.c_str(), params.top_k);

        // 调用召回服务（内部会先进行标量检索，如果命中则直接返回结果）
        vector<shared_ptr<RuleExample>> ruleExamples;
        bool recallSuccess = RewriteRuleService::GetInstance()->Recall(params.human, ruleExamples);

        if (!recallSuccess) {
            string response = BuildErrorResponse("500", "recall service failed");
            SendResponse(context, response);
            return;
        }

        // 检查 Recall 方法内部是否通过标量检索精确命中
        if (!ruleExamples.empty()) {
            // 标量检索精确命中，直接返回改写结果
            std::string rewrittenQuery = ruleExamples[0]->GetNormalizedText();
            StringBuffer sb;
            Writer<StringBuffer> writer(sb);
            writer.StartObject();
            writer.String("need_rewrite");
            writer.Int(1);
            writer.String("rewritten_query");
            writer.String(rewrittenQuery.c_str());
            writer.EndObject();

            string response = sb.GetString();
            SendResponse(context, response);

            auto end = chrono::system_clock::now();
            auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
            LOG_INFO("[RewriteRule TIME-CONSUMING] QueryRules(ScalarHit) %d ms, query=%s, rewritten_query=%s",
                     duration.count(), params.human.c_str(), rewrittenQuery.c_str());
            return;
        }

        // 将召回结果转换为 RewriteRuleData
        vector<RewriteRuleData> results;
        for (const auto& example : ruleExamples) {
            if (example == nullptr) {
                continue;
            }
            RewriteRuleData rule;
            rule.id = example->GetId();
            rule.human = example->GetExampleText();
            rule.ai = example->GetNormalizedText();
            rule.created_at = example->GetCreatedAt();
            rule.updated_at = example->GetUpdatedAt();
            results.push_back(rule);
        }

        // 构建响应
        string response = BuildQueryResponse(results);
        SendResponse(context, response);

        auto end = chrono::system_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
        LOG_INFO("[RewriteRule TIME-CONSUMING] QueryRules(Recall) %d ms, query=%s, top_k=%d, results=%d",
                 duration.count(), params.human.c_str(), params.top_k, (int)results.size());
        return;
    }

    // 参数校验（全量查询时跳过校验）
    string errorMsg;
    if (!params.isQueryAll && !ValidateHumanInput(params.human, errorMsg)) {
        string response = BuildErrorResponse("400", "invalid request: " + errorMsg);
        SendResponse(context, response);
        return;
    }

    // 查询规则
    vector<RewriteRuleData> results;
    bool querySuccess = false;
    if (params.isQueryAll) {
        querySuccess = QueryAllRules(results);
    } else {
        querySuccess = QueryRuleByHuman(params.human, results);
    }

    if (!querySuccess) {
        string response = BuildErrorResponse("500", "database query failed");
        SendResponse(context, response);
        return;
    }

    // 构建响应
    string response = BuildQueryResponse(results);
    SendResponse(context, response);

    auto end = chrono::system_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    LOG_INFO("[RewriteRule TIME-CONSUMING] QueryRules %d ms, isQueryAll=%s, human=%s, results=%d",
             duration.count(), params.isQueryAll ? "true" : "false", params.human.c_str(), (int)results.size());
}
// 解析批量更新请求体
bool RewriteRuleService::ParseReq4ImportRules(const std::string& requestBody, std::vector<RewriteRuleData>& rules) {
    Document doc;
    if (doc.Parse(requestBody.c_str()).HasParseError()) {
        LOG_ERR("ParseReq4ImportRules failed: JSON parsing error: %s", GetParseError_En(doc.GetParseError()));
        return false;
    }
    
    if (!doc.IsObject()) {
        LOG_ERR("ParseReq4ImportRules: Item 1 Invalid request format, not a valid JSON object");
        return false;
    }
    
    if (!doc.HasMember(REWRITE_RULE_REQUEST_RULES.c_str()) || !doc[REWRITE_RULE_REQUEST_RULES.c_str()].IsArray()) {
        LOG_ERR("ParseReq4ImportRules: Item 1 Missing required %s array field", REWRITE_RULE_REQUEST_RULES.c_str());
        return false;
    }
    
    const Value& rulesArray = doc[REWRITE_RULE_REQUEST_RULES.c_str()];
    int validRuleCount = 0;
    
    for (SizeType i = 0; i < rulesArray.Size(); ++i) {
        const int ruleIndex = (int)i;
        const Value& ruleObj = rulesArray[i];
        
        if (!ruleObj.IsObject()) {
            LOG_ERR("ParseReq4ImportRules: Item %d Format error, not a valid JSON object", ruleIndex + 1);
            continue;
        }
        
        RewriteRuleData rule;

        // 解析 human (必填)
        if (!ruleObj.HasMember(REWRITE_RULE_REQUEST_HUMAN.c_str())) {
            LOG_ERR("ParseReq4ImportRules: Item %d Missing required %s field", ruleIndex + 1, REWRITE_RULE_REQUEST_HUMAN.c_str());
            continue;
        }
        if (!ruleObj[REWRITE_RULE_REQUEST_HUMAN.c_str()].IsString()) {
            LOG_ERR("ParseReq4ImportRules: Item %d %s field is not a string type", ruleIndex + 1, REWRITE_RULE_REQUEST_HUMAN.c_str());
            continue;
        }
        if (ruleObj[REWRITE_RULE_REQUEST_HUMAN.c_str()].GetStringLength() == 0) {
            LOG_ERR("ParseReq4ImportRules: Item %d %s field is empty", ruleIndex + 1, REWRITE_RULE_REQUEST_HUMAN.c_str());
            continue;
        }
        rule.human = ruleObj[REWRITE_RULE_REQUEST_HUMAN.c_str()].GetString();

        // 解析 ai (必填)
        if (!ruleObj.HasMember(REWRITE_RULE_REQUEST_AI.c_str())) {
            LOG_ERR("ParseReq4ImportRules: Item %d Missing required %s field", ruleIndex + 1, REWRITE_RULE_REQUEST_AI.c_str());
            continue;
        }
        if (!ruleObj[REWRITE_RULE_REQUEST_AI.c_str()].IsString()) {
            LOG_ERR("ParseReq4ImportRules: Item %d %s field is not a string type", ruleIndex + 1, REWRITE_RULE_REQUEST_AI.c_str());
            continue;
        }
        if (ruleObj[REWRITE_RULE_REQUEST_AI.c_str()].GetStringLength() == 0) {
            LOG_ERR("ParseReq4ImportRules: Item %d %s field is empty", ruleIndex + 1, REWRITE_RULE_REQUEST_AI.c_str());
            continue;
        }
        rule.ai = ruleObj[REWRITE_RULE_REQUEST_AI.c_str()].GetString();
        
        // 解析 history (可选)
        if (!ruleObj.HasMember(REWRITE_RULE_REQUEST_HISTORY.c_str())) {
            rules.push_back(rule);
            validRuleCount++;
            continue;
        }

        const Value& historyArray = ruleObj[REWRITE_RULE_REQUEST_HISTORY.c_str()];
        if (!historyArray.IsArray()) {
            LOG_ERR("ParseReq4ImportRules: Item %d [%s] field is not an array type",
                    ruleIndex + 1, REWRITE_RULE_REQUEST_HISTORY.c_str());
            continue;
        }

        for (SizeType j = 0; j < historyArray.Size(); ++j) {
            const int historyIndex = (int)j;
            const Value& historyItem = historyArray[j];

            if (!historyItem.IsObject()) {
                LOG_ERR("ParseReq4ImportRules: Item %d.%s[%d] History item is not a valid JSON object",
                        ruleIndex + 1, REWRITE_RULE_REQUEST_HISTORY.c_str(), historyIndex + 1);
                continue;
            }

            bool hasHuman = historyItem.HasMember(REWRITE_RULE_REQUEST_HUMAN.c_str())
                && historyItem[REWRITE_RULE_REQUEST_HUMAN.c_str()].IsString()
                && historyItem[REWRITE_RULE_REQUEST_HUMAN.c_str()].GetStringLength() > 0;
            bool hasAi = historyItem.HasMember(REWRITE_RULE_REQUEST_AI.c_str())
                && historyItem[REWRITE_RULE_REQUEST_AI.c_str()].IsString()
                && historyItem[REWRITE_RULE_REQUEST_AI.c_str()].GetStringLength() > 0;

            // human 和 ai 至少要有一个
            if (!hasHuman && !hasAi) {
                LOG_ERR("ParseReq4ImportRules: Item %d.history[%d] must have at least human or ai field",
                        ruleIndex + 1, historyIndex + 1);
                continue;
            }

            // 使用 HistoryItem 结构，允许只有 human 或只有 ai
            HistoryItem historyEntry;
            if (hasHuman) {
                historyEntry.human = historyItem[REWRITE_RULE_REQUEST_HUMAN.c_str()].GetString();
            }
            if (hasAi) {
                historyEntry.ai = historyItem[REWRITE_RULE_REQUEST_AI.c_str()].GetString();
            }
            rule.history.push_back(historyEntry);
        }

        rules.push_back(rule);
        validRuleCount++;
    }
    LOG_INFO("ParseReq4ImportRules: import finished, total items, successfully insert %d", (int)rulesArray.Size(), validRuleCount);
    return validRuleCount > 0;
}

// 解析查询请求体
// 返回值: true-解析成功, false-解析失败
// params: 输出参数，包含 human, isQueryAll, query_type, top_k
bool RewriteRuleService::ParseQueryRequest(const string& requestBody, RewriteRuleQueryParams& params) {
    params.isQueryAll = false;
    params.query_type = "";
    params.top_k = 10;  // 默认值

    // 请求体为空时，查询全量
    if (requestBody.empty()) {
        params.isQueryAll = true;
        return true;
    }

    Document doc;
    if (doc.Parse(requestBody.c_str()).HasParseError()) {
        LOG_ERR("ParseQueryRequest failed: %s", GetParseError_En(doc.GetParseError()));
        return false;
    }

    // 解析 query_type 字段（可选）
    if (doc.HasMember("query_type") && doc["query_type"].IsString()) {
        params.query_type = doc["query_type"].GetString();
    }

    // 解析 top_k 字段（可选）
    if (doc.HasMember("top_k") && doc["top_k"].IsInt()) {
        params.top_k = doc["top_k"].GetInt();
    }

    // 没有human字段时，查询全量
    if (!doc.IsObject() || !doc.HasMember("human")) {
        params.isQueryAll = true;
        return true;
    }

    // human字段存在但不是字符串类型，返回失败
    if (!doc["human"].IsString()) {
        LOG_ERR("ParseQueryRequest: human field is not a string type");
        return false;
    }

    params.human = doc["human"].GetString();
    return true;
}

// 参数校验规则数据
bool RewriteRuleService::ValidateRuleData(const RewriteRuleData& rule, string& errorMsg) {
    // human 字段校验
    if (rule.human.empty()) {
        errorMsg = "field \"human\" is empty";
        return false;
    }
    
    // ai 字段校验
    if (rule.ai.empty()) {
        errorMsg = "field \"ai\" is empty";
        return false;
    }
    
    // 长度校验
    if (rule.human.length() > 2048) {
        errorMsg = "field \"human\" exceeds maximum length (2048)";
        return false;
    }
    
    if (rule.ai.length() > 2048) {
        errorMsg = "field \"ai\" exceeds maximum length (2048)";
        return false;
    }
    
    // history 轮数校验
    if (rule.history.size() > 20) {
        errorMsg = "field \"history\" exceeds maximum turns (20)";
        return false;
    }
    
    // history 中每项校验
    for (size_t i = 0; i < rule.history.size(); ++i) {
        const auto& historyItem = rule.history[i];
        
        // human 字段校验
        if (historyItem.human.empty()) {
            errorMsg = "history[" + to_string(i) + "].human is empty";
            return false;
        }
        
        if (historyItem.human.length() > 2048) {
            errorMsg = "history[" + to_string(i) + "].human exceeds maximum length (2048)";
            return false;
        }
        
        // ai 字段校验
        if (historyItem.ai.empty()) {
            errorMsg = "history[" + to_string(i) + "].ai is empty";
            return false;
        }
        
        if (historyItem.ai.length() > 2048) {
            errorMsg = "history[" + to_string(i) + "].ai exceeds maximum length (2048)";
            return false;
        }
    }
    
    return true;
}

// 参数校验 human 输入
bool RewriteRuleService::ValidateHumanInput(const string& human, string& errorMsg) {
    if (human.empty()) {
        errorMsg = "field \"human\" is empty";
        return false;
    }
    
    if (human.length() > 2048) {
        errorMsg = "field \"human\" exceeds maximum length (2048)";
        return false;
    }
    
    return true;
}

string RewriteRuleService::BuildHistoryContextJson(const std::vector<HistoryItem>& history) {
    // 使用 HistoryItem 的 ToJson 方法构建 JSON 数组
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartArray();

    for (const auto& turn : history) {
        turn.ToJson(writer);
    }

    writer.EndArray();
    return sb.GetString();
}

bool RewriteRuleService::QueryRuleByHuman(const string& human, vector<RewriteRuleData>& results) {
    // 调用 RuleExampleTbl 的查询方法
    vector<std::shared_ptr<RuleExample>> queryResults;
    bool success = RuleExampleTbl::QueryRuleByHuman(human, queryResults);
    if (!success) {
        LOG_ERR("QueryRuleByHuman: query failed");
        return false;
    }
    
    // 转换查询结果
    results.clear();
    for (const auto& ruleExample : queryResults) {
        if (ruleExample == nullptr) {
            continue;
        }
        
        RewriteRuleData rule;
        rule.id = ruleExample->GetId();
        rule.human = ruleExample->GetExampleText();
        rule.ai = ruleExample->GetNormalizedText();
        rule.created_at = ruleExample->GetCreatedAt();
        rule.updated_at = ruleExample->GetUpdatedAt();
        
        // 解析 history_context
        if (!ruleExample->GetHistoryContext().empty()) {
            Document historyDoc;
            if (historyDoc.Parse(ruleExample->GetHistoryContext().c_str()).HasParseError()) {
                LOG_ERR("QueryRuleByHuman: failed to parse history_context, id=%s", rule.id.c_str());
                continue;
            }
            
            if (historyDoc.IsArray()) {
                for (SizeType i = 0; i < historyDoc.Size(); ++i) {
                    const Value& turn = historyDoc[i];
                    if (turn.IsObject() && turn.HasMember("human") && turn.HasMember("ai") &&
                        turn["human"].IsString() && turn["ai"].IsString()) {
                        // 使用新的 HistoryItem 结构
                        HistoryItem historyEntry;
                        historyEntry.human = turn["human"].GetString();
                        historyEntry.ai = turn["ai"].GetString();
                        rule.history.push_back(historyEntry);
                    }
                }
            }
        }
        
        results.push_back(rule);
    }
    
    LOG_INFO("QueryRuleByHuman: found %zu rules for human=%s", results.size(), human.c_str());
    return true;
}

bool RewriteRuleService::QueryAllRules(vector<RewriteRuleData>& results) {
    // 调用 RuleExampleTbl 的查询全量方法
    vector<std::shared_ptr<RuleExample>> queryResults;
    bool success = RuleExampleTbl::QueryAllRules(queryResults);
    if (!success) {
        LOG_ERR("QueryAllRules: query failed");
        return false;
    }

    // 转换查询结果
    results.clear();
    for (const auto& ruleExample : queryResults) {
        if (ruleExample == nullptr) {
            continue;
        }

        RewriteRuleData rule;
        rule.id = ruleExample->GetId();
        rule.human = ruleExample->GetExampleText();
        rule.ai = ruleExample->GetNormalizedText();
        rule.created_at = ruleExample->GetCreatedAt();
        rule.updated_at = ruleExample->GetUpdatedAt();

        // 解析 history_context
        if (!ruleExample->GetHistoryContext().empty()) {
            Document historyDoc;
            if (historyDoc.Parse(ruleExample->GetHistoryContext().c_str()).HasParseError()) {
                LOG_ERR("QueryAllRules: failed to parse history_context, id=%s", rule.id.c_str());
                continue;
            }

            if (historyDoc.IsArray()) {
                for (SizeType i = 0; i < historyDoc.Size(); ++i) {
                    const Value& turn = historyDoc[i];
                    if (turn.IsObject() && turn.HasMember("human") && turn.HasMember("ai") &&
                        turn["human"].IsString() && turn["ai"].IsString()) {
                        HistoryItem historyEntry;
                        historyEntry.human = turn["human"].GetString();
                        historyEntry.ai = turn["ai"].GetString();
                        rule.history.push_back(historyEntry);
                    }
                }
            }
        }

        results.push_back(rule);
    }

    LOG_INFO("QueryAllRules: found %zu rules", results.size());
    return true;
}

// 标量检索实现（精确匹配）
// 返回值: true-命中, false-未命中
// results: 输出参数，命中时返回查询到的 RuleExample 列表，未命中时返回空数组
bool RewriteRuleService::ScalarSearch(const std::string& query,
                                       std::vector<std::shared_ptr<RuleExample>>& results) {
    // 标量检索不限制数量，返回最多100条结果
    bool success = RuleExampleTbl::QueryRuleByHuman(query, results, 100);
    if (!success || results.empty()) {
        LOG_INFO("ScalarSearch: no exact match found for query=%s", query.c_str());
        // 返回空数组
        results.clear();
        return false;
    }

    // 精确匹配命中，返回所有匹配结果
    LOG_INFO("ScalarSearch: exact match found, query=%s, result_count=%zu",
             query.c_str(), results.size());
    return true;
}

// 召回服务实现
bool RewriteRuleService::Recall(const std::string& query,
                               std::vector<std::shared_ptr<RuleExample>>& results)
{
    // 获取配置参数
    auto& config = GetRewriteRuleConfig();

    // 标量检索未命中，继续进行近似检索（BM25 + 向量检索）

    // === 需求1: 双路检索分数计算 ===
    // 分别存储BM25和Vector检索的结果，包含排名信息
    std::vector<std::pair<std::shared_ptr<RuleExample>, int>> bm25Results;  // (结果, 排名)
    std::vector<std::pair<std::shared_ptr<RuleExample>, int>> vectorResults; // (结果, 排名)

    // 1. BM25检索
    {
        auto queryExample = std::make_shared<DMContext::RuleExample>();
        queryExample->SetQuery(query);
        auto rawResults = DMContext::RuleExampleTbl::QueryRuleExampleByExampleText(queryExample, config.bm25_top_k);
        for (size_t i = 0; i < rawResults.size(); ++i) {
            bm25Results.emplace_back(rawResults[i], static_cast<int>(i) + 1);
        }
        LOG_INFO("Recall: BM25 retrieved %zu results", bm25Results.size());
    }

    // 2. 向量检索
    {
        auto queryExample = std::make_shared<DMContext::RuleExample>();
        queryExample->SetQuery(query);
        auto rawResults = DMContext::RuleExampleTbl::QueryRuleExampleByExampleVector(queryExample, config.embedding_top_k);
        for (size_t i = 0; i < rawResults.size(); ++i) {
            vectorResults.emplace_back(rawResults[i], static_cast<int>(i) + 1);
        }
        LOG_INFO("Recall: Vector retrieved %zu results", vectorResults.size());
    }

    // === 需求2: 分数路由 ===
    // 构建分数路由映射: key为检索文本字符串，value为pair(分数, 数据对象)
    // 使用RRF (Reciprocal Rank Fusion) 计算加权分数
    // RRF分数 = bm25_ratio * sum(1/(smoothing_const + rank)) + embedding_ratio * sum(1/(smoothing_const + rank))
    ScoreRouteMap scoreRouteMap;

    // 计算BM25的RRF分数
    for (const auto& [example, rank] : bm25Results) {
        if (!example || example->GetId().empty()) continue;
        float rrfScore = 1.0f / (config.smoothing_const + rank);
        std::string key = "BM25_" + example->GetId();
        scoreRouteMap[key].push_back({rrfScore, example});
    }

    // 计算Vector的RRF分数
    for (const auto& [example, rank] : vectorResults) {
        if (!example || example->GetId().empty()) continue;
        float rrfScore = 1.0f / (config.smoothing_const + rank);
        std::string key = "Vector_" + example->GetId();
        scoreRouteMap[key].push_back({rrfScore, example});
    }

    // 计算每个结果的综合RRF分数
    std::map<std::string, float> finalScores;  // id -> 综合分数
    std::unordered_map<std::string, std::shared_ptr<RuleExample>> idToExample;

    for (const auto& [key, scorePairs] : scoreRouteMap) {
        // key格式为 "BM25_id" 或 "Vector_id"，提取id
        size_t underscorePos = key.find('_');
        if (underscorePos == std::string::npos) continue;

        std::string searchType = key.substr(0, underscorePos);
        std::string id = key.substr(underscorePos + 1);

        float weight = (searchType == "BM25") ? config.rrf_bm25_ratio : config.rrf_embedding_ratio;

        for (const auto& pair : scorePairs) {
            float weightedScore = weight * pair.score;
            finalScores[id] += weightedScore;
            if (idToExample.find(id) == idToExample.end()) {
                idToExample[id] = pair.data;
            }
        }
    }

    // 按分数排序结果
    std::vector<std::pair<std::string, float>> sortedScores(finalScores.begin(), finalScores.end());
    std::sort(sortedScores.begin(), sortedScores.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // 填充结果
    for (const auto& [id, score] : sortedScores) {
        auto it = idToExample.find(id);
        if (it != idToExample.end()) {
            it->second->SetRRFScore(score);
            results.push_back(it->second);
            LOG_INFO("Recall: result id=%s, rrf_score=%.4f", id.c_str(), score);
        }
    }

    LOG_INFO("Recall: final results count=%zu", results.size());

    return !results.empty();
}

// 构建批量更新响应
string RewriteRuleService::BuildBatchUpdateResponse(const BatchUpdateResult& result) {
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    
    writer.StartObject();
    writer.String("code");
    writer.String("0");
    writer.String("errMsg");
    writer.String("success");
    
    writer.String("data");
    writer.StartObject();
    
    writer.String("total");
    writer.Int(result.total);
    writer.String("inserted");
    writer.Int(result.inserted);
    writer.String("updated");
    writer.Int(result.updated);
    writer.String("failed");
    writer.Int(result.failed);
    
    writer.String("items");
    writer.StartArray();
    
    for (const auto& item : result.items) {
        writer.StartObject();
        writer.String("index");
        writer.Int(item.index);
        writer.String("status");
        writer.String(item.status.c_str());
        writer.String("id");
        writer.String(item.id.empty() ? "" : item.id.c_str());
        writer.String("human");
        writer.String(item.human.c_str());
        if (!item.error_msg.empty()) {
            writer.String("error_msg");
            writer.String(item.error_msg.c_str());
        }
        writer.EndObject();
    }
    
    writer.EndArray();
    writer.EndObject();
    writer.EndObject();
    
    return sb.GetString();
}

// 构建查询响应
string RewriteRuleService::BuildQueryResponse(const vector<RewriteRuleData>& results) {
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    
    writer.StartObject();
    writer.String("code");
    writer.String("0");
    writer.String("errMsg");
    writer.String("success");
    
    writer.String("data");
    writer.StartObject();
    
    writer.String("total");
    writer.Int(results.size());
    writer.String("items");
    writer.StartArray();
    
    for (const auto& rule : results) {
        writer.StartObject();
        writer.String("id");
        writer.String(rule.id.c_str());
        writer.String("human");
        writer.String(rule.human.c_str());
        writer.String("ai");
        writer.String(rule.ai.c_str());
        
        // 添加 history 字段
        writer.String("history");
        writer.StartArray();
        
        for (const auto& turn : rule.history) {
            writer.StartObject();
            writer.String("human");
            writer.String(turn.human.c_str());
            writer.String("ai");
            writer.String(turn.ai.c_str());
            writer.EndObject();
        }
        
        writer.EndArray();
        writer.String("created_at");
        writer.String(rule.created_at.c_str());
        writer.String("updated_at");
        writer.String(rule.updated_at.c_str());
        
        writer.EndObject();
    }
    
    writer.EndArray();
    writer.EndObject();
    writer.EndObject();
    
    return sb.GetString();
}

// 构建错误响应
string RewriteRuleService::BuildErrorResponse(const string& code, const string& errMsg) {
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    
    writer.StartObject();
    writer.String("code");
    writer.String(code.c_str());
    writer.String("errMsg");
    writer.String(errMsg.c_str());
    writer.String("data");
    writer.Null();
    writer.EndObject();
    
    return sb.GetString();
}

// 发送响应
void RewriteRuleService::SendResponse(const shared_ptr<CMFrm::ServiceRouter::Context>& context,
                                     const string& response) {
    HttpHelper::WriteResponse(context, response, CMFrm::COM::HttpStatus::HTTP200);
}

// 静态成员初始化
std::once_flag RewriteRuleService::rewrite_rule_config_init_once;
std::unique_ptr<RewriteRuleService::RewriteRuleConfig> RewriteRuleService::rewrite_rule_config_instance;

// 获取重写规则配置
const RewriteRuleService::RewriteRuleConfig& RewriteRuleService::GetRewriteRuleConfig()
{
    std::call_once(rewrite_rule_config_init_once, &RewriteRuleService::InitializeRewriteRuleConfig);
    return *rewrite_rule_config_instance;
}

void RewriteRuleService::InitializeRewriteRuleConfig()
{
    try {
        // 创建配置实例
        rewrite_rule_config_instance = std::make_unique<RewriteRuleConfig>();
        // 从配置管理器获取 rewrite_rule_config 配置参数
        auto rewriteConfigParams = DMContext::ConfigMgr::GetInstance()->GetRewriteRuleConfigParams();

        // 解析使能开关
        auto it_enable = rewriteConfigParams.find("prompt_select_enable");
        if (it_enable != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->prompt_select_enable = (it_enable->second == "1");
        }

        // 解析 rewrite_rule_config 配置
        auto it_rrf_bm25_ratio = rewriteConfigParams.find("rrf_bm25_ratio");
        if (it_rrf_bm25_ratio != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->rrf_bm25_ratio = std::stof(it_rrf_bm25_ratio->second);
        }

        auto it_rrf_embedding_ratio = rewriteConfigParams.find("rrf_embedding_ratio");
        if (it_rrf_embedding_ratio != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->rrf_embedding_ratio = std::stof(it_rrf_embedding_ratio->second);
        }

        auto it_bm25_top_k = rewriteConfigParams.find("bm25_top_k");
        if (it_bm25_top_k != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->bm25_top_k = std::stoi(it_bm25_top_k->second);
        }

        auto it_embedding_top_k = rewriteConfigParams.find("embedding_top_k");
        if (it_embedding_top_k != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->embedding_top_k = std::stoi(it_embedding_top_k->second);
        }

        auto it_first_level_score = rewriteConfigParams.find("first_level_score");
        if (it_first_level_score != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->first_level_score = std::stof(it_first_level_score->second);
        }

        auto it_second_level_score = rewriteConfigParams.find("second_level_score");
        if (it_second_level_score != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->second_level_score = std::stof(it_second_level_score->second);
        }

        auto it_smoothing_const = rewriteConfigParams.find("smoothing_const");
        if (it_smoothing_const != rewriteConfigParams.end()) {
            rewrite_rule_config_instance->smoothing_const = std::stof(it_smoothing_const->second);
        }

        LOG_INFO(
                "RewriteRuleService::InitializeRewriteRuleConfig - rewrite_rule_config parsed: prompt_select_enable: %d, "
                "rrf_bm25_ratio: %.2f, rrf_embedding_ratio: %.2f, bm25_top_k: %d, embedding_top_k: %d, "
                "first_level_score: %.2f, second_level_score: %.2f, smoothing_const: %.2f",
                rewrite_rule_config_instance->prompt_select_enable,
                rewrite_rule_config_instance->rrf_bm25_ratio,
                rewrite_rule_config_instance->rrf_embedding_ratio,
                rewrite_rule_config_instance->bm25_top_k,
                rewrite_rule_config_instance->embedding_top_k,
                rewrite_rule_config_instance->first_level_score,
                rewrite_rule_config_instance->second_level_score,
                rewrite_rule_config_instance->smoothing_const);
    } catch (...) {
        LOG_ERR("RewriteRuleService::InitializeRewriteRuleConfig - Exception during config initialization, using defaults");
        // 异常时使用默认值（已在结构体定义中设置）
    }
}

// 根据RRF分数选择prompt和示例数据
RewriteRuleService::SelectPromptResult RewriteRuleService::SelectPrompt(
    const std::vector<std::shared_ptr<RuleExample>>& results)
{
    SelectPromptResult selectResult;
    selectResult.promptLevel = "default";

    auto& config = GetRewriteRuleConfig();
    // 检查功能使能开关
    if (!config.prompt_select_enable) {
        LOG_INFO("SelectPrompts: prompt_select_enable is disabled, using default prompt");
        return selectResult;
    }

    // 规则1: 如果Recall返回结果为空，则直接返回默认prompt
    if (results.empty()) {
        LOG_INFO("SelectPrompts: recall results is empty, using default prompt");
        return selectResult;
    }

    // 打印每条数据的RRF分数
    for (const auto& example : results) {
        if (example) {
            LOG_INFO("SelectPrompts print rrf score query=%s: exampleText=%s, rrf_score=%.6f, id=%s", example->GetQuery().c_str(),
                     example->GetExampleText().c_str(), example->GetRRFScore(), example->GetId().c_str());
        }
    }

    // 统计各分数区间的数据量
    int firstLevelCount = 0;
    int secondLevelCount = 0;

    for (const auto& example : results) {
        if (!example) continue;
        float score = example->GetRRFScore();
        if (score >= config.first_level_score) {
            firstLevelCount++;
        }
//        else if (score >= config.second_level_score) {
//            secondLevelCount++;
//        }
    }

    LOG_INFO("SelectPrompts: score distribution - first_level: %d, second_level: %d",
             firstLevelCount, secondLevelCount);

    // 规则2: 使用first_level_prompt
    if (firstLevelCount > 0) {
        LOG_INFO("SelectPrompts: using first_level prompt, threshold=%.4f", config.first_level_score);
        selectResult.promptLevel = "first_level";
        for (const auto& example : results) {
            if (example && example->GetRRFScore() >= config.first_level_score) {
                selectResult.filteredExamples.push_back(example);
            }
        }
        return selectResult;
    }

    // 规则3: 使用second_level_prompt
    if (secondLevelCount > 0) {
        LOG_INFO("SelectPrompts: using second_level prompt, threshold=[%.4f, %.4f)",
                 config.second_level_score, config.first_level_score);
        selectResult.promptLevel = "second_level";
        for (const auto& example : results) {
            if (example) {
                float score = example->GetRRFScore();
                if (score >= config.second_level_score && score < config.first_level_score) {
                    selectResult.filteredExamples.push_back(example);
                }
            }
        }
        return selectResult;
    }

    // 规则4: 使用default prompt
    LOG_INFO("SelectPrompts: all rrf_score < second_level_score(%.2f), using default prompt",
             config.second_level_score);
    return selectResult;
}


// 组装三段式prompt: desc + examples + output
std::pair<std::string, std::string> RewriteRuleService::AssemblePrompt(const std::string& query, const std::string& promptLevel,
    const std::vector<std::shared_ptr<DMContext::RuleExample>>& examples)
{
    // 根据promptLevel从静态map获取system和user prompt key
    auto it = PROMPT_LEVEL_MAP.find(promptLevel);
    std::string systemPromptKey, userPromptKey;
    if (it != PROMPT_LEVEL_MAP.end()) {
        std::tie(systemPromptKey, userPromptKey) = it->second;
    } else {
        std::tie(systemPromptKey, userPromptKey) = PROMPT_LEVEL_MAP.at("default");
    }

    LOG_INFO("AssemblePrompt: level=%s, system=%s, user=%s",
             promptLevel.c_str(), systemPromptKey.c_str(), userPromptKey.c_str());
    // 获取system和user prompt内容
    std::string systemPrompt = ModelMgr::GetInstance()->GetPrompt(systemPromptKey);
    std::string fewShow;
    // 将召回的示例追加到user prompt后面
    if (!examples.empty()) {
        for (size_t i = 0; i < examples.size(); ++i) {
            const auto& example = examples[i];
            fewShow += "### 示例" + std::to_string(i + 1) + "\n";
            if (!example->GetHistoryContext().empty()) {
                fewShow += "上下文：" + example->GetHistoryContext() + "\n";
                LOG_INFO("AssemblePrompt: context=%s, rrf_score=%.4f",
                         example->GetHistoryContext().c_str(), example->GetRRFScore());
            }
            fewShow += "用户输入：" + example->GetExampleText() + "\n";
            fewShow += "用户完整意图：{\"用户完整意图\":\"" + example->GetNormalizedText() + "\"}\n";
        }
    }

    const std::string promptName = "first_level_rewrite_query_user";
    auto userPrompt = mmsdk::MMPromptTemplate(mmsdk::MMPromptTmpltKey{
            .name = promptName,
            .owner = SERVICE_NAME,
    });

    if (!userPromptKey.empty()) {
        userPrompt.SetContent(ModelMgr::GetInstance()->GetPrompt(userPromptKey));
        userPrompt.RenderPrompt({{"few_shots", fewShow}});
        userPrompt.RenderPrompt({{"query", query}});
        return std::make_pair(userPrompt.GetContent(), systemPrompt);
    }

    return std::make_pair(query, systemPrompt);
}
}  // namespace DMContext