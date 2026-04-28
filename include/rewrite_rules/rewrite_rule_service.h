/*
 • Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

 */

#ifndef REWRITE_RULE_SERVICE_H
#define REWRITE_RULE_SERVICE_H

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <unordered_map>
#include "singleton.h"
#include "service_router/route_context.h"
#include "bean/rule_example.h"
#include "mm_prompttmplt.h"

namespace DMContext {
    // 规则数据模型结构
    // 对话历史项
    struct HistoryItem {
        std::string human;
        std::string ai;

        // 从 JSON 对象解析
        static HistoryItem FromJson(const rapidjson::Value& json) {
            HistoryItem item;
            if (json.HasMember("human") && json["human"].IsString()) {
                item.human = json["human"].GetString();
            }
            if (json.HasMember("ai") && json["ai"].IsString()) {
                item.ai = json["ai"].GetString();
            }
            return item;
        }

        // 转换为 JSON 对象
        void ToJson(rapidjson::Writer<rapidjson::StringBuffer>& writer) const {
            writer.StartObject();
            if (!human.empty()) {
                writer.String("human");
                writer.String(human.c_str());
            }
            if (!ai.empty()) {
                writer.String("ai");
                writer.String(ai.c_str());
            }
            writer.EndObject();
        }

        // 从 JSON 字符串解析
        static HistoryItem ParseFromString(const std::string& jsonStr) {
            HistoryItem item;
            rapidjson::Document doc;
            if (!doc.Parse(jsonStr.c_str()).HasParseError() && doc.IsObject()) {
                item = FromJson(doc);
            }
            return item;
        }

        // 转换为 JSON 字符串
        std::string ToJsonString() const {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
            ToJson(writer);
            return sb.GetString();
        }

        // 从 JSON 数组字符串解析为 vector
        static std::vector<HistoryItem> ParseArrayFromString(const std::string& jsonStr) {
            std::vector<HistoryItem> items;
            rapidjson::Document doc;
            if (doc.Parse(jsonStr.c_str()).HasParseError() || !doc.IsArray()) {
                return items;
            }
            for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
                items.push_back(FromJson(doc[i]));
            }
            return items;
        }
    };

    struct RewriteRuleData {
        std::string human;
        std::string ai;
        std::vector<HistoryItem> history;
        std::string id;
        std::string created_at;
        std::string updated_at;
    };

    // 单条规则处理结果
    struct RuleProcessResult {
        int index;
        std::string status; // "inserted" | "updated" | "error"
        std::string id;
        std::string human;
        std::string error_msg; // 当 status 为 "error" 时的错误信息
    };

    // 批量更新结果
    struct BatchUpdateResult {
        int total;
        int inserted;
        int updated;
        int failed;
        std::vector<RuleProcessResult> items;
    };

    // 查询参数结构体
    struct RewriteRuleQueryParams {
        std::string human;           // 查询文本
        bool isQueryAll = false;     // 是否查询全量
        std::string query_type;      // 查询类型: "recall" 或空
        int top_k = 10;              // 召回数量，默认10
    };

class RewriteRuleService : public Singleton<RewriteRuleService> {
public:
    // 使用 Singleton 模板的 GetInstance
    using Singleton<RewriteRuleService>::GetInstance;

    static std::shared_ptr<RewriteRuleService> CreateInstance();

    // 导入规则接口
    void ImportRules(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 查询规则接口
    void QueryRules(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    // 召回服务接口
    // 如果标量检索精确命中，results 中会返回匹配的 RuleExample 列表
    bool Recall(const std::string& query,
               std::vector<std::shared_ptr<RuleExample>>& results);

    // 标量检索接口（精确匹配）
    // 返回值: true-命中, false-未命中
    // results: 输出参数，命中时返回匹配的 RuleExample 列表，未命中时返回空数组
    bool ScalarSearch(const std::string& query, std::vector<std::shared_ptr<RuleExample>>& results);

    // 重写规则配置结构体
    struct RewriteRuleConfig {
        bool prompt_select_enable{true};              // prompt选择功能使能开关：true开启，false关闭
        float rrf_bm25_ratio{0.8f};                   // RRF中BM25的权重比例
        float rrf_embedding_ratio{0.2f};              // RRF中embedding的权重比例
        int bm25_top_k{5};                             // BM25召回的top_k
        int embedding_top_k{3};                        // embedding召回的top_k
        float first_level_score{0.9f};                // 一级分数阈值
        float second_level_score{0.7f};               // 二级分数阈值
        float smoothing_const{60.0f};                 // RRF平滑常数

        // 设置默认值
        RewriteRuleConfig() = default;
    };
    // 获取重写规则配置
    static const RewriteRuleService::RewriteRuleConfig& GetRewriteRuleConfig();

    // Prompt选择结果结构体
    struct SelectPromptResult {
        std::string promptLevel;                     // 选中的prompt级别: "default", "first_level", "second_level"
        std::vector<std::shared_ptr<RuleExample>> filteredExamples;      // 过滤后的示例数据
    };

    // 根据RRF分数选择prompt和示例数据
    // 输入：Recall方法返回的结果（每条数据已设置RRF分数）
    // 输出：SelectPromptResult（包含选中的prompt级别和过滤后的示例数据）
    SelectPromptResult SelectPrompt(const std::vector<std::shared_ptr<RuleExample>>& results);

     // 组装三段式prompt（前置声明）
    // 输入: promptLevel - 选中的prompt级别, examples - 过滤后的示例数据
    // 输出: 组装好的完整prompt
     std::pair<std::string, std::string> AssemblePrompt(const std::string& query, const std::string& promptLevel,
        const std::vector<std::shared_ptr<DMContext::RuleExample>>& examples);

    // 分数路由映射结构：检索文本 -> (RRF加权分数, 数据对象)
    struct ScoreDataPair {
        float score;
        std::shared_ptr<RuleExample> data;
    };
    using ScoreRouteMap = std::unordered_map<std::string, std::vector<ScoreDataPair>>;

    // 处理prompt的回调函数类型
    using PromptHandler = std::function<std::string(const std::vector<std::shared_ptr<RuleExample>>&)>;

    private:
        // 初始化重写规则配置
        static void InitializeRewriteRuleConfig();

        static std::once_flag rewrite_rule_config_init_once;

        static std::unique_ptr<RewriteRuleConfig> rewrite_rule_config_instance;

        static std::string BuildHistoryContextJson(const std::vector<HistoryItem>& history);

        // 请求体解析
        bool ParseReq4ImportRules(const std::string &requestBody, std::vector <RewriteRuleData> &rules);
        bool ParseQueryRequest(const std::string& requestBody, RewriteRuleQueryParams& params);
        
        // 参数校验
        bool ValidateRuleData(const RewriteRuleData& rule, std::string& errorMsg);
        bool ValidateHumanInput(const std::string& human, std::string& errorMsg);
        
        bool QueryRuleByHuman(const std::string& human, std::vector<RewriteRuleData>& results);
        bool QueryAllRules(std::vector<RewriteRuleData>& results);

        // JSON 生成
        std::string BuildBatchUpdateResponse(const BatchUpdateResult& result);
        std::string BuildQueryResponse(const std::vector<RewriteRuleData>& results);
        std::string BuildErrorResponse(const std::string& code, const std::string& errMsg);


        // 响应返回
        void SendResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context, 
                         const std::string& response);
    };

}  // namespace DMContext

#endif  // REWRITE_RULE_SERVICE_H