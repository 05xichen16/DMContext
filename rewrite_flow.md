# DMContext 上下文改写（Rewrite Query）流程详解

> 范围：`/api/v1/contexts/query` 第一次查询时的"改写"分支，从入口到 LLM 返回的完整链路。
> 用途：归档学习材料，方便后续维护、调参和定位 bug。

---

## 1. 改写要解决什么问题

用户当前的一句话往往依赖前几轮对话才说得清楚，例如：

| 历史 | 当前用户输入 | 期望改写结果 |
|---|---|---|
| 我想看黄渤的电影 | 2025 年的 | 我想看黄渤 2025 年的电影 |
| 我在纠结两部电影，一部喜剧，一部悬疑 | 前者好看吗？ | 偏喜剧的好看吗？ |
| 国庆假期上海的天气咋样 | 明天怎么样？ | 上海明天的天气怎么样？ |

改写器的目标：**把"依赖上下文的句子"改成"脱离上下文也能被独立理解的句子"**，方便后续做记忆召回、检索和大模型回答。

---

## 2. 触发条件

只有 `/api/v1/contexts/query` 接口、**且**满足以下条件才会触发：

- 走 `ExecuteFirstQuery`（即请求体**没有** `response_id`，是第一次查询）；
- 请求体 `rewrite_query == true`（默认 `false`）。

第二次查询（带 `response_id`）**不会再改写**——它会从缓存里取出第一次的 `rewrittenQuery` 直接用。

代码位置：`context_service.cpp:2688`
```cpp
if (params.isRewritequery) {
    rewrittenQuery = RewriteQueryFromUserConversation(conversations, params.content, rewriteInput);
}
```

---

## 3. 总流程图

```
ExecuteFirstQuery                                          (context_service.cpp:2653)
  │
  │  此时已经从 DB 拉到了 conversations（最近 N 轮历史对话）
  │
  └─► params.isRewritequery == true ?
        │
        │ 是
        ▼
  RewriteQueryFromUserConversation(conversations, query)   (context_service.cpp:1349)
    │
    ├── ① BuildRewriteContext       ─► 1+N 算法压缩历史    (1225)
    │                                  给 LLM 看的"对话上下文"
    │
    ├── ② RewriteRuleService::ScalarSearch  精确匹配规则库 (rewrite_rule_service.cpp:613)
    │     │
    │     ├ 命中 → return query      （跳过 LLM 改写）
    │     │
    │     └ 未命中 ↓
    │
    ├── ③ RewriteRuleService::Recall          双路召回+RRF (631)
    │     │   BM25 + Vector → RRF 融合
    │     │   输出：按 RRF 分数倒序的示例列表
    │
    ├── ④ RewriteRuleService::SelectPrompt   按分数选 prompt (941)
    │     │   default / first_level / second_level
    │
    ├── ⑤ RewriteRuleService::AssemblePrompt  渲染 few-shot (1022)
    │     │   把示例插进 prompt 模板的 {{few_shots}} 占位符
    │
    ├── ⑥ DoRewriteInfer                       调 LLM 推理  (1290)
    │     │   把 ① 的上下文 + ⑤ 的 prompt 一起送给 LLM
    │
    └── ⑦ ExtractIntentFromRewrittenResult    解析 LLM JSON (1419)
          │
          ▼ rewrittenQuery
   返回 ExecuteFirstQuery
     ├─ 把 rewrittenQuery 写入 ContextCache    (供二次查询用)
     └─ 用 rewrittenQuery 异步召回长期记忆     (ProcessMemory)
```

---

## 4. 步骤详解

### 步骤①：`BuildRewriteContext` —— 1+N 算法压缩历史

**位置**：`context_service.cpp:1225-1287`

**目的**：给 LLM 看一段"压缩过的历史"作为对话参考，但又不能让 prompt 太长。

**算法常量**：
```cpp
constexpr size_t maxTotalTurns  = 5;  // 总共看 5 轮
constexpr size_t recentRawTurns = 1;  // 其中"最近 1 轮"用原始 QA
                                      // 剩下 4 轮用摘要 (abstract_qa)
```

