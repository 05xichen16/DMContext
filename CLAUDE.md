# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository scope

This directory holds **only the implementation files and headers** of the `DMContextService` microservice. Layout:

```
.
├── main.cpp
├── route_mgr.cpp
├── config_mgr.cpp           ConfigService.cpp / context_service.cpp / context_builder.cpp / context_db_client.cpp / model_infer.cpp
├── rewrite_rules/
│   └── rewrite_rule_service.cpp
├── include/                 ← all corresponding headers (config_mgr.h, context_service.h, …)
│   └── rewrite_rules/rewrite_rule_service.h
└── conf/                    ← runtime config root (mirrors what gets shipped to /opt/coremind/conf/)
    ├── dmcontextservice_config.json
    ├── model_url.json
    └── prompt.json
```

There are **no build files** (no CMake/Make/vcxproj) and **no tests** in this tree — the build system lives in the larger Huawei **CoreMind / CMFrm** platform tree that this code is checked into. Treat this directory as a working copy of files extracted from that larger tree: changes here must compile in that outer tree.

Implication: do not invent build/test commands. If the user asks how to build or run, tell them you can't see the build system from this tree. Runtime defaults: config root `${DMCONTEXT_CONFIG_ROOT_PATH:-/opt/coremind/conf/}`, log root `/opt/coremind/logs/ModelLogs/` — both paths are baked into the code.

## Architecture

Three-layer pipeline. Requests enter via the framework's REST router → are dispatched to a `RouteMgr` handler → handed off to `ContextService` (or `ConfigService` / `RewriteRuleService`) on a thread pool.

### Entry point — `main.cpp`

Initialization order matters and is sequential with retries (`CMFrm::Utils::Retry`, 10 attempts × 5s backoff) on the modules that talk to external services:

1. `RouteMgr::InitRestRouter()` (registers handlers — must run **before** framework init)
2. `CMFrm::Starter::FrameworkStarter::InitFramework`
3. `ConfigMgr::InitConfigMgr()` (loads `dmcontextservice_config.json`)
4. `RagMgr::Init()` (retried)
5. `DatabaseMgr::InitDatabaseMgr()`
6. `MicroserviceMgr::Init()`
7. `ModelMgr::Init()` (retried)
8. Optional `AISF::EmbeddingService::Init` when `USE_REMOTE_EMBEDDING` is defined

### Routing — `route_mgr.cpp`

`RouteMgr` is a singleton holding two thread pools:
- `m_queryThreadPool` — 32 threads, name `router_mem_query`, **only** for context-query handlers
- `m_threadPool` — 16 threads, name `router_normal`, for writes/deletes/configs/prompts

All handlers return `CMFrm::COM::ASYNC` and submit work to the appropriate pool, except the rewrite-rule and `HandleContextFiles` handlers which run synchronously (`CMFrm::COM::SYNC`).

Three parallel families of context endpoints exist intentionally — **do not unify them without checking with the user**:
- `/v1/user/context/{add,query,delete}` — internal/AISF, no service grouping
- `/context/v1/{write,query,delete}` — internal alias, registered under service `ContextService`
- `/api/v1/contexts/{write,query,delete}` — external "ability development" API, distinct handlers (`HandleContextWrite`, `HandleContextQueryX`, `HandleContextDelete`) that call the **`*New` variants** in `ContextService`

`HandleContextFiles` (`GET /api/v1/contexts/files`) reads OM_-prefixed JSONL log files from `/opt/coremind/logs/ModelLogs/{serviceId}_{userId}/` and aggregates them into `context_id → turn → session_type` (where `session_type` ∈ {`query_api_1`, `query_api_2`, `delete_api`, `write_api`}; only `write_api` gets a `_<ts>` suffix on its key).

### Core service — `context_service.cpp` / `include/context_service.h`

`ContextService` is a Meyers singleton (`GetInstance()`) with its own internal 32-thread pool (`m_ThreadPool`) plus two background timers managed by `CMFrm::Timer::TimerManager`:
- **Write-cache flush timer** — every 100ms calls `FlushWriteCache()`, batch-saving accumulated `UserConversation` entries from `m_writeCache` to the DB.
- **Expired-context-cache cleaner** — every 5 minutes runs `CleanExpiredCache()` over `m_contextCacheMap`. Default expiry is **300 seconds** (`CleanExpiredCache(int64_t expireSeconds = 300)` in the header).

Two parallel API generations live side-by-side:

