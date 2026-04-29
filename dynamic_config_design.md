<!--
  Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.
-->

# DMContextService REST 动态配置需求与代码定位

## 1. 需求背景

当前 `DMContextService` 中有一批上下文查询、改写、召回、缓存和超时参数散落在请求默认值、硬编码常量、静态初始化配置中。运维或算法调优时，如果需要修改这些参数，部分只能改配置文件后重启服务，部分虽然已有 REST 配置接口但业务代码没有实时读取，不能满足在线调参诉求。

本需求目标是开放 REST 接口，调用接口修改参数后：

1. 参数立即更新到服务内存态，后续请求无需重启即可生效。
2. 参数同步持久化到 `dmcontextservice_config.json`，服务重启后仍保留。
3. 对线程池大小等不能安全动态调整的参数，支持写入配置文件，但明确标记为“重启生效”。

## 2. 已确认需求边界

| 问题 | 确认结论 |
|---|---|
| “第一次查询 5 轮、第二次查询 20 轮”含义 | 指 DB 查询 `topK`，不是最终组装返回的 `history` 轮数 |
| “选择算法：标量、向量、BM25、混合”范围 | 只针对改写规则召回，不影响长期记忆召回 |
| “用户缓存的用户历史对话、记忆占用总空间” | 需要做可配置上限，不只是查询监控 |

## 3. 现有能力

### 3.1 通用配置 REST 接口

配置接口已在 `route_mgr.cpp` 注册：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/contexts/configs` | 查询支持的配置类型 |
| GET | `/api/v1/contexts/configs/items?type=xxx` | 查询某类配置 |
| GET | `/api/v1/contexts/configs/item?type=xxx&name=yyy` | 查询单项配置 |
| PUT | `/api/v1/contexts/configs/item?type=xxx&name=yyy` | 更新单项配置 |

相关代码：

| 文件 | 位置 | 说明 |
|---|---|---|
| `route_mgr.cpp` | 81-85 | 注册 configs REST 接口 |
| `config_service.cpp` | 159-232 | 解析更新请求，调用配置管理器更新并保存 |
| `config_mgr.cpp` | 329-351 | 更新内存配置 |
| `config_mgr.cpp` | 403-512 | 将配置写回文件，采用 tmp 文件再 rename 的方式 |

### 3.2 Prompt 在线替换接口

Prompt 接口已在 `route_mgr.cpp` 注册：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/contexts/prompts/item?name=xxx` | 查询 prompt |
| PUT | `/api/v1/contexts/prompts/item?name=xxx` | 更新单个 prompt |
| PUT | `/api/v1/contexts/prompts` | 批量更新 prompt |

相关代码：

| 文件 | 位置 | 说明 |
|---|---|---|
| `route_mgr.cpp` | 87-90 | 注册 prompts REST 接口 |
| `config_service.cpp` | 265-328 | 单个 prompt 更新 |
| `config_service.cpp` | 330-352 | 批量 prompt 更新 |
| `model_infer.cpp` | 80 | 每次调用模型前从 `ModelMgr` 获取 prompt |
| `rewrite_rule_service.cpp` | 1037、1061 | 改写规则 prompt 从 `ModelMgr` 获取 |

结论：在线替换上下文 prompt 的接口已经存在，后续需要确认 `ModelMgr::UpdatePrompt` / `SavePromptsToFile` 在外部平台树中的实现是否线程安全。

## 4. 参数清单与代码定位

