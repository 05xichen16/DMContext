# DMContextService 动态配置参数清单（按业务接口流程整理）

> **范围**：仅列出**当前不能在运行时调整**（硬编码 / 单次加载 / 粒度不够）的参数。
>
> **不包含**：
> - 接口入参本身（请求体里的 `rewrite_query` / `enable_memory` / `memory_count` / `token_budget` / `conversation_turns` / `mode` 等）——调用方传什么就生效什么；
> - prompt 内容——已有 `PUT /api/v1/contexts/prompts/item` 在线替换接口。
>
> **改造类型标识**：
> - 🅐 = 请求体默认值硬编码（请求没传时回落到字面量，需移入 `default_query_params`）
> - 🅑 = 完全硬编码常量（写在 `.cpp/.h` 里，ConfigMgr 不知道）
> - 🅒 = 已在配置文件但运行时改了不生效（`call_once` 缓存 / 粒度不够）

---

## 一、`POST /api/v1/contexts/query` —— 查询接口

### 流程总览

```
HandleContextQueryX                       (route_mgr.cpp:188)
  └─► [线程池] m_queryThreadPool 提交     (route_mgr.cpp:97  | 32 线程)
       └─► ContextService::GetContextNew  (context_service.cpp:2775)
            └─► ParseQueryParams          (1492) ← 请求体默认值在这里
                ├─ 有 response_id → ExecuteSecondQuery
                └─ 无 response_id → ExecuteFirstQuery
```

### 1.1 入口阶段：`ParseQueryParams` 默认值

| 类 | 参数 | 作用 | 当前默认 | 代码位置 |
|---|---|---|---|---|
| 🅐 | `rewrite_query` 默认值 | 请求体不传时是否改写 | `false` | `context_service.cpp:1545` |
| 🅐 | `enable_memory` 默认值 | 请求体不传时是否召回记忆 | `true` | `context_service.cpp:1554` |
| 🅐 | `conversation_turns` 默认值（第一次） | N 轮压缩对话 | `3` | `context_service.cpp:1566` |
| 🅐 | `conversation_turns` 默认值（第二次） | 带 response_id 时的 N 轮 | `4` | `context_service.cpp:1564` |
| 🅐 | `memory_count` 默认值 | 长期记忆条数上限 | `20` | `context_service.cpp:1572` |
| 🅐 | `memory_token_budget` 默认值 | 长期记忆字符数上限 | `8192` | `context_service.cpp:1574` |
| 🅐 | `token_budget` 默认值 | 上下文组装总 token 预算 | `8192` | `context_service.cpp:1579` |

### 1.2 第一次查询流程：`ExecuteFirstQuery`

```
ExecuteFirstQuery                                            (2653)
  ├─► QueryHistoryData(qaFilter, limit=max_db_query_turns)   (2673)
  │     └─ aging_period 过滤过期对话                          (1675)
  ├─► [if isRewritequery] RewriteQueryFromUserConversation   (1349, 2690)
  │     ├─ BuildRewriteContext (1+N 算法)                    (1225)
  │     ├─ RewriteRuleService::Recall                         ┐
  │     │    ├─ BM25 召回                                     │ rewrite_rule_config
  │     │    ├─ Embedding 召回                                │ 各项参数
  │     │    └─ RRF 融合                                      │
  │     ├─ RewriteRuleService::SelectPrompt                  ─┘
  │     └─ DoRewriteInfer → LLMModelInfer  (60s 超时)         (model_infer.cpp:54)
  ├─► BuildHistoryResult                                      (3037)
  ├─► WriteResponse (返回第一次结果)
  ├─► AddContextCache (写入 m_contextCacheMap)               (1103)
  └─► ProcessMemory (异步, 提交到 m_ThreadPool=32)            (2406)
        └─ RetrieveMemory                                     (2473)
             ├─ quality → GetLongMem    (KMM normal)          (172)
             └─ fast    → LightRetrievalMem (topK=20 硬编码)  (230)
```

#### 涉及的需配置化参数

**a) DB 拉取（第一次轮数）**

| 类 | 参数 | 作用 | 当前默认 | 代码位置 |
|---|---|---|---|---|
| 🅒 | `max_db_query_turns_first` | 第一次 DB 拉取历史轮数 | `20`（**与第二次共用一个字段**） | `context_service.cpp:2673`、`config_mgr.cpp:106-108` |

**b) 1+N 改写算法（`BuildRewriteContext`）**

| 类 | 参数 | 作用 | 当前默认 | 代码位置 |
|---|---|---|---|---|
| 🅑 | `maxTotalTurns` | 1+N 总轮数（拼给改写模型） | `5` (constexpr) | `context_service.cpp:1233` |
| 🅑 | `recentRawTurns` | 1+N 中"原始对话"轮数 | `1` (constexpr) | `context_service.cpp:1234` |