| Concept | Legacy (`AddContext` / `GetContext` / `DeleteContext`) | New (`AddContextNew` / `GetContextNew` / `DeleteContextNew`) |
|---|---|---|
| Storage | `QAShortMemoryMgr` + `QAShortMemory` | `UserConversation` + `SaveToDatabase` + write-cache batching |
| Request parsing | `parseJsonToUserContext` / `parseJsonToQAFilter` | `ParseQueryParams` / `ParseWriteParams` (rapidjson) |
| User ID | raw `userId` | composite `serviceId + "_" + userId` (set inside `ParseQueryParams`; rebuilt inline in `DeleteContextNew`) |
| Response model | sync write of memory result | `responseId`-keyed two-stage query (see below) |
| Delete semantics | hard delete via `QAShortMemoryMgr::DeleteQAPairs` | **soft delete** — `ContextDbClient::UpdateContextStatus(..., "inactive")` |

**Two caches, distinct roles:**
- `m_contextCacheMap` (`std::map<responseId, ContextCache>`, mutex `m_contextCacheMutex`) — full first-query result snapshot (rewrittenQuery, qaList, abstractQA, assembledContext, conversations, cachedResponseJson). Cleared on `RemoveContextCache` from `ExecuteSecondQuery`, or by the 5-min timer at 300s TTL.
- `caches` (`std::map<responseId, UserMemCache>`) — long-memory results from `ProcessMemory`. `UserMemCache` is an LRU with default `_limit{5}` per key.
- `_adding_map` (`std::unordered_map<key, AddStatus>`, mutex `_map_mtx`) — per-key in-flight lock used by `TryAcquireAddingStatus` / `ReleaseAddingStatus`. Same map is keyed by `userId` on the legacy path and `responseId` on the new path — collisions in unusual key shapes would silently block.
- `FindMem` waits on `AddStatus.cv` with a **hardcoded 1500ms timeout** (`context_service.h:251`).

**Two-stage query flow** (new API only, in `GetContextNew`):
1. First call (no `response_id`) → `ExecuteFirstQuery` does the full work, generates a `responseId` via `GenerateResponseId`, stashes intermediate results (`rewrittenQuery`, `conversations`, …) in `m_contextCacheMap`, returns the `responseId`. Memory retrieval kicks off in the background via `ProcessMemory`.
2. Subsequent call (same `response_id`) → `ExecuteSecondQuery` pops the cache entry (`RemoveContextCache` after read) and pulls long-memory from `FindMem(responseId, content)`. On cache miss, falls back to `QueryHistoryData`.

**Memory retrieval mode switch** — `query_config.mode` (default `"quality"` from `QueryConfig` struct in `context_service.h:461`, also overridable per-request) drives `RetrieveMemory`:
- `quality` → `RetrieveMemoryByNormalMode` → KMM `/kmm/v1/user/memories/get` via `GetLongMem`
- `fast` → `RetrieveMemoryByLightMode` → KMM `/kmm/v1/user/memory/light_retrieval` via `LightRetrievalMem`

Both `ProcessMemory` and the legacy `submitAsyncMemoryTask` are gated by `TryAcquireAddingStatus(...)`.

**Forget-memory path:** if the request `content` exactly matches one of the strings in `forget_memory_commands` from config, `ParseQueryParams` flips `isForgetMemory=true`, disables `enableMemory` and `isRewritequery`, and `ExecuteFirstQuery` issues `ContextDbClient::DeleteAllMemory(userId)` after responding.

A per-call `g_qaCallback` (set via `SetQACompleteCallback`, registered in `SetCallBackFunc`) buffers QA pairs for asynchronous summary generation; `FlushAllBuffers` is invoked by the destructor.

### Configuration — `config_mgr.cpp` + `config_service.cpp`

Two-layer split:
- **`ConfigMgr`** (`config_mgr.cpp` + `include/config_mgr.h`) — singleton owning all config state. Loads `dmcontextservice_config.json` once at `InitConfigMgr()` from `${DMCONTEXT_CONFIG_ROOT_PATH:-/opt/coremind/conf/}`. Provides typed getters (`GetCommonParamsByKey`, `GetRerankConfigParams`, …) plus generic `GetConfigByType` / `UpdateConfig` / `UpdateArrayConfig` / `SaveConfigToFile`.
- **`ConfigService`** (`config_service.cpp` + `include/config_service.h`) — REST handler layer that the `RouteMgr` config/prompt handlers delegate to. Parses request params, calls `ConfigMgr` (or `ModelMgr` for prompts), and **automatically calls `SaveConfigToFile()` / `SavePromptsToFile()` on every successful update**. So `PUT /api/v1/contexts/configs/item` already gives the "internal state + on-disk file both updated" semantics required by the runtime-config requirement.

`SaveConfigToFile` rewrites the on-disk JSON via **temp-file-then-rename** (writes `*.tmp`, then `std::rename` over the original) — preserve this pattern when adding new persisted config sections, otherwise a crash mid-write will corrupt the file.