### 4.1 查询与历史组装参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| 第一次 DB 查询 topK | `query_config.max_db_query_turns=20` | `context_service.cpp:2673` | 不能单独配置第一次查询，需求为 5 | `query_config.first_query_db_top_k` |
| 第二次 DB 查询 topK | `query_config.max_db_query_turns=20` | `context_service.cpp:2597` | 不能单独配置第二次查询，需求为 20 | `query_config.second_query_db_top_k` |
| 最终组装历史轮数 | 请求参数，否则第一次 3、第二次 4 | `context_service.cpp:1557-1567` | 默认值硬编码，和需求描述不一致 | `query_config.first_history_turns`、`query_config.second_history_turns` |
| 总 token 预算 | 请求参数，否则 8192 | `context_service.cpp:1576-1579` | 默认值硬编码 | `query_config.total_token_budget` |
| 历史组装策略：第一次 | `useMerge=false` | `context_service.cpp:2700-2705` | 策略硬编码 | `query_config.first_history_build_strategy` |
| 历史组装策略：第二次 | `useMerge=true` | `context_service.cpp:2619-2624` | 策略硬编码 | `query_config.second_history_build_strategy` |
| 1+N 改写上下文总轮数 | `maxTotalTurns=5` | `context_service.cpp:1232-1234` | 硬编码 | `rewrite_query_config.max_total_turns` |
| 1+N 原始对话轮数 | `recentRawTurns=1` | `context_service.cpp:1232-1234` | 硬编码 | `rewrite_query_config.recent_raw_turns` |
| 最近一轮原始对话 token 阈值 | `1000` | `context_builder.cpp:115` | 硬编码 | `query_config.last_raw_round_token_threshold` |
| 上下文窗口可见周期 | `com_params.aging_period=30` | `context_service.cpp:1675-1687` | 已读配置，可热生效 | 保留 `com_params.aging_period` |

说明：

- “第一次/第二次查询 5/20 轮”已确认为 DB 查询 topK，应改造 `ExecuteFirstQuery` 和 `ExecuteSecondQuery` 的 DB 查询 limit，而不是只改 `conversationTurns`。
- `conversationTurns` 仍可保留请求级覆盖能力，请求未传时才读取配置默认值。

### 4.2 改写规则召回参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| 召回算法选择 | 当前固定：标量精确命中，否则 BM25 + 向量混合 | `rewrite_rule_service.cpp:631-725` | 不能配置 scalar/vector/bm25/hybrid | `rewrite_rule_config.recall_algorithm` |
| 标量召回 | 固定先执行 | `context_service.cpp:1369-1376`、`rewrite_rule_service.cpp:613-626` | 不能关闭或按算法选择跳过 | 受 `recall_algorithm` 控制 |
| BM25 topK | `bm25_top_k=5` | `rewrite_rule_service.cpp:648` | 配置已存在但不是热生效 | 保留 `rewrite_rule_config.bm25_top_k` |
| 向量 topK | `embedding_top_k=3` | `rewrite_rule_service.cpp:659` | 配置已存在但不是热生效 | 保留 `rewrite_rule_config.embedding_top_k` |
| BM25 权重 | `rrf_bm25_ratio=0.8` | `rewrite_rule_service.cpp:700` | 配置已存在但不是热生效 | 保留 `rewrite_rule_config.rrf_bm25_ratio` |
| 向量权重 | `rrf_embedding_ratio=0.2` | `rewrite_rule_service.cpp:700` | 配置已存在但不是热生效 | 保留 `rewrite_rule_config.rrf_embedding_ratio` |
| 一级分值 | `first_level_score=0.9` | `rewrite_rule_service.cpp:975`、`991` | 配置已存在但不是热生效 | 保留 `rewrite_rule_config.first_level_score` |
| 二级分值 | `second_level_score=0.7` | `rewrite_rule_service.cpp:1006` | 配置已存在但不是热生效；且二级计数逻辑被注释 | 保留 `rewrite_rule_config.second_level_score` |
| RRF 平滑常数 | 代码默认 60，配置文件当前没有 | `rewrite_rule_service.cpp:917-920` | 文件缺字段时无法持久化新增项 | `rewrite_rule_config.smoothing_const` |

关键问题：