**示意（假设 conversations 有 8 轮）**：
```
       旧 ──────────────────────────► 新
索引:  0    1    2    3    4    5    6    7
                  └────摘要 4 轮────┘   └原始 1 轮┘
                  取 abstract_qa 字段    取 history 字段
```

**输出**：`std::vector<std::map<std::string, std::string>>`，每条是 `{"role": "user/assistant", "content": "..."}`，按时间正序（旧 → 新）。

**为什么这样设计**：
- 最近一轮是当前问题的"直接上下文"，不能丢信息 → 用原始 QA；
- 更早的对话只需要"主题/对象"层面的信息 → 用摘要节省 token；
- 形成"1 原始 + N 摘要"的模式，控制总 token 在可接受范围。

**关键调用**：
```cpp
ExtractMessagesFromHistory(originQA, recentMemories);  // 解析 history JSON 数组
ExtractMessagesFromHistory(abstractDoc, abstractMemories);
memories = abstractMemories ++ recentMemories;        // 摘要在前，原始在后
```

---

### 步骤②：`ScalarSearch` —— 精确匹配规则库

**位置**：`rewrite_rule_service.cpp:613-628`

**目的**：如果运维之前在改写规则表里登记过完全相同的 query，**直接命中规则，跳过 LLM**（最快路径）。

**实现**：
```cpp
RuleExampleTbl::QueryRuleByHuman(query, results, 100);
```

按当前用户原句去 `tbl_rewrite_rule_example` 表查 `human` 字段，最多取 100 条。

**走向**：

| 结果 | 后续行为 |
|---|---|
| 命中 (`results` 非空) | `RewriteQueryFromUserConversation` 直接 `return query`（**注意**：是返回原 query，不是改写后的——这里 ScalarSearch 的命中只用作"不必再 LLM 改写"的信号） |
| 未命中 | 进入步骤③ Recall |

**调用点**：`context_service.cpp:1371-1376`
```cpp
if (RewriteRuleService::GetInstance()->ScalarSearch(query, scalarResults)) {
    LOG_INFO("Recall: scalar search hit, ...");
    return query;
}
```

---

### 步骤③：`Recall` —— 双路召回 + RRF 融合

**位置**：`rewrite_rule_service.cpp:631-729`

**目的**：从规则库里找"语义/字面相似"的历史改写示例，作为 LLM 的 few-shot 参考。

#### 3.1 双路并行召回

| 路径 | 函数 | 配置项 | 默认 |
|---|---|---|---|
| BM25（字面相似） | `RuleExampleTbl::QueryRuleExampleByExampleText` | `bm25_top_k` | 5 |
| Vector（语义相似） | `RuleExampleTbl::QueryRuleExampleByExampleVector` | `embedding_top_k` | 3 |

返回结果都附带"在该路召回中的排名 rank（1-based）"。

#### 3.2 RRF (Reciprocal Rank Fusion) 融合

**单路 RRF 分**（与原始相似度无关，只看排名）：
```
rrf = 1 / (smoothing_const + rank)
    = 1 / (60 + rank)
```

**最终融合分**：
```
final_score = rrf_bm25_ratio × bm25_rrf  +  rrf_embedding_ratio × vec_rrf
            = 0.8 × bm25_rrf  +  0.2 × vec_rrf
```

#### 3.3 计算示例

某条规则 X 在 BM25 排第 1、Vector 排第 2：
```
bm25_rrf = 1/(60+1) = 0.01639
vec_rrf  = 1/(60+2) = 0.01613
final    = 0.8 × 0.01639 + 0.2 × 0.01613 = 0.01634
```

#### 3.4 输出

按 `final_score` 倒序排，每条规则的分数被存进 `RuleExample::SetRRFScore(score)`，整体作为 `vector<shared_ptr<RuleExample>>` 返回。

#### 3.5 为什么用 RRF

