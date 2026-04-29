# DMContext 长期记忆召回（Memory Retrieval）流程详解

> 范围：`/api/v1/contexts/query` 接口的"长期记忆召回"分支，从触发到结果落地的完整链路。
> 用途：归档学习材料，方便后续维护、调参和定位 bug。

---

## 1. 长期记忆是什么 / 为什么需要召回

DMContextService 自身**不存储**长期记忆，长期记忆由独立的 **KMM (Knowledge Memory Module)** 服务管理（运行在 `https://{kmm_ip}:27203`）。KMM 里存的是用户跨会话沉淀下来的事实/偏好，例如：

| 类型 | 例子 |
|---|---|
| 个人事实 | "用户的女儿生日是 7 月 4 日" |
| 偏好 | "用户喜欢喝美式咖啡，少糖少冰" |
| 历史决策 | "用户已下单瑞幸美式橙 C，大杯无糖加冰" |

每次用户提问时，DMContextService 要去 KMM 召回**与当前 query 相关的若干条记忆**，作为上下文一并返回给上游，让回答模型"知道用户是谁"。

---

## 2. 触发条件

只有 `/api/v1/contexts/query` 接口的**第一次查询**会发起召回：

- 走 `ExecuteFirstQuery`（即请求体没有 `response_id`）；
- 请求体 `enable_memory == true`（默认 `true`）。

第二次查询（带 `response_id`）**不会再去 KMM**——它从本地缓存取出第一次的召回结果直接用。

代码位置：`context_service.cpp:2741`
```cpp
if (params.enableMemory) {
    ProcessMemory(params, rewrittenQuery, responseId, g_turn_timestamp);
}
```

---

## 3. 整体设计：两阶段查询 + 异步召回

为了**降低首次查询的延迟**，整个召回是异步的：

```
┌──── 第一次查询 (无 response_id) ───────────────────────────────┐
│                                                               │
│   ExecuteFirstQuery                                           │
│     ├─ [同步] 拉历史 / 改写 / 拼 history → 立即返回响应        │
│     │           响应里 memory 字段是空的，但带回 responseId    │
│     │                                                         │
│     └─ [异步, m_ThreadPool 32 线程] ProcessMemory             │
│           ├─ RetrieveMemory  (调 KMM)                         │
│           ├─ AddMem          (结果存进 caches[responseId])    │
│           └─ ReleaseAddingStatus  (唤醒等待者)                 │
│                                                               │
└───────────────────────────────────────────────────────────────┘

  上游拿到 responseId，可能此时还在召回，也可能已经召回完了

┌──── 第二次查询 (带 response_id) ──────────────────────────────┐
│                                                               │
│   ExecuteSecondQuery                                          │
│     ├─ FindMem(responseId)  最多等 1500ms                     │
│     │     ├─ 召回早完成 → 立即拿到结果                         │
│     │     ├─ 召回中等待 → cv.wait_for 被唤醒                  │
│     │     └─ 1500ms 超时 → 拿到 nullptr，本次无 memory        │
│     │                                                         │
│     ├─ BuildMemoryStr  按 memory_count/memory_token_budget 截断│
│     └─ 把 memory 字符串拼到响应里返回                          │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

**为什么这样设计**：上游能立即拿到 history 开始展示界面或继续后续调用，而记忆召回（要等 KMM HTTP 来回，慢的话几百毫秒到 1 秒）在后台进行，第二次查询时再取。这样整体感知延迟更低。

---

## 4. 总流程图（含调用链 + 锁 + 关键变量）

```
[第一次查询]
  ExecuteFirstQuery (context_service.cpp:2653)
    │
    │ 已经走完：拉历史、改写、生成 responseId
    │
    └─► params.enableMemory ?
          │
          │ 是
          ▼
    ProcessMemory(params, rewrittenQuery, responseId, begin)  (2406)
      │
      ├─► TryAcquireAddingStatus(responseId)                  (2444)
      │     │   _adding_map[responseId].is_adding = true
      │     │
      │     ├─ 已经在召回 → 直接 return（避免重复）
      │     └─ 抢到锁 ↓
      │
      └─► m_ThreadPool->Submit([异步执行])                    (32 线程, 87-88)
            │
            ├─► RetrieveMemory(params, rewrittenQuery, longMem) (2473)
            │     │
            │     ├─ params.mode == "fast" ?
            │     │
            │     ├─ 是 ─► RetrieveMemoryByLightMode          (2486)
            │     │         ├─ ContextDbClient::GetKMMUrl(
            │     │         │     "/kmm/v1/user/memory/light_retrieval")
            │     │         └─ LightRetrievalMem(url, userId, query, longMem) (230)
            │     │               POST 到 KMM, 解析 data[].answer[]
            │     │
            │     └─ 否 ─► RetrieveMemoryByNormalMode         (2502)
            │               ├─ ContextDbClient::GetKMMUrl(
            │               │     "/kmm/v1/user/memories/get")
            │               └─ GetLongMem(url, userId, query, longMem) (172)
            │                     POST 到 KMM, 解析 data[].content
            │
            ├─► AddMem(responseId, content, rewrittenQuery,
            │           ids, longMem, duration)                (h:214)
            │     │
            │     │ caches[responseId] = std::make_shared<UserMemCache>()
            │     │ caches[responseId]->Add(responseId,        ← key 又是自己
            │     │                          oq, rq, ids, longMem, duration)
            │     │
            │     └─ MemCache 落到 caches[responseId] 里
            │
            └─► ReleaseAddingStatus(responseId)                (2458)
                  │   _adding_map[responseId].is_adding = false
                  │   status.cv.notify_all()  ← 唤醒可能在 FindMem 等的第二次查询
                  └─