- `RewriteRuleService::GetRewriteRuleConfig()` 使用 `std::call_once` 初始化，见 `rewrite_rule_service.cpp:866-869`。配置接口更新 `rewrite_rule_config` 后，业务仍使用旧的 `rewrite_rule_config_instance`。
- 需要改成每次读取 `ConfigMgr`，或提供 `ReloadRewriteRuleConfig()`，在 `ConfigService::UpdateConfig` 成功后触发刷新。

建议 `recall_algorithm` 取值：

| 值 | 行为 |
|---|---|
| `scalar` | 仅精确标量检索 |
| `bm25` | 标量可选；近似召回仅 BM25 |
| `vector` | 标量可选；近似召回仅向量 |
| `hybrid` | 标量精确命中优先；未命中时 BM25 + 向量 RRF 融合 |

是否保留“标量精确命中优先”建议独立配置：

```json
{
  "rewrite_rule_config": {
    "recall_algorithm": "hybrid",
    "scalar_first_enable": "1"
  }
}
```

### 4.3 记忆召回参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| 记忆召回模式 | `query_config.mode=quality` | `context_service.cpp:1536-1537`、`2473-2483` | 已可通过配置默认控制 | 保留 `query_config.mode` |
| 是否召回记忆 | 请求参数，否则 true | `context_service.cpp:1550-1555` | 默认值硬编码 | `query_config.enable_memory` |
| 记忆条数 | 请求参数，否则 20 | `context_service.cpp:1571-1572` | 默认值硬编码 | `query_config.memory_count` |
| 记忆 token/字符预算 | 请求参数，否则 8192 | `context_service.cpp:1573-1574` | 默认值硬编码 | `query_config.memory_token_budget` |
| 轻量检索 topK | `topK=20` | `context_service.cpp:236-237` | 硬编码，且和最终 `memory_count` 不是同一个控制点 | `query_config.light_retrieval_top_k` |
| 查询记忆等待超时 | `1500ms` | `context_service.h:249-264` | 硬编码 | `timeout_config.memory_wait_timeout_ms` |

说明：

- 长期记忆召回算法选择不在本次“标量/向量/BM25/混合”的范围内；算法选择只针对改写规则召回。
- `memory_count` 和 `memory_token_budget` 当前在 `BuildMemoryStr` 中二选一，且 `memory_count > 0` 优先。这个优先级需要保留或在接口文档中明确。

### 4.4 改写开关与 prompt

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| 是否改写 | 请求参数，否则 false | `context_service.cpp:1541-1546` | 默认值硬编码 | `query_config.rewrite_query_enable` |
| 改写 prompt | `ModelMgr::GetPrompt` | `context_service.cpp:1389-1398`、`rewrite_rule_service.cpp:1037` | 已有 prompt 更新接口 | 保留 prompts 接口 |
| 忘记记忆命令 | `forget_memory_commands` | `context_service.cpp:1585-1593` | 已可配置，数组接口支持 | 保留 |

### 4.5 缓存与空间上限参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| responseId 上下文缓存过期时间 | `300s` | `context_service.h:468`、`context_service.cpp:1126` | 默认值硬编码，定时任务未传配置 | `cache_config.context_cache_expire_seconds` |
| responseId 缓存清理周期 | `5min` | `context_service.cpp:1153-1158` | 硬编码 | `cache_config.context_cache_clean_interval_ms` |
| responseId 缓存总空间上限 | 无 | `context_service.h:449-451` | 当前 `m_contextCacheMap` 无容量限制 | `cache_config.context_cache_max_items`、`context_cache_max_bytes` |
| 记忆缓存单 cache limit | `UserMemCache::_limit=5` | `context_service.h:176` | 硬编码，只限制单个 `UserMemCache` | `cache_config.memory_cache_per_key_limit` |
| 记忆缓存总空间上限 | 无 | `context_service.h:443-447` | `caches` map 无全局容量限制 | `cache_config.memory_cache_max_items`、`memory_cache_max_bytes` |
| 写缓存 flush 周期 | `100ms` | `context_service.cpp:1012-1014` | 硬编码，注释写 500ms | `cache_config.write_cache_flush_interval_ms` |