The known config sections (each with its own getter and a branch in `GetConfigByType` / `UpdateConfig` / `SaveConfigToFile`) are:
`com_params`, `aging_policy`, `memory_tbl_map`, `database`, `model`, `rerank_config`, `rewrite_rule_config`, `query_config`, `forget_memory_commands`.

Adding a new section means touching **all** of: `InitConfigMgr` (load), `GetAllConfigTypes` (list), `GetConfigByType` (read), `UpdateConfig` or `UpdateArrayConfig` (write), and `SaveConfigToFile` (persist) — plus declaring the member in `include/config_mgr.h`. Easy to forget one.

**Snapshot-once gotcha** — `RewriteRuleService::GetRewriteRuleConfig()` lazily initializes a `RewriteRuleConfig` struct via `std::call_once` (`include/rewrite_rules/rewrite_rule_service.h:189-193`). Once constructed it is **never refreshed from `ConfigMgr`**. So `PUT /api/v1/contexts/configs/item?type=rewrite_rule_config` updates the map and the file but the rewrite algorithm continues using the originally-loaded values until restart. Anything new that needs to be live-reloaded must read from `ConfigMgr` on each call, not snapshot at init.

### History assembly — `context_builder.cpp` (separate from `HistoryBuilder.h`)

`BuildContextMsgAdaptive` (signature in `include/context_builder.h`) is a **token-budgeted, adaptive** history builder used independently of the `BuildHistoryResult` / `BuildHistory` family declared in the external `HistoryBuilder.h`. Strategy: take the latest raw turn if its token count ≤ `LAST_ROUND_TOKEN_THRESHOLD` (hardcoded **1000** despite the docstring saying 200), else use its compressed version; then fill backwards from `abstractQa` until the `maxTokens` budget (default **3000**) is hit. Two pieces of dead-reckoning live here that the new "history assembly strategy" requirement may want to surface as config.

### Downstream client — `context_db_client.cpp`

`ContextDbClient::GetKMMUrl(path)` is **not** standard service discovery. It scalar-queries the RAG table `tbl_user_classified_persona` for `user_id == "kmm_ip"`, caches the IP in `s_kmmIpCache`, and constructs `https://<ip>:27203<path>`. Port `27203` is hardcoded. KMM REST path constants are at the top of `include/context_db_client.h`.

### Model inference — `model_infer.cpp`

`LLMModelInfer` uses a **hardcoded 60s timeout** on the model response (`cond.wait_for(lock, std::chrono::seconds(60), …)`). `LLMModelInferSyncWithTaskType` reads model name and service name from env vars (default `LLM_MODEL_32B` and `LLM_MODEL_SERVICE_NAME`) via `std::getenv` — **passes the result directly into `std::string`'s constructor, will crash if the env var is unset**. Both names can be overridden per-call.

## Conventions worth knowing

- **Logging:** use `LOG_INFO` / `LOG_ERR` / `LOG_WARNING` from `logger.h`. Existing code logs entry/exit of every handler with `__FUNCTION__`, plus `[DMContext TIME-CONSUMING] <op> <ms>` timing lines around any submitted task — keep this pattern.
- **JSON:** the codebase mixes **rapidjson** (preferred for new code, used in `route_mgr` / `config_mgr` / `config_service` / `ContextService::*New`) and **jsoncpp** (`Json::Value`, used in legacy paths and in `DumpHistoryData`). When editing legacy code, match what's there rather than converting.
- **Errors:** handlers return JSON `{code, message|data}`. `BuildWriteResponse` builds write/delete responses; query responses use `BuildHistoryResult` / `BuildHistoryResultForJson`; `ConfigService` builds responses inline with rapidjson.
- **Async response:** `CMFrm::COM::ASYNC` handlers must call `context->WriteAsyncResponse()` (directly or via `HttpHelper::WriteResponse`) on every code path, including error returns — otherwise the request hangs.
- **Adding-status guard:** `TryAcquireAddingStatus` / `ReleaseAddingStatus` form a per-key in-flight lock used to deduplicate background memory work. Always pair them — early returns inside a guarded block must release first.
- **Default-only env vars:** `DMCONTEXT_CONFIG_ROOT_PATH` (config root, defaults to `/opt/coremind/conf/`), `LLM_MODEL_32B` and `LLM_MODEL_SERVICE_NAME` (no defaults, will crash if absent).
- **Per-request override of config:** `ParseQueryParams` lets the caller override `mode` / `conversation_turns` / `memory_count` / `memory_token_budget` / `token_budget` / `enable_memory` / `rewrite_query` per request. Config-file values are defaults/fallbacks. Keep this layered semantics when adding new tunables.
- **Comments and copyright:** every file starts with a Huawei copyright header; preserve it on new files.
