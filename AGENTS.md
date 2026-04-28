# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Repository scope

This directory holds **only the implementation files** of the `DMContextService` microservice (4 `.cpp` files plus `dmcontextservice_config.json`). There are no headers, no build files (no CMake/Make/vcxproj), and no tests in this tree — the corresponding headers and the build system live in the larger Huawei **CoreMind / CMFrm** platform tree that this code is checked into and built from. Treat this directory as a working copy of files extracted from that larger tree: changes here must compile in that outer tree.

Implication: do not invent build/test commands. If the user asks how to build or run, tell them you can't see the build system from this tree. The runtime config root defaults to `/opt/coremind/conf/` and runtime logs to `/opt/coremind/logs/ModelLogs/` — both paths are baked into the code.

## Architecture

Three-layer pipeline. Requests enter via the framework's REST router → are dispatched to a `RouteMgr` handler → handed off to `ContextService` (or `ConfigService` / `RewriteRuleService`) on a thread pool.

### Entry point — `main.cpp`

Initialization order matters and is sequential with retries (`CMFrm::Utils::Retry`, 10 attempts × 5s backoff) on the modules that talk to external services:

1. `RouteMgr::InitRestRouter()` (registers handlers — must run **before** framework init so the router is attached)
2. `CMFrm::Starter::FrameworkStarter::InitFramework`
3. `ConfigMgr::InitConfigMgr()` (loads `dmcontextservice_config.json`)
4. `RagMgr::Init()` (retried)
5. `DatabaseMgr::InitDatabaseMgr()`
6. `MicroserviceMgr::Init()`
7. `ModelMgr::Init()` (retried)
8. Optional `AISF::EmbeddingService::Init` when `USE_REMOTE_EMBEDDING` is defined

### Routing — `route_mgr.cpp`

`RouteMgr` is a singleton holding two thread pools:
- `m_queryThreadPool` — 32 threads, name `router_mem_query`, used **only** for context-query handlers
- `m_threadPool` — 16 threads, name `router_normal`, used for writes, deletes, configs, prompts

All handlers return `CMFrm::COM::ASYNC` and submit work to the appropriate pool, except the rewrite-rule and `HandleContextFiles` handlers which run synchronously (`CMFrm::COM::SYNC`).

Three parallel families of context endpoints exist intentionally — **do not unify them without checking with the user**:
- `/v1/user/context/{add,query,delete}` — internal/AISF, no service grouping
- `/context/v1/{write,query,delete}` — internal alias, registered under service `ContextService`
- `/api/v1/contexts/{write,query,delete}` — external "ability development" API, distinct handlers (`HandleContextWrite`, `HandleContextQueryX`, `HandleContextDelete`) that call the **`*New` variants** in `ContextService`

`HandleContextFiles` (`GET /api/v1/contexts/files`) reads OM_-prefixed JSONL log files from `/opt/coremind/logs/ModelLogs/{serviceId}_{userId}/` and aggregates them into `context_id → turn → session_type` (where `session_type` ∈ {`query_api_1`, `query_api_2`, `delete_api`, `write_api`}; only `write_api` gets a `_<ts>` suffix on its key).

### Core service — `context_service.cpp`

`ContextService` is a Meyers singleton (`GetInstance()`) with its own internal 32-thread pool (`m_ThreadPool`) plus two background timers managed by `CMFrm::Timer::TimerManager`:
- **Write-cache flush timer** — every 100ms calls `FlushWriteCache()`, batch-saving accumulated `UserConversation` entries from `m_writeCache` to the DB in one batch (the legacy thread-based version is commented-out below it; do not delete the comment without confirming).
- **Expired-context-cache cleaner** — every 5 minutes runs `CleanExpiredCache()` over `m_contextCacheMap` (keyed by `responseId`).

Two parallel API generations live side-by-side in this file:

| Concept | Legacy (`AddContext` / `GetContext` / `DeleteContext`) | New (`AddContextNew` / `GetContextNew` / `DeleteContextNew`) |
|---|---|---|
| Storage abstraction | `QAShortMemoryMgr` + `QAShortMemory` | `UserConversation` + `SaveToDatabase` + write-cache batching |
| Request parsing | `parseJsonToUserContext` / `parseJsonToQAFilter` | `ParseQueryParams` / `ParseWriteParams` (rapidjson) |
| User ID | raw `userId` | composite `serviceId + "_" + userId` (set inside `ParseQueryParams`; also rebuilt inline in `DeleteContextNew`) |
| Response model | sync write of memory result | `responseId`-keyed two-stage query (see below) |
| Delete semantics | hard delete via `QAShortMemoryMgr::DeleteQAPairs` | **soft delete** — `ContextDbClient::UpdateContextStatus(..., "inactive")` |