建议空间上限策略：

1. `max_items` 控制对象数量，易实现且开销小。
2. `max_bytes` 控制估算内存，按字符串长度估算，不做精确 allocator 级统计。
3. 超过上限时按 LRU 或最早 timestamp 淘汰。
4. 对 `m_contextCacheMap` 和 `caches` 分别控制，避免互相影响。

### 4.6 超时参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 问题 | 建议配置项 |
|---|---:|---|---|---|
| 模型调用超时 | `60s` | `model_infer.cpp:54` | 硬编码 | `timeout_config.model_infer_timeout_ms` |
| 查询记忆等待超时 | `1500ms` | `context_service.h:249` | 硬编码 | `timeout_config.memory_wait_timeout_ms` |
| 缓存查找超时 | 同 `FindMem` 等待超时 | `context_service.h:249` | 需求中可作为独立项；当前未拆分 | `timeout_config.cache_lookup_timeout_ms` |
| KMM/DB HTTP 请求超时 | 未在本目录看到显式设置 | `context_db_client.cpp:75-159`、`context_service.cpp:172-292` | 依赖框架默认超时 | 如框架支持，新增 `timeout_config.kmm_request_timeout_ms` |

### 4.7 线程参数

| 参数 | 当前默认值/来源 | 当前代码位置 | 动态生效建议 |
|---|---:|---|---|
| 查询线程池 | 32 | `route_mgr.cpp:27`、`97` | 非动态生效，重启生效 |
| 普通路由线程池 | 16 | `route_mgr.cpp:28`、`98` | 非动态生效，重启生效 |
| 上下文服务内部线程池 | 32 | `context_service.cpp:87-88` | 非动态生效，重启生效 |
| 历史对话管理线程 | 当前未看到独立线程池，主要是 `m_ThreadPool` 和异步任务 | `context_service.cpp:937`、`1960`、`2416` | 需确认外部平台是否另有线程池 |

线程池已创建后，本目录代码未暴露 resize 能力。建议接口允许写配置文件，但响应中标记：

```json
{
  "code": 200,
  "message": "updated successfully, restart required",
  "effective": "restart"
}
```

## 5. 建议配置结构

建议在 `dmcontextservice_config.json` 增补如下配置段。所有值继续沿用当前代码风格，使用字符串保存，降低对现有 `GetStringMap` 的影响。

```json
{
  "query_config": {
    "mode": "quality",
    "first_query_db_top_k": "5",
    "second_query_db_top_k": "20",
    "first_history_turns": "5",
    "second_history_turns": "20",
    "total_token_budget": "8192",
    "memory_count": "20",
    "memory_token_budget": "8192",
    "enable_memory": "true",
    "rewrite_query_enable": "false",
    "light_retrieval_top_k": "20",
    "first_history_build_strategy": "normal",
    "second_history_build_strategy": "merge",
    "last_raw_round_token_threshold": "1000"
  },
  "rewrite_query_config": {
    "max_total_turns": "5",
    "recent_raw_turns": "1"
  },
  "rewrite_rule_config": {
    "prompt_select_enable": "1",
    "recall_algorithm": "hybrid",
    "scalar_first_enable": "1",
    "rrf_bm25_ratio": "0.8",
    "rrf_embedding_ratio": "0.2",
    "bm25_top_k": "5",
    "embedding_top_k": "3",
    "first_level_score": "0.9",
    "second_level_score": "0.7",
    "smoothing_const": "60"
  },
  "cache_config": {
    "context_cache_expire_seconds": "300",
    "context_cache_clean_interval_ms": "300000",
    "context_cache_max_items": "10000",
    "context_cache_max_bytes": "104857600",
    "memory_cache_per_key_limit": "5",
    "memory_cache_max_items": "10000",
    "memory_cache_max_bytes": "104857600",
    "write_cache_flush_interval_ms": "100"
  },
  "timeout_config": {
    "model_infer_timeout_ms": "60000",
    "memory_wait_timeout_ms": "1500",
    "cache_lookup_timeout_ms": "1500",
    "kmm_request_timeout_ms": "5000"
  },
  "thread_config": {
    "route_query_threads": "32",
    "route_normal_threads": "16",
    "context_service_threads": "32"
  }
}
```