**c) 改写规则召回 + Prompt 选择（`RewriteRuleService`）—— 全部 🅒：已在 conf 但 `call_once` 阻断**

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅒 | `prompt_select_enable` | prompt 选择开关 | `"1"` | `rewrite_rule_service.cpp:881-884` |
| 🅒 | `bm25_top_k` | BM25 召回个数 | `5` | `rewrite_rule_service.cpp:897-900`、`648` |
| 🅒 | `embedding_top_k` | 向量召回个数 | `3` | `rewrite_rule_service.cpp:902-905`、`659` |
| 🅒 | `rrf_bm25_ratio` | 混合算法 BM25 权重 | `0.8` | `rewrite_rule_service.cpp:887-890`、`700` |
| 🅒 | `rrf_embedding_ratio` | 混合算法向量权重 | `0.2` | `rewrite_rule_service.cpp:892-895`、`700` |
| 🅒 | `first_level_score` | 一级分数阈值 | `0.9` | `rewrite_rule_service.cpp:907-910`、`975` |
| 🅒 | `second_level_score` | 二级分数阈值 | `0.7` | `rewrite_rule_service.cpp:912-915`、`1006` |
| 🅒 | `smoothing_const` | RRF 平滑常数（**未在 conf 文件中暴露**） | `60.0` | `rewrite_rule_service.cpp:917-920` |

**d) 上下文组装（`BuildContextMsgAdaptive` / `BuildHistory`）**

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅑 | `LAST_ROUND_TOKEN_THRESHOLD` | 最后一轮 >1000 token 切换为压缩版 | `1000` (constexpr) | `context_builder.cpp:115` |
| 🅑 | `BuildContextMsgAdaptive` 默认 maxTokens | 上下文组装总 token 上限 | `3000` (默认形参) | `context_builder.h:46` |

**e) 模型 / 召回超时**

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅑 | LLM 模型调用超时 | 改写 LLM 等回包 | `60s` (硬编码) | `model_infer.cpp:54` |
| 🅑 | KMM 查询记忆超时 | `GetLongMem` / `LightRetrievalMem` HTTP 超时 | 走框架默认（无独立配置） | `context_service.cpp:172-227`、`230-292` |
| 🅑 | `LightRetrievalMem` topK | fast 模式 KMM 召回 topK | `20` (硬编码) | `context_service.cpp:237` |

**f) 第一次缓存写入（影响第二次查询）**

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅑 | `m_contextCacheMap` 容量上限 | 用户上下文缓存条数上限 | **当前无上限** | `context_service.cpp:1103-1110`（需新增） |

### 1.3 第二次查询流程：`ExecuteSecondQuery`

```
ExecuteSecondQuery                                          (2572)
  ├─► GetContextCache(responseId)                           (1113)
  │     └─ cache miss → QueryHistoryData(limit=max_db_query_turns)
  ├─► FindMem(responseId, content)  (1500ms 超时)           (context_service.h:234)
  │     └─ 等 ProcessMemory 完成（cv.wait_for）
  ├─► BuildMemoryStr(memory_count, memory_token_budget)     (1690)
  ├─► BuildHistoryResult (useMerge=true)                    (3037)
  ├─► RemoveContextCache                                    (1142)
  └─► WriteResponse
```

#### 涉及的需配置化参数

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅒 | `max_db_query_turns_second` | 第二次 cache miss 时 DB 拉取轮数 | `20`（**与第一次共用**） | `context_service.cpp:2597`、`config_mgr.cpp:106-108` |
| 🅑 | 缓存查找超时 | `FindMem` 等 `ProcessMemory` 完成 | `1500ms` (硬编码) | `context_service.h:251` |
| 🅑 | `UserMemCache._limit` | 每个 responseId 的 LRU 上限 | `5` (硬编码) | `context_service.h:176` |

---

## 二、`POST /api/v1/contexts/write` —— 写入接口

### 流程总览

```
HandleContextWrite                                          (route_mgr.cpp:111)
  └─► [线程池] m_threadPool 提交                            (route_mgr.cpp:98 | 16 线程)
       └─► ContextService::AddContextNew
            ├─► ParseWriteParams                            (1768)
            │     └─ enable_memory 默认值
            ├─► ParseMessagesToConversations
            ├─► AddToWriteCache (进 m_writeCache)           (1041)
            └─► AsyncGenerateSummary (异步, m_ThreadPool=32)
                  └─ summaryAbstractQAContext / summaryAbstractContext
                       └─ LLMModelInfer (60s 超时)          (model_infer.cpp:54)

[后台定时器] FlushWriteCache (每 100ms)                     (1012-1013)
  └─► SaveToDatabase → ContextDbClient::WriteConversations  (1074)
```

### 涉及的需配置化参数

| 类 | 参数 | 作用 | 当前值 | 代码位置 |
|---|---|---|---|---|
| 🅐 | `enable_memory` 默认值（写入路径） | 该轮是否触发记忆抽取 | `true` | `context_service.cpp:1768-1810` |
| 🅑 | 写缓存 flush 定时器间隔 | 多久把 `m_writeCache` 批量入库 | `100ms` (硬编码) | `context_service.cpp:1013` |
| 🅑 | LLM 模型调用超时（异步摘要） | `summaryAbstractQAContext` 等 LLM | `60s` (硬编码) | `model_infer.cpp:54`（与查询接口共用） |