- 单路（BM25 / Vector）的"原始分数量纲不同"，不能直接相加；
- RRF 不依赖原始分，只看"排第几"，对两路输出做稳定融合；
- `smoothing_const=60` 是经验值，让排名靠后的结果分数差异变小，避免 top-1 一家独大。

#### 3.6 涉及配置

| 配置项 | 默认 | 含义 |
|---|---|---|
| `bm25_top_k` | 5 | BM25 召回个数 |
| `embedding_top_k` | 3 | 向量召回个数 |
| `rrf_bm25_ratio` | 0.8 | BM25 在融合中的权重 |
| `rrf_embedding_ratio` | 0.2 | 向量权重 |
| `smoothing_const` | 60 | RRF 平滑常数 |

---

### 步骤④：`SelectPrompt` —— 按分数选 prompt 等级

**位置**：`rewrite_rule_service.cpp:941-1018`

**目的**：召回到的相似示例**质量好** → 走"少例 + 强约束"的高级 prompt；**质量差** → 退化到通用 prompt。

#### 4.1 配置项

| 配置项 | 默认 | 含义 |
|---|---|---|
| `prompt_select_enable` | "1" | 总开关，"0" 时永远用 default |
| `first_level_score` | 0.9 | 一级阈值 |
| `second_level_score` | 0.7 | 二级阈值 |

#### 4.2 判断逻辑

```
if (prompt_select_enable == "0" || results 为空)
    → "default"，不带 few-shot

else if (任意一条 score >= 0.9)
    → "first_level"
       filteredExamples = 所有 score ≥ 0.9 的示例

else if (任意一条 0.7 ≤ score < 0.9)   ⚠️ 见 4.3
    → "second_level"
       filteredExamples = 所有 0.7 ≤ score < 0.9 的示例

else
    → "default"
```

#### 4.3 ⚠️ 已知坑

`rewrite_rule_service.cpp:978-980` 那段统计 `secondLevelCount` 的代码**被注释掉了**：

```cpp
if (score >= config.first_level_score) {
    firstLevelCount++;
}
//        else if (score >= config.second_level_score) {  // ← 被注释
//            secondLevelCount++;
//        }
```

所以 `secondLevelCount` 永远是 0，**second_level 分支实际走不到**——当前实际只有 `default` 和 `first_level` 两档。

如果未来要恢复二级 prompt，需要：
1. 解开 978-980 行的注释；
2. 在 `prompt.json` 中提供 `second_level_rewrite_query_system` / `second_level_rewrite_query_user` 这两个 prompt（目前 prompt.json 里没有）。

#### 4.4 输出

```cpp
struct SelectPromptResult {
    std::string promptLevel;                                  // "default" / "first_level" / "second_level"
    std::vector<std::shared_ptr<RuleExample>> filteredExamples;  // 用于 few-shot 的示例
};
```

---

### 步骤⑤：`AssemblePrompt` —— 渲染 few-shot prompt

**位置**：`rewrite_rule_service.cpp:1022-1068`

**目的**：把 ④ 选定的 prompt 模板 + 召回的示例渲染成最终送给 LLM 的字符串。

#### 5.1 prompt 等级 → prompt 名映射

写死在 `rewrite_rule_service.cpp:50-54`：

```cpp
PROMPT_LEVEL_MAP = {
    "default"      → ("rewrite_query_system",              "")
    "first_level"  → ("first_level_rewrite_query_system",  "first_level_rewrite_query_user")
    "second_level" → ("second_level_rewrite_query_system", "second_level_rewrite_query_user")
}
```

每个元组是 `(system_prompt_key, user_prompt_key)`。

#### 5.2 prompt 内容来源

通过 `ModelMgr::GetInstance()->GetPrompt(promptKey)` 从 `prompt.json` 取，可以通过 REST 接口在线替换：

```
GET  /api/v1/contexts/prompts/item?name=rewrite_query_system
PUT  /api/v1/contexts/prompts/item?name=rewrite_query_system
     body: {"value": "<新 prompt 内容>"}
PUT  /api/v1/contexts/prompts          # 批量替换整份 prompt.json
```

#### 5.3 few-shot 渲染