Legacy `AddContext` / `GetContext` are split into small helpers:
- `filterNotEmptyMemory` — drops QA pairs whose assistant answer is empty (and back-fills `abstract_qa` with `original_qa` when missing)
- `addQAPairsAndSummary` — writes the response then runs `summaryContext` to backfill summaries
- `submitAsyncMemoryTask` — the legacy `needStartGetMemory == "true"` branch; **gated by `TryAcquireAddingStatus(userId)`** and clears the user cache before re-fetching

**Two-stage query flow** (new API only, in `GetContextNew`):
1. First call (no `response_id` in request) → `ExecuteFirstQuery` does the full work, generates a `responseId` via `GenerateResponseId`, stashes intermediate results (`rewrittenQuery`, `conversations`) in `m_contextCacheMap`, returns the `responseId` in the response. Memory retrieval kicks off in the background via `ProcessMemory`.
2. Subsequent call (same `response_id` provided) → `ExecuteSecondQuery` pops the cache entry (`RemoveContextCache` after read) and pulls the long-memory result from `FindMem(responseId, content)`. If the cache miss, it falls back to `QueryHistoryData`.

**Memory retrieval mode switch** — `query_config.mode` (`"quality"` default vs `"fast"`) drives `RetrieveMemory`:
- `quality` → `RetrieveMemoryByNormalMode` → KMM endpoint `/kmm/v1/user/memories/get` via `GetLongMem`
- `fast` → `RetrieveMemoryByLightMode` → KMM endpoint `/kmm/v1/user/memory/light_retrieval` via `LightRetrievalMem`

`ProcessMemory` is also gated by `TryAcquireAddingStatus(responseId)` to prevent duplicate concurrent retrievals for the same response. Note that the same `_adding_map` is keyed by `userId` (legacy path) and by `responseId` (new path) — they share namespace, so collisions in unusual key shapes would silently block.

The "forget memory" path: if the request `content` exactly matches one of the strings in `forget_memory_commands` from config, `ParseQueryParams` flips `isForgetMemory=true`, disables `enableMemory` and `isRewritequery`, and `ExecuteFirstQuery` issues `ContextDbClient::DeleteAllMemory(userId)` after responding.

There is also a per-call `g_qaCallback` (set via `SetQACompleteCallback`, registered in `SetCallBackFunc`) that buffers QA pairs for asynchronous summary generation; `FlushAllBuffers` is invoked by the destructor.

### Configuration — `config_mgr.cpp`

`ConfigMgr` loads `dmcontextservice_config.json` once at startup from `${DMCONTEXT_CONFIG_ROOT_PATH:-/opt/coremind/conf/}`. It supports **runtime mutation** through the `/api/v1/contexts/configs/*` and `/api/v1/contexts/prompts/*` REST endpoints, which call `UpdateConfig` / `UpdateArrayConfig` / `SaveConfigToFile`.

`SaveConfigToFile` rewrites the on-disk JSON via **temp-file-then-rename** (writes `*.tmp`, then `std::rename` over the original) — preserve this pattern when adding new persisted config sections, otherwise a crash mid-write will corrupt the file.

The known config sections (each with its own getter and a branch in `GetConfigByType` / `UpdateConfig` / `SaveConfigToFile`) are:
`com_params`, `aging_policy`, `memory_tbl_map`, `database`, `model`, `rerank_config`, `rewrite_rule_config`, `query_config`, `forget_memory_commands`.

Adding a new section means touching **all** of: `InitConfigMgr` (load), `GetAllConfigTypes` (list), `GetConfigByType` (read), `UpdateConfig` or `UpdateArrayConfig` (write), and `SaveConfigToFile` (persist). Easy to forget one.

## Conventions worth knowing

- **Logging:** use the `LOG_INFO` / `LOG_ERR` / `LOG_WARNING` macros from `logger.h`. Existing code logs entry/exit of every handler with `__FUNCTION__`, plus `[DMContext TIME-CONSUMING] <op> <ms>` timing lines around any submitted task — keep this pattern when adding handlers.
- **JSON:** the codebase mixes **rapidjson** (preferred for new code, used in `route_mgr` / `config_mgr` / `ContextService::*New`) and **jsoncpp** (`Json::Value`, used in legacy paths and in `DumpHistoryData`). When editing legacy code, match what's already there rather than converting.
- **Errors:** handlers return JSON with `code` (HTTP-style int) and `data`/`msg`. `BuildWriteResponse` is the canonical builder for write/delete responses; query responses build their JSON inline via `BuildHistoryResult` / `BuildHistoryResultForJson`.
- **Async response:** `CMFrm::COM::ASYNC` handlers must call `context->WriteAsyncResponse()` (directly or via `HttpHelper::WriteResponse`) on every code path, including error returns — otherwise the request hangs.
- **Adding-status guard:** `TryAcquireAddingStatus` / `ReleaseAddingStatus` form a per-key in-flight lock used to deduplicate background memory work. Always pair them — early returns inside a guarded block must release first.
- **Comments and copyright:** every file starts with a Huawei copyright header; preserve it on new files.