> **不需要改造**：`KMM 写入 URL`（`/kmm/v1/user/memory/history/batch`）由 `ContextDbClient` 走 KMM 服务发现，不涉及参数化。

---

## 三、`POST /api/v1/contexts/delete` —— 删除接口

### 流程总览

```
HandleContextDelete                                         (route_mgr.cpp:172)
  └─► [线程池] m_threadPool 提交                            (16 线程)
       └─► ContextService::DeleteContextNew
            └─► ContextDbClient::UpdateContextStatus(..., "inactive")  软删除
```

### 涉及的需配置化参数

> **本接口无需配置化参数**。删除是软删（`UpdateContextStatus → "inactive"`），无可调阈值/超时/默认值。

---

## 四、跨接口共享的基础设施（贯穿三个接口）

### 4.1 线程池（按确认"非动态生效"，PUT 时返回 `effective: on_restart`）

```
RouteMgr::InitRestRouter (启动时一次性创建)                 (route_mgr.cpp:97-98)
ContextService 静态成员 (启动时一次性创建)                  (context_service.cpp:87-88)
```

| 类 | 参数 | 作用 | 服务的接口 | 当前值 | 代码位置 |
|---|---|---|---|---|---|
| 🅑 | `ROUTE_MAX_QUERY_THEARD` | 接口入口线程池 | **query** | `32` | `route_mgr.cpp:27`、`97` |
| 🅑 | `ROUTE_MAX_NORMAL_THEARD` | 接口入口线程池 | **write / delete** | `16` | `route_mgr.cpp:28`、`98` |
| 🅑 | `ContextService::m_ThreadPool` | 内部异步任务池 | query 的 `ProcessMemory` + write 的 `AsyncGenerateSummary` | `32` (写死) | `context_service.cpp:87-88` |

### 4.2 缓存清理定时器（影响 query 接口的二次缓存命中）

```
StartCacheTimer (启动时注册 TimerTask)                      (1009)
  ├─ FlushWriteCache 定时器 (100ms 间隔)                    ← 影响 write
  └─ InitCleanExpiredContextCacheTimer (5min 间隔)          ← 影响 query
       └─ CleanExpiredCache(expireSeconds=300)              (1126)
```

| 类 | 参数 | 作用 | 服务的接口 | 当前值 | 代码位置 |
|---|---|---|---|---|---|
| 🅑 | `m_contextCacheMap` TTL | 用户缓存过期时间（300s） | query（影响二次命中） | `300s` (默认形参) | `context_service.h:468`、`context_service.cpp:1126` |
| 🅑 | 过期清理定时器间隔 | 多久跑一次过期清理 | query | `5min` (硬编码) | `context_service.cpp:1157` |

### 4.3 已弃用 / 待确认

| 类 | 参数 | 服务的接口 | 状态 | 代码位置 |
|---|---|---|---|---|
| 🅒 | `rerank_config.top_k` / `reject_score` / `paas_score` / `max_memories` | 推测 query 召回 | **代码树里搜不到读取方**，待确认是否被外部模块使用 | `dmcontextservice_config.json:42-48`、`config_mgr.cpp:142-144` |

---

## 五、按接口的参数总数 + 改造工作量

| 接口 / 模块 | 🅐 | 🅑 | 🅒 | 合计 | 关键改造 |
|---|---|---|---|---|---|
| `/api/v1/contexts/query` | 7 | 9 | 9 | 25 | 拆 `max_db_query_turns`、解 `RewriteRuleService::call_once`、`m_contextCacheMap` 加上限 |
| `/api/v1/contexts/write` | 1 | 2 | 0 | 3 | flush 定时器间隔参数化 |
| `/api/v1/contexts/delete` | 0 | 0 | 0 | 0 | 无 |
| 跨接口（线程池） | 0 | 3 | 0 | 3 | 仅暴露配置入口，重启生效 |
| 跨接口（定时器） | 0 | 2 | 0 | 2 | 改 TTL 即时生效，改间隔需重建 TimerTask |
| 跨接口（rerank_config） | 0 | 0 | 4 | 4 | **先确认有无读取方** |
| **合计** | **8** | **16** | **13** | **37** | |

---

## 六、实施前必须解决的 3 个风险点

1. **`RewriteRuleService::call_once`**（`rewrite_rule_service.cpp:866-870`）—— 不解决，🅒 类全部 9 项 `rewrite_rule_config.*` 字段改了 ConfigMgr 也不会生效。改造方案：删 `call_once`，改成"`PUT configs/item?type=rewrite_rule_config` 时主动调 `RewriteRuleService::ReloadFromConfigMgr()`"。
2. **`rerank_config` 是否还有人用** —— 必须先到外部 CoreMind 树搜调用方，否则白做。
3. **定时器间隔改了不会立即生效** —— `TimerTask` 启动时一次性注册。要么接受"下个周期生效"，要么实现重建 TimerTask（多 ~20 行）。