## 6. 接口设计建议

### 6.1 复用现有通用配置接口

继续使用：

```http
PUT /api/v1/contexts/configs/item?type=query_config&name=first_query_db_top_k
Content-Type: application/json

{"value":"5"}
```

成功响应建议增加生效范围：

```json
{
  "code": 200,
  "message": "updated successfully",
  "effective": "runtime"
}
```

线程类参数：

```http
PUT /api/v1/contexts/configs/item?type=thread_config&name=route_query_threads
Content-Type: application/json

{"value":"64"}
```

```json
{
  "code": 200,
  "message": "updated successfully, restart required",
  "effective": "restart"
}
```

### 6.2 支持批量更新配置

当前 `UpdateConfig` 只支持单项对象或数组。建议新增批量对象能力：

```http
PUT /api/v1/contexts/configs/items?type=query_config
Content-Type: application/json

{
  "first_query_db_top_k": "5",
  "second_query_db_top_k": "20",
  "memory_count": "20",
  "enable_memory": "true"
}
```

这样前端/运维界面可以一次提交某个配置分组，避免多次请求中间状态不一致。

## 7. 改造点清单

### 7.1 ConfigMgr

涉及文件：

- `include/config_mgr.h`
- `config_mgr.cpp`

改造建议：

1. 新增 `cache_config`、`timeout_config`、`thread_config`、`rewrite_query_config` 配置段。
2. `InitConfigMgr` 加载新配置段。
3. `GetAllConfigTypes` 返回新配置段。
4. `GetConfigByType` 支持读取新配置段。
5. `UpdateConfig` 支持写新配置段。
6. `SaveConfigToFile` 支持持久化新配置段。
7. `SaveConfigToFile` 当前只更新已有 key，建议支持不存在的 key 时新增字段。
8. 修复 `GetConfig` 中重复加锁导致的潜在死锁：不要在持锁状态下调用同样加锁的 `GetConfigByType`。

### 7.2 ContextService

涉及文件：

- `include/context_service.h`
- `context_service.cpp`
- `context_builder.cpp`
- `include/context_builder.h`

改造建议：

1. `ParseQueryParams` 中默认值从 `query_config` 读取：
   - `rewrite_query_enable`
   - `enable_memory`
   - `first_history_turns`
   - `second_history_turns`
   - `memory_count`
   - `memory_token_budget`
   - `total_token_budget`
2. `ExecuteFirstQuery` 使用 `first_query_db_top_k`。
3. `ExecuteSecondQuery` 使用 `second_query_db_top_k`。
4. `BuildRewriteContext` 中 `maxTotalTurns`、`recentRawTurns` 从 `rewrite_query_config` 读取。
5. `LightRetrievalMem` 的 `topK` 从 `query_config.light_retrieval_top_k` 读取。
6. `FindMem` 等待超时从 `timeout_config.memory_wait_timeout_ms` 或 `cache_lookup_timeout_ms` 读取。
7. `CleanExpiredCache` 默认过期时间从 `cache_config.context_cache_expire_seconds` 读取。
8. 为 `m_contextCacheMap` 和 `caches` 增加全局容量控制。
9. `BuildContextMsgAdaptive` 的 `LAST_ROUND_TOKEN_THRESHOLD` 改为参数或配置读取。

### 7.3 RewriteRuleService

涉及文件：