[第二次查询]
  ExecuteSecondQuery (2572)
    │
    └─► FindMem(responseId, content)                          (h:234)
          │
          ├─ 锁 _map_mtx 查 _adding_map[responseId] → AddStatus
          ├─ 没找到 → return nullptr
          ├─ 找到 ↓
          │
          ├─ status.cv.wait_for(
          │      status_lock,
          │      1500ms,                                       ← 硬编码超时
          │      [&]{ return !status.is_adding; })
          │
          │   ├─ 已经完成 → 谓词为真，立刻返回
          │   ├─ 召回中 → 被 notify_all 唤醒
          │   └─ 超时 → 返回 false
          │
          ├─ 超时 → 删 _adding_map[responseId], return nullptr
          ├─ 等到 ↓
          │
          ├─ 从 caches[responseId] 取 UserMemCache::Get(responseId)
          │   返回 MemCache (含 longMem 列表)
          │
          └─ 删 _adding_map[responseId], return MemCache

    ▼
    BuildMemoryStr(memoryCount, memoryTokenBudget, memCache->longMem) (1690)
      │
      ├─ memoryCount > 0  → 取前 memoryCount 条
      ├─ memoryTokenBudget > 0 → 累加字符数到不超过 budget
      └─ 都 ≤0 → 全部塞进去
    ▼
    拼到 meta.memory，BuildHistoryResult 写进响应 JSON
```

---

## 5. 步骤详解

### 5.1 ProcessMemory —— 异步召回入口

**位置**：`context_service.cpp:2406-2442`

```cpp
void ContextService::ProcessMemory(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        const std::string& responseId,
        const long begin)
{
    if (!TryAcquireAddingStatus(responseId)) {
        return;  // 同一 responseId 已有召回在飞，不重复触发
    }
    m_ThreadPool->Submit([this, params, responseId, rewrittenQuery, begin]() {
        try {
            std::vector<std::string> longMem;
            std::vector<std::string> ids;

            RetrieveMemory(params, rewrittenQuery, longMem);  // 实际去 KMM

            int64_t duration_total = ...;
            AddMem(responseId, params.content, rewrittenQuery,
                   ids, longMem, duration_total);             // 结果落缓存
        } catch (...) { LOG_ERR(...); }
        ReleaseAddingStatus(responseId);                       // 唤醒等待者
    });
}
```

**关键点**：
- 不阻塞主线程；
- `TryAcquireAddingStatus` 是去重锁，防止同一 responseId 并发召回；
- 异常也会走到 `ReleaseAddingStatus`，否则 `FindMem` 会一直等到超时。

---

### 5.2 TryAcquireAddingStatus / ReleaseAddingStatus —— 在飞锁

**位置**：`context_service.cpp:2444-2471`

**作用**：用 `_adding_map[responseId].is_adding` 标记"该 responseId 正在召回中"，并配合 `cv` 让 `FindMem` 能等待。

```cpp
struct AddStatus {
    bool is_adding = false;
    std::mutex mtx;
    std::condition_variable cv;
};
std::unordered_map<std::string, AddStatus> _adding_map;
std::mutex _map_mtx;  // 保护 _adding_map 的增删
```

**TryAcquireAddingStatus 流程**：
```
锁 _map_mtx
  status = _adding_map[responseId]  // 不存在则插入新条目