把 ④ 过滤后的示例拼成：

```
### 示例1
上下文：xxx
用户输入：原 query
用户完整意图：{"用户完整意图":"期望改写结果"}

### 示例2
上下文：...
用户输入：...
用户完整意图：{"用户完整意图":"..."}
```

把这个字符串塞进 user prompt 模板的 `{{few_shots}}` 占位符，把当前 query 塞进 `{{query}}` 占位符。

模板渲染：
```cpp
userPrompt.RenderPrompt({{"few_shots", fewShow}});
userPrompt.RenderPrompt({{"query", query}});
```

#### 5.4 输出

```cpp
return std::make_pair(
    userPrompt.GetContent(),  // .first  = 渲染后的 user prompt
    systemPrompt              // .second = system prompt
);
```

---

### 步骤⑥：`DoRewriteInfer` —— 调 LLM 推理

**位置**：`context_service.cpp:1290-1327`

**模型 / 参数**：

| 参数 | 值 | 含义 |
|---|---|---|
| 模型名 | `std::getenv("LLM_MODEL_32B")` | 来自环境变量，例：`Qwen3-32B-RewriteQA` |
| 模型服务名 | `SERVICE_NAME` | 改写专用服务路由 |
| temperature | `0.001` | 接近确定性，避免随机性 |
| schedoption | `{"X-Task-Type": "DMcontextserviceRewriteQuery"}` | 调度标识 |

**调用栈**：

```cpp
reactLLMomponent->CreateTask(reasonParam, callback, [&](errCode, taskId) {
    CustomedLLMComponent->UpdateUserContext(
        taskId,
        mixedContext,        // ← 步骤①的"1+N 历史"塞进去
        [=](errCode) {
            reactLLMomponent->RequestAsync(taskId, req, {});
            //                                        ↑
            //                              步骤⑤的 (user, system) prompt
        });
});

// 主线程同步等
auto result = resultFuture.get();
```

**注意**：这条路径**没有显式超时控制**。`model_infer.cpp:LLMModelInfer` 那个 60s 超时是另一条路径（用于摘要等场景），DoRewriteInfer 走 future + UpdateUserContext，理论上会一直阻塞直到 LLM 返回。

---

### 步骤⑦：`ExtractIntentFromRewrittenResult` —— 解析 LLM 返回

**位置**：`context_service.cpp:1419-1465`

LLM 返回的是 JSON 字符串（不同版本格式略有差异），按优先级逐个尝试：

| 优先级 | 格式 | 提取字段 | 来源 |
|---|---|---|---|
| 1 | `{"意图":"..."}` | `意图` | 最新版 prompt（rewrite_query_system） |
| 2 | `{"用户完整意图":"..."}` | `用户完整意图` | 较新版 prompt（first_level_rewrite_query_system/user） |
| 3 | `{"need_rewrite":1, "rewritten_query":"..."}` | `rewritten_query` | 旧格式 |
| 4 | `{"need_rewrite":0}` | （返回空字符串） | 旧格式表示"不需改写" |
| 5 | 解析失败 / 字段缺失 | （返回空字符串） | — |

**最终行为**：`RewriteQueryFromUserConversation` 末尾：
```cpp
return extractedQuery.empty() ? query : extractedQuery;
```
即解析失败/空时**回退到原 query**，保证流程不中断。

---

## 5. 改写完之后

回到 `ExecuteFirstQuery`（`context_service.cpp:2696` 之后）：

```cpp
// 1. 把 rewrittenQuery 写入 ContextCache，第二次查询要用
auto cache = std::make_shared<ContextCache>();
cache->responseId     = responseId;
cache->rewrittenQuery = rewrittenQuery;
cache->conversations  = conversations;
AddContextCache(responseId, cache);

// 2. 用 rewrittenQuery 异步召回长期记忆（如果开启）
if (params.enableMemory) {
    ProcessMemory(params, rewrittenQuery, responseId, ...);
    //   └─► RetrieveMemory:
    //         quality 模式 → GetLongMem(KMM, rewrittenQuery)
    //         fast 模式    → LightRetrievalMem(KMM, rewrittenQuery)
}
```