- `include/rewrite_rules/rewrite_rule_service.h`
- `rewrite_rules/rewrite_rule_service.cpp`

改造建议：

1. `RewriteRuleConfig` 增加：
   - `recall_algorithm`
   - `scalar_first_enable`
2. 去除或绕开 `std::call_once` 导致的配置固化问题。
3. `Recall` 按 `recall_algorithm` 控制 BM25、向量、混合召回。
4. `ScalarSearch` 是否优先执行由 `scalar_first_enable` 控制。
5. 修复 `SelectPrompt` 中二级分值计数逻辑被注释的问题，否则 `second_level_score` 实际不会触发二级 prompt。

### 7.4 ModelInfer

涉及文件：

- `model_infer.cpp`

改造建议：

1. `LLMModelInfer` 中 `cond.wait_for(... seconds(60))` 改为读取 `timeout_config.model_infer_timeout_ms`。
2. 超时后需要明确返回失败日志，避免只依赖 `ret` 初始值判断。

### 7.5 RouteMgr

涉及文件：

- `route_mgr.cpp`

改造建议：

1. 线程池大小可以从 `thread_config` 初始化读取。
2. 运行时更新 `thread_config` 后标记为重启生效。
3. 如外部 `ThreadPool` 支持 resize，再单独评估动态生效；当前本目录看不到相关 API。

## 8. 生效策略

| 配置类型 | 生效方式 | 原因 |
---|---|---|
| 查询默认值、topK、token、开关 | 运行时生效 | 每次请求解析参数时读取配置即可 |
| 改写规则召回配置 | 运行时生效 | 需要移除静态固化或支持 reload |
| prompt | 运行时生效 | 已有接口，调用模型前重新获取 prompt |
| 缓存过期时间、空间上限 | 运行时生效 | 清理和插入时读取配置即可 |
| 模型/记忆等待超时 | 运行时生效 | 每次等待前读取配置即可 |
| 线程池大小 | 重启生效 | 线程池已构造，当前未看到 resize 能力 |

## 9. 风险与注意事项

1. 配置值当前大多以字符串保存，业务读取时需要做数值/布尔校验，非法值不能直接 `stoi`，否则可能导致请求线程异常。
2. 配置更新接口当前对未知 key 不够友好，文件中没有字段时不会新增，需要改造持久化逻辑。
3. 改写规则配置热生效要注意并发读写，建议用 `std::shared_mutex` 或原子替换 `shared_ptr<RewriteRuleConfig>`。
4. 缓存空间上限如果按 bytes 估算，只能作为近似限制，需要在文档中说明不是精确堆内存统计。
5. 批量配置更新要保证原子性：内存更新成功但文件保存失败时，需要明确回滚策略或返回部分失败。
6. `ConfigMgr::GetConfig` 存在重复加锁风险，建议优先修复，否则单项查询接口可能卡死。

## 10. 建议验收标准

| 场景 | 验收点 |
|---|---|
| 修改 `first_query_db_top_k=5` | 第一次查询调用 KMM 历史查询时 `top_k=5` |
| 修改 `second_query_db_top_k=20` | 第二次查询缓存 miss 后 DB 查询 `top_k=20` |
| 修改 `memory_count` | 第二次查询返回的 `memory` 最多包含配置条数 |
| 修改 `enable_memory=false` | 第一次查询不触发 `ProcessMemory` |
| 修改 `rewrite_query_enable=true` | 请求未传 `rewrite_query` 时默认执行改写 |
| 修改 `recall_algorithm=bm25` | 改写规则召回只走 BM25 近似检索 |
| 修改 `prompt_select_enable=0` | 改写规则 prompt 选择固定走 default |
| 修改 prompt | 后续模型调用使用新 prompt，无需重启 |
| 修改缓存过期时间 | responseId 缓存按新时间清理 |
| 修改线程池大小 | 配置文件更新成功，响应提示重启生效 |