解锁 _map_mtx

锁 status.mtx
  if (status.is_adding) return false   // 已经有人在飞
  status.is_adding = true               // 抢到
return true
```

**ReleaseAddingStatus 流程**：
```
锁 _map_mtx 找到 status
解锁

锁 status.mtx
  status.is_adding = false
  status.cv.notify_all()                ← 关键：唤醒 FindMem 里的 wait_for
```

**注意**：legacy 路径（`AddContext` / `submitAsyncMemoryTask`）也用这个 `_adding_map`，但 key 用的是 `userId`；新路径用 `responseId`。**共享同一个 namespace**，理论上 key 形态不同冲突概率低，但奇葩 key 形态可能撞车。

---

### 5.3 RetrieveMemory —— quality / fast 模式分发

**位置**：`context_service.cpp:2473-2516`

```cpp
void RetrieveMemory(...) {
    bool useLightRetrieval = (params.mode == "fast");
    if (useLightRetrieval) RetrieveMemoryByLightMode(...);
    else                    RetrieveMemoryByNormalMode(...);
}
```

`params.mode` 来源：
1. 请求体显式传 `"mode": "quality" | "fast"` → 用请求值；
2. 请求体没传 → 取 `query_config.mode`（默认 `"quality"`）。

代码位置：`context_service.cpp:1536-1537`、`97-112` (`GetQueryConfig`)。

#### Quality 模式（默认）—— `GetLongMem`

**KMM 接口**：`POST /kmm/v1/user/memories/get`

**位置**：`context_service.cpp:172-227`

**请求体**：
```json
{
  "query": "改写后的 query",
  "metadata": "{\"needTopicRetrival\":false,\"needSummary\":false}"
}
```
**请求头**：`userId: <user_id>`

**响应体**（关键字段）：
```json
{
  "data": [
    { "content": "记忆条文 1" },
    { "content": "记忆条文 2" },
    ...
  ]
}
```

**提取逻辑**：遍历 `data` 数组，取每条的 `content` 字段塞进 `longMem`。

#### Fast 模式 —— `LightRetrievalMem`

**KMM 接口**：`POST /kmm/v1/user/memory/light_retrieval`

**位置**：`context_service.cpp:230-292`

**请求体**：
```json
{
  "topK": 20,
  "query": [{"question": "改写后的 query"}]
}
```
- ⚠️ `topK=20` **硬编码**在 `context_service.cpp:237`，不可调；

**响应体**（关键字段）：
```json
{
  "data": [
    { "answer": ["记忆条文 1", "记忆条文 2"] },
    { "answer": ["记忆条文 3"] }
  ]
}
```

**提取逻辑**：遍历 `data[].answer[]` 把字符串都塞进 `longMem`。

#### 两种模式的区别

| 维度 | quality | fast |
|---|---|---|
| KMM 端点 | `memories/get` | `memory/light_retrieval` |
| 召回方式 | KMM 内部走完整召回（含 rerank） | KMM 走轻量级 topK 召回 |
| 速度 | 较慢 | 较快 |
| 召回质量 | 较高 | 较低 |
| 默认 | ✅ | — |

---

### 5.4 KMM 服务发现 —— `ContextDbClient::GetKMMUrl`

**位置**：`context_db_client.cpp:27-67`

```cpp
std::string ContextDbClient::GetKMMUrl(const std::string &path)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);

    // 从 RAG 表 tbl_user_classified_persona 查 user_id="kmm_ip" 这条
    auto retCode = retriever->ScalarSearch("tbl_user_classified_persona",
        QueryOption::Builder(SCALAR)
            .WithBaseSelectFields({"content"})
            .WithBaseFilters(
                FilterOption::Builder()
                    .WithFieldName("user_id").WithEQ()
                    .WithValue(VarCharValue("kmm_ip")).Build())
            .Build(),
        queryResult);

    if (queryResult 非空) {
        s_kmmIpCache = queryResult[0][0];   // 拿到 IP
    } else {
        s_kmmIpCache = "get_ip_error";       // 找不到
    }

    return "https://" + s_kmmIpCache + ":27203" + path;
}
```

**几个隐含问题**：
1. **端口 `27203` 硬编码**；
2. **`s_kmmIpCache` 名字叫缓存但实际每次都被覆盖**——每次都会查一次 RAG 表，没起到缓存作用；
3. 找不到时返回 `https://get_ip_error:27203/...`，会让 HTTP 调用失败但不会 throw。