第二次查询带 `response_id` 来时，`ExecuteSecondQuery` 直接从 `m_contextCacheMap` 取出 `rewrittenQuery` 一并返回，**不再走改写流程**。

---

## 6. 涉及的全部可调参数（速查）

| 步骤 | 配置项 | 当前默认 | 当前是否可动态生效 |
|---|---|---|---|
| ① | `maxTotalTurns` (1+N 总轮) | 5 (constexpr) | ❌ 硬编码 |
| ① | `recentRawTurns` (1+N 中的 1) | 1 (constexpr) | ❌ 硬编码 |
| ③ | `bm25_top_k` | 5 | ⚠️ 已在 conf，但 `call_once` 阻断 |
| ③ | `embedding_top_k` | 3 | ⚠️ 同上 |
| ③ | `rrf_bm25_ratio` | 0.8 | ⚠️ 同上 |
| ③ | `rrf_embedding_ratio` | 0.2 | ⚠️ 同上 |
| ③ | `smoothing_const` | 60.0 | ⚠️ 同上（且 conf 文件未暴露） |
| ④ | `prompt_select_enable` | "1" | ⚠️ 同上 |
| ④ | `first_level_score` | 0.9 | ⚠️ 同上 |
| ④ | `second_level_score` | 0.7 | ⚠️ 同上（且 4.3 已说明 second_level 分支死代码） |
| ⑤ | prompt 内容（`prompt.json`） | 见文件 | ✅ 已有 PUT prompts 接口 |
| ⑥ | LLM 模型名 | env `LLM_MODEL_32B` | ✅ 改环境变量 + 重启 |

> "`call_once` 阻断"：`RewriteRuleService::GetRewriteRuleConfig()`（`rewrite_rule_service.cpp:866-870`）用 `std::call_once` 只在第一次调用时把 ConfigMgr 的值复制到 `rewrite_rule_config_instance` 缓存里，之后无论 ConfigMgr 怎么改，业务侧拿到的都是启动时快照。要做动态生效需要去掉 `call_once`，改成在 `PUT configs/item?type=rewrite_rule_config` 时主动 reload。

---

## 7. 一句话总结

> **当前 query** + **5 轮历史（1 原始 + 4 摘要）** → **从规则库召回相似示例（BM25 + 向量 + RRF）** → **按召回分数挑 prompt 等级** → **把示例渲染成 few-shot prompt** → **送 LLM 推理** → **解析 JSON 拿改写结果** → **拿改写结果去召回长期记忆**。

---

## 8. 相关文件索引

| 文件 | 关键内容 |
|---|---|
| `context_service.cpp:1225-1287` | `BuildRewriteContext`（1+N 算法） |
| `context_service.cpp:1290-1327` | `DoRewriteInfer`（LLM 调用） |
| `context_service.cpp:1349-1406` | `RewriteQueryFromUserConversation`（改写主入口） |
| `context_service.cpp:1419-1465` | `ExtractIntentFromRewrittenResult`（解析） |
| `context_service.cpp:2653-2773` | `ExecuteFirstQuery`（改写的外层调用） |
| `rewrite_rule_service.cpp:50-54` | `PROMPT_LEVEL_MAP`（等级 → prompt 名映射） |
| `rewrite_rule_service.cpp:613-628` | `ScalarSearch`（精确匹配） |
| `rewrite_rule_service.cpp:631-729` | `Recall`（BM25+向量+RRF） |
| `rewrite_rule_service.cpp:866-938` | `GetRewriteRuleConfig` / `InitializeRewriteRuleConfig`（含 `call_once`） |
| `rewrite_rule_service.cpp:941-1018` | `SelectPrompt`（分数 → prompt 等级） |
| `rewrite_rule_service.cpp:1022-1068` | `AssemblePrompt`（渲染 few-shot） |
| `conf/prompt.json` | 所有 prompt 模板内容 |
| `conf/dmcontextservice_config.json` | `rewrite_rule_config` 配置节 |