---

### 5.5 AddMem —— 把召回结果落缓存

**位置**：`context_service.h:214-232`

```cpp
void AddMem(response_id, oq, rq, rqa_ids, longMem, duration)
{
    if (caches.find(response_id) == caches.end()) {
        caches[response_id] = std::make_shared<UserMemCache>();
    }
    caches[response_id]->Add(response_id,    // ← 注意：key 还是 response_id
                              oq, rq, rqa_ids, longMem, duration);
}
```

**`MemCache` 结构**（`context_service.h:49-66`）：
```cpp
struct MemCache {
    std::string originQ;                     // 原始 query
    std::string rewriteQ;                    // 改写后的 query
    std::vector<std::string> related_qa_ids; // (新接口里没用上)
    std::vector<std::string> longMem;        // ★ KMM 召回结果列表
    int64_t duration_total = 0;              // KMM 调用总耗时 (ms)
};
```

**`UserMemCache` 结构**（`context_service.h:104-177`）：
- 一个带 LRU 淘汰的缓存，`_limit{5}`；
- `_cache_map`: `unordered_map<key, {MemCache, list_iterator}>`
- `_order_list`: `list<string>`，记录访问顺序。

#### ⚠️ 设计冗余

新接口里 `caches[responseId]->Add(responseId, ...)`，**外层 key 是 responseId、内层 LRU 的 key 也是 responseId**，所以每个 UserMemCache 实际只存一条数据，`_limit{5}` 完全没用上。

这是历史遗留——legacy 路径（`submitAsyncMemoryTask`）一个 userId 下可能有多条 query 缓存，所以才需要 LRU。新路径继承了这个数据结构但没利用它。

---

### 5.6 FindMem —— 等待并取出召回结果

**位置**：`context_service.h:234-284`

完整流程：

```
锁 _map_mtx
  it = _adding_map.find(responseId)
  没找到 → return nullptr  (没有任何召回任务)
  找到 status
解锁 _map_mtx

锁 status.mtx
  status.cv.wait_for(1500ms, [&]{ return !status.is_adding; })
  ┌─ 召回早完成 (is_adding=false) → 谓词立即为真，wait_ok=true
  ├─ 召回中 → 阻塞等待 ReleaseAddingStatus 的 notify_all
  └─ 1500ms 超时 → wait_ok=false

  if (!wait_ok) {
    锁 _map_mtx
    _adding_map.erase(responseId)        // 清理键，避免泄漏
    return nullptr
  }
解锁 status.mtx

锁 _map_mtx
  从 caches[responseId] 取 MemCache
  _adding_map.erase(responseId)            // 用过即清
return MemCache
```

**几个关键点**：
- 1500ms 超时是**硬编码**在 `context_service.h:251`；
- 超时后**召回任务还在 m_ThreadPool 跑**，最终 `AddMem` 还会写进 `caches[responseId]`，但这条记录此时**没人删**——`caches` 没有 TTL 清理（不像 `m_contextCacheMap` 有 5min cleaner），属于潜在泄漏；
- 用过即从 `_adding_map` 删，所以**第三次查询同样的 responseId 会拿不到**（不影响业务，第二次查询已经把结果用掉了）。

---

### 5.7 BuildMemoryStr —— 按上限截断

**位置**：`context_service.cpp:1690-1723`

```cpp
std::string BuildMemoryStr(memoryCount, memoryTokenBudget, longMem)
{
    if (memoryCount > 0) {
        // 优先按条数截断：取前 memoryCount 条
        for (auto& mem : longMem) {
            if (count >= memoryCount) break;
            memoryStr += mem;
        }
    } else if (memoryTokenBudget > 0) {
        // 退路：按字符数截断
        for (auto& mem : longMem) {
            if (totalChars + mem.size() > memoryTokenBudget) break;
            memoryStr += mem;
        }
    } else {
        // 都没传 → 全部拼接
        for (auto& mem : longMem) memoryStr += mem;
    }
    return memoryStr;
}
```

**两种限制策略**：
| 字段 | 默认 | 优先级 | 含义 |
|---|---|---|---|
| `memory_count` | 20 | 高 | 取前 N 条 |
| `memory_token_budget` | 8192 | 低 | 累加字符数到不超过 budget（注意是字符数，不是真 token） |

> ⚠️ 字段叫 `token_budget` 但实际用的是 `mem.size()`（字节数），中文 UTF-8 1 字 = 3 字节，估算偏差较大。如果未来要改成真 token 计数，需要接入 `EstimateTokens`/`calculate_estimated_tokens`。

---

## 6. 完整时序图（典型场景）

### 场景 A：第二次查询时召回已完成

```
Time   主线程 (first query)              ThreadPool                主线程 (second query)
───────────────────────────────────────────────────────────────────────────────
0ms    ExecuteFirstQuery enter
       拉历史 / 改写
80ms   WriteResponse 返回 (无 memory) ──►
       TryAcquireAddingStatus ✓
       Submit 任务 ─────────────────────► RetrieveMemory
                                          GetLongMem (KMM HTTP)
                                          ...
500ms                                     KMM 返回，longMem 填好
                                          AddMem → caches[respId]
                                          ReleaseAddingStatus
                                          notify_all (无人在等)

3000ms                                                              ExecuteSecondQuery
                                                                    FindMem(respId)
                                                                    is_adding=false → 立即返回
                                                                    取 caches[respId]
                                                                    BuildMemoryStr → memory
                                                                    WriteResponse (含 memory)
```

### 场景 B：第二次查询时召回还在跑

```
Time   主线程 (first query)              ThreadPool                主线程 (second query)
───────────────────────────────────────────────────────────────────────────────
0ms    ExecuteFirstQuery enter
80ms   WriteResponse ──────────────────►
       Submit 任务 ─────────────────────► RetrieveMemory (慢 800ms)

500ms                                                               ExecuteSecondQuery
                                                                    FindMem(respId)
                                                                    is_adding=true
                                                                    cv.wait_for(1500ms) ⏳

880ms                                     RetrieveMemory done
                                          AddMem
                                          ReleaseAddingStatus
                                          notify_all ──────────────► wait_for 唤醒
                                                                    取 caches[respId]
                                                                    BuildMemoryStr
                                                                    WriteResponse (含 memory)
```

### 场景 C：超时（KMM 慢或不可用）

```
Time   主线程 (first query)              ThreadPool                主线程 (second query)
───────────────────────────────────────────────────────────────────────────────
0ms    Submit 任务 ─────────────────────► RetrieveMemory (KMM 卡住)
500ms                                                               FindMem
                                                                    cv.wait_for(1500ms) ⏳
2000ms                                                              超时
                                                                    erase(_adding_map[respId])
                                                                    return nullptr
                                                                    BuildMemoryStr 不调用
                                                                    WriteResponse (memory="")
3500ms                                    KMM 终于返回
                                          AddMem → caches[respId]   ← 没人来拿，泄漏
                                          ReleaseAddingStatus
                                          notify_all (无人在等)
```

---

## 7. 涉及的全部可调参数（速查）

| 步骤 | 参数 | 当前默认 | 当前是否可动态生效 |
|---|---|---|---|
| 触发 | 请求体 `enable_memory` | true | ✅ 入参 |
| 触发 | `enable_memory` 默认值 | true | ❌ 硬编码 (`context_service.cpp:1554`) |
| 模式 | 请求体 `mode` | quality | ✅ 入参 |
| 模式 | `query_config.mode` 默认 | quality | ✅ 每次重读 |
| Light KMM | `topK` | 20 | ❌ 硬编码 (`context_service.cpp:237`) |
| KMM 调用 | HTTP 超时 | 框架默认 | ❌ 没单独配 |
| KMM URL | 端口 | 27203 | ❌ 硬编码 (`context_db_client.cpp:66`) |
| FindMem | 等待超时 | 1500ms | ❌ 硬编码 (`context_service.h:251`) |
| 截断 | 请求体 `memory_count` | 20 | ✅ 入参 |
| 截断 | `memory_count` 默认值 | 20 | ❌ 硬编码 (`context_service.cpp:1572`) |
| 截断 | 请求体 `memory_token_budget` | 8192 | ✅ 入参 |
| 截断 | `memory_token_budget` 默认值 | 8192 | ❌ 硬编码 (`context_service.cpp:1574`) |
| 缓存 | `UserMemCache._limit` | 5 | ❌ 硬编码 (新接口实际没用上) |
| 缓存 | `caches` 容器 TTL | — | ❌ 没有清理机制（潜在泄漏） |

---

## 8. 已知问题清单

| # | 问题 | 影响 | 位置 |
|---|---|---|---|
| 1 | `caches` 没有 TTL 清理机制 | FindMem 超时后召回结果落进来就没人删 → 内存泄漏 | `context_service.cpp:218-219` |
| 2 | `UserMemCache` LRU 在新接口没用上 | `_limit{5}` 是死设计，每个 UserMemCache 只存一条 | `context_service.h:104-177` |
| 3 | `s_kmmIpCache` 名字叫缓存但每次都被覆盖 | 每次召回都查一次 RAG 表（无缓存效果） | `context_db_client.cpp:23,60` |
| 4 | `_adding_map` 在 legacy 和新路径共享 namespace | userId 和 responseId 形态不同问题不大，但极端情况会撞 | `context_service.cpp:2444-2471` |
| 5 | LightRetrievalMem `topK=20` 硬编码 | fast 模式无法调召回个数 | `context_service.cpp:237` |
| 6 | KMM 端口 `27203` 硬编码 | 改 KMM 端口需要改代码 | `context_db_client.cpp:66` |
| 7 | `memory_token_budget` 实际用的是字节数 | 字段名误导，中文偏差大 | `context_service.cpp:1709-1714` |
| 8 | `RetrieveMemoryByLightMode/NormalMode` 失败时 longMem 为空，但 AddMem 仍会写一条空记录 | 第二次查询能区分"召回失败"和"没召到"吗？拿到的 MemCache 非空但 longMem 为空 | `context_service.cpp:2486-2516` |

---

## 9. 一句话总结

> **第一次查询**结束后**异步**用改写后的 query 调 KMM 召回（quality 走 `memories/get`，fast 走 `light_retrieval`），结果落进 `caches[responseId]`；**第二次查询**通过 `FindMem` 等待最多 1500ms，从 `caches` 取出 longMem，按 `memory_count`/`memory_token_budget` 截断后拼进响应的 `memory` 字段。

---

## 10. 相关文件索引

| 文件 | 关键内容 |
|---|---|
| `context_service.cpp:172-227` | `GetLongMem`（quality 模式 KMM 调用） |
| `context_service.cpp:230-292` | `LightRetrievalMem`（fast 模式 KMM 调用，含 topK=20） |
| `context_service.cpp:1690-1723` | `BuildMemoryStr`（按条数/字符数截断） |
| `context_service.cpp:2406-2442` | `ProcessMemory`（异步召回入口） |
| `context_service.cpp:2444-2471` | `TryAcquireAddingStatus` / `ReleaseAddingStatus`（在飞锁） |
| `context_service.cpp:2473-2516` | `RetrieveMemory` + `RetrieveMemoryByLightMode` + `RetrieveMemoryByNormalMode` |
| `context_service.cpp:2572-2650` | `ExecuteSecondQuery`（调 FindMem + BuildMemoryStr） |
| `context_service.cpp:2653-2773` | `ExecuteFirstQuery`（结尾触发 ProcessMemory） |
| `context_service.h:49-66` | `MemCache` 结构 |
| `context_service.h:104-177` | `UserMemCache` LRU 实现 |
| `context_service.h:214-232` | `AddMem`（落缓存） |
| `context_service.h:234-284` | `FindMem`（等待并取结果，含 1500ms 超时） |
| `context_service.h:436-447` | `AddStatus` 结构、`_adding_map`、`caches`、`m_ThreadPool` |
| `context_db_client.cpp:27-67` | `GetKMMUrl`（KMM 服务发现） |
| `conf/dmcontextservice_config.json` | `query_config.mode`（quality/fast 默认） |
