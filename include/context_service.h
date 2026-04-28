/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#ifndef DMCONTEXTSERVICE_CONTEXT_SERVICE_H
#define DMCONTEXTSERVICE_CONTEXT_SERVICE_H

#include "bean/qa_short_memory.h"
#include "bean/user_conversation.h"
#include "logger.h"
#include "service_router/route_context.h"
#include "threadpool/threadpool.h"
#include <chrono>
#include <atomic>
#include <iterator>
#include <list>
#include <locale>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <timermanager/timer_manager.h>
#include <timermanager/timer_manager_model.h>
#include "message_buffer.h"
#include "model_log.h"
#include "HistoryBuilder.h"

namespace DMContext {
struct ContextItem {
    std::string query;
    std::string answer;
    std::string customSummary;
    std::string reWriteQuery;
    std::string date;
};

struct UserContext {
    std::string userId;
    std::string subUserId;
    std::string sessionId;
    std::string agentName;
    std::string label;
    std::vector<ContextItem> context;
};

struct MemCache {
    std::string originQ;
    std::string rewriteQ;
    std::vector<std::string> related_qa_ids;
    std::vector<std::string> longMem;
    int64_t duration_total = 0;

    MemCache(const std::string &oq, const std::string &rq, const std::vector<std::string> rqa_ids,
             const std::vector<std::string> longMem, int64_t duration)
        : originQ(oq),
          rewriteQ(rq),
          related_qa_ids(rqa_ids),
          longMem(longMem),
          duration_total(duration)
    {
    }
    MemCache(){};
};

// Volcanoes V2 上下文组装结果
struct ContextBuildResult {
    rapidjson::Value history;         // 组装后的上下文
    int32_t totalTokens = 0;              // 总 token 数
    int32_t historyTokenCount = 0;        // 历史对话 token 数
    int32_t historyTurns = 0;             // 历史对话轮数
    int32_t memoryTokenCount = 0;         // 长期记忆 token 数
    int code = 0;                         // 错误码
    std::string errorMsg;                 // 错误信息

    ContextBuildResult() : history(rapidjson::kArrayType) {}

    explicit ContextBuildResult(rapidjson::Document::AllocatorType* /*alloc*/)
        : history(rapidjson::kArrayType) {}
};

// 新增：存储完整上下文结果的缓存结构
struct ContextCache {
    std::string responseId;
    std::string originalQuery;                    // 原始查询
    std::string rewrittenQuery;                   // 改写后的查询
    std::string qaList;                            // 历史对话数据（JSON字符串）
    std::string abstractQA;                        // 摘要对话数据（JSON字符串）
    std::string assembledContext;                 // 组装后的上下文（JSON字符串）
    std::string cachedResponseJson;               // 缓存的响应JSON（直接返回用）
    std::vector<std::string> longMem;             // 长期记忆
    std::vector<std::shared_ptr<UserConversation>> conversations;  // 第一次查询的历史对话数据
    int32_t totalTokens = 0;
    int32_t historyTokenCount = 0;
    int32_t historyTurns = 0;
    int32_t memoryTokenCount = 0;
    int64_t timestamp;                            // 时间戳，用于过期清理

    ContextCache() : timestamp(time(nullptr)) {}
};

class UserMemCache {
public:
    UserMemCache() {}
    UserMemCache(size_t limit) : _limit(limit){};
    void Add(const std::string &user_id, const std::string &oq, const std::string &rq, const std::vector<std::string> rqa_ids,
             const std::vector<std::string> longMem, int64_t duration = 0)
    {
        std::shared_lock lock(_mtuex);

        auto it = _cache_map.find(user_id);
        if (it != _cache_map.end()) {
            it->second.value = std::make_shared<MemCache>(oq, rq, rqa_ids, longMem, duration);
            _order_list.splice(_order_list.end(), _order_list, it->second.listIt);
            return;
        }

        if (_cache_map.size() >= _limit) {
            std::string ordestKey = _order_list.front();
            _cache_map.erase(ordestKey);
            _order_list.pop_front();
        }
        _order_list.push_front(user_id);
        auto listIt = --_order_list.end();
        auto new_mem = std::make_shared<MemCache>(oq, rq, rqa_ids, longMem, duration);
        LOG_INFO("user_mem_cache insert rqa_ids_size %d, long_mem_size %d", rqa_ids.size(), longMem.size());
        _cache_map[user_id] = {new_mem, listIt};
    }

    std::shared_ptr<MemCache> Get(const std::string &user_id)
    {
        std::shared_lock lock{_mtuex};
        auto it = _cache_map.find(user_id);
        if (it == _cache_map.end()) {
            return nullptr;
        }
        return it->second.value;
    }

    int size()
    {
        return _order_list.size();
    }

    void Clear()
    {
        std::unique_lock lock(_mtuex);
        _cache_map.clear();
        _order_list.clear();
    }

    bool Remove(const std::string &user_id)
    {
        std::unique_lock lock(_mtuex);
        auto it = _cache_map.find(user_id);
        if (it == _cache_map.end()) {
            return false;
        }
        _order_list.erase(it->second.listIt);
        _cache_map.erase(it);
        return true;
    }

private:
    struct MapEntry {
        std::shared_ptr<MemCache> value;
        std::list<std::string>::iterator listIt;
    };

private:
    std::unordered_map<std::string, MapEntry> _cache_map;
    std::list<std::string> _order_list;
    mutable std::shared_mutex _mtuex;
    size_t _limit{5};
};

class ContextService {
public:
    static ContextService &GetInstance();

    ContextService(const ContextService &) = delete;

    ContextService &operator=(const ContextService &) = delete;

    ~ContextService();

    void SetCallBackFunc();

    void ProcessQACompleteCallback(const std::string& body);

    // 新接口：/context/write 入口
    void AddContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, std::string opType);
    void AddContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, std::string opType);

    std::vector<std::shared_ptr<QAShortMemory>> filterNotEmptyMemory(
        const std::vector<std::shared_ptr<QAShortMemory>>& shortMemories);
    void addQAPairsAndSummary(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
        const std::vector<std::string>& ids,
        std::vector<std::shared_ptr<QAShortMemory>>& notEmptyMemory,
        const UserContext& userContext);

    void GetContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    void submitAsyncMemoryTask(const QAFilter& qaFilter, int limit);

    // 新接口：/context/query 入口
    // isInternal: true 表示内部/老接口调用，false 表示外部/新版API调用
    void GetContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                        bool isInternal = true,
                        const std::chrono::system_clock::time_point& start_GetContext = std::chrono::system_clock::now());

    void AddMem(const std::string &response_id, const std::string &oq, const std::string &rq,
                const std::vector<std::string> &rqa_ids, const std::vector<std::string> &longMem, int64_t duration = 0)
    {
        try {
            if (auto it = caches.find(response_id); it == caches.end()) {
                caches[response_id] = std::make_shared<UserMemCache>();
            }
            // 标记为正在添加状态，并记录添加的查询信息

            caches[response_id]->Add(response_id, oq, rq, rqa_ids, longMem, duration);
            // 标记为添加完成状态

            LOG_INFO("AddMem in response_id %s, the query is %s, the size is %d, rqa_ids size %d, "
                     "longMem %d",
                     response_id.c_str(), oq.c_str(), caches[response_id]->size(), rqa_ids.size(), longMem.size());
        } catch (...) {
            throw;
        }
    }

    std::shared_ptr<MemCache> FindMem(const std::string &response_id, const std::string &oq)
    {
        std::unique_lock<std::mutex> map_lock(_map_mtx);
        auto it = _adding_map.find(response_id);
        if (it == _adding_map.end()) {
            LOG_ERR("FindMem failed, _adding_map is empty");
            return nullptr;
        }
        auto& status = it->second;
        map_lock.unlock();

        // ======================
        // 带超时等待
        // ======================
        std::unique_lock<std::mutex> status_lock(status.mtx);
        bool wait_ok = status.cv.wait_for(
                status_lock,
                std::chrono::milliseconds(1500),
                [&]() {
                    return !status.is_adding;
                }
        );

        // 超时了！
        if (!wait_ok) {
            // 超时也要删除 key，避免内存泄漏
            map_lock.lock();
            _adding_map.erase(response_id);
            LOG_INFO("FindMem failed, query is %s, timeout", oq.c_str());
            return nullptr; // 超时返回失败
        }
        status_lock.unlock();

        auto itCaches = caches.find(response_id);
        if (itCaches != caches.end()) {
            auto result = itCaches->second->Get(response_id);
            if (result != nullptr) {
                LOG_INFO("FindMem ok, query is %s ", oq.c_str());
                map_lock.lock();
                _adding_map.erase(response_id);
                return result;
            }
        }
        LOG_INFO("FindMem failed, query is %s, mem is null", oq.c_str());
        // ======================
        // 2. 逻辑处理完，再删除 key
        // ======================
        map_lock.lock();
        _adding_map.erase(response_id);
        return nullptr;
    }

    void DeleteContext(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    bool TryAcquireAddingStatus(const std::string& userId);
    void ReleaseAddingStatus(const std::string& userId);

    void ClearUserCache(const std::string& userId)
    {
        std::unique_lock lock(_mtuex);
        auto it = caches.find(userId);
        if (it != caches.end()) {
            it->second->Clear();
            caches.erase(it);
        }
    }

    void ClearAllCache()
    {
        std::unique_lock lock(_mtuex);
        caches.clear();
    }

    // 从UserConversation中查询数据进行改写
    std::string RewriteQueryFromUserConversation(const std::vector<std::shared_ptr<UserConversation>>& conversations,
                                                  const std::string& query, std::string& input);

    // 新接口：/context/delete 入口
    void DeleteContextNew(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    std::vector<std::shared_ptr<UserConversation>> GetConversations(
        const std::vector<std::shared_ptr<UserConversation>>& conversations);

    // GetContextNew 拆分出的辅助方法
    struct QueryParams {
        std::string userId;
        std::string originalUserId;  // 原始userId，未拼接前的值
        std::string srvId;  // 服务ID，与userId组合形成新的userId
        std::string contextId;
        std::string agentRole;
        std::string responseId;
        std::string queryType;
        std::string mode;
        bool enableMemory;
        bool isRewritequery;
        std::string reWriteContent;
        int32_t conversationTurns;  // 修正拼写：coversion_turns -> conversation_turns
        int32_t memoryCount;        // 记忆条数控制
        int32_t memoryTokenBudget;  // 长期记忆字符数预算
        int32_t tokenBudget;
        std::string outputType;
        std::string content;
        std::string voiceprintId;   // 生物特征信息-声纹ID
        std::string faceId;         // 生物特征信息-面部特征ID
        bool isForgetMemory;        // 是否是忘记记忆命令
    };

    // 解析请求参数
    static bool ParseQueryParams(const rapidjson::Document& doc, QueryParams& params);

    // 构造 QAFilter
    static QAFilter BuildQAFilter(const QueryParams& params);

    // 处理历史对话数据
    static void ProcessHistoryData(
                                   const std::vector<std::shared_ptr<UserConversation>>& conversations,
                                    rapidjson::Value& qaList, rapidjson::Value& abstractQA,
                                    const std::string& query,
                                    rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& alloc);

    // 获取历史对话数据
    static void QueryHistoryData(const QAFilter& qaFilter, int32_t limit,
                                  std::vector<std::shared_ptr<UserConversation>>& conversations);
    // 构建记忆字符串：根据 memoryCount 或 memoryTokenBudget 控制记忆内容
    static std::string BuildMemoryStr(int32_t memoryCount, int32_t memoryTokenBudget,
                                       const std::vector<std::string>& longMem);

    // 处理记忆并返回响应
    void ProcessMemory(
        const QueryParams& params,
        const std::string& rewrittenQuery,
        const std::string& responseId,
        const long begin);

    // ProcessMemory 拆分出的辅助方法
    void RetrieveMemory(const QueryParams& params, const std::string& rewrittenQuery,
                        std::vector<std::string>& longMem);
    void RetrieveMemoryByLightMode(const QueryParams& params, const std::string& rewrittenQuery,
                                    std::vector<std::string>& longMem);
    void RetrieveMemoryByNormalMode(const QueryParams& params, const std::string& rewrittenQuery,
                                    std::vector<std::string>& longMem);

    void ExecuteFirstQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
        const QueryParams& params,
        const QAFilter& qaFilter,
        std::string& responseId,
        const std::chrono::system_clock::time_point& start_GetContext);

    void ExecuteSecondQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context, const QueryParams& params,
                            const QAFilter& qaFilter,
                            const std::chrono::system_clock::time_point& start_GetContext);

    // 生成 response_id
    static std::string GenerateResponseId(const std::string& userId, const std::string& content);

    // AddContextNew 拆分出的辅助方法
    struct WriteParams {
        std::string userId;
        std::string srvId;  // 服务ID，与userId组合形成新的userId
        std::string contextId;
        std::string agentRole;
        std::string voiceprintId;
        std::string faceId;
        std::string taskStatus;  // 任务状态：Completed 表示完整QA，其他值表示不完整
        bool enableMemory;      // 是否开启长期记忆检索
        std::string uuid; //    // 数据库中id字段
        std::string opType; //    // 数据库操作类型 ，write--插入，update--更新，Completed--结束，启动压缩
        std::vector<std::shared_ptr<UserConversation>> conversations;
        bool ignoreThisRound;  // 是否忽略本轮处理
    };

    // 解析写入请求参数
    static bool ParseWriteParams(const rapidjson::Document& doc, WriteParams& params);

    // 解析 messages 并转换为 UserConversation 列表（支持对象和数组两种格式）
    static bool ParseMessagesToConversations(const rapidjson::Document& doc,
                                              const WriteParams& params,
                                              std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 创建 UserConversation 对象的辅助函数
    static std::shared_ptr<UserConversation> CreateUserConversation(
        const WriteParams& params, const std::string& role,
        const std::string& content, const std::string& metaData, const std::string& time);

    // 过滤有效消息（非空 answer）
    static std::vector<std::shared_ptr<UserConversation>> FilterValidConversations(
        const std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 写入数据库
    static bool SaveToDatabase(const std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 构建写入成功响应
    static void BuildWriteResponse(std::string& responseJson, int code, std::string& message);

    // 异步生成摘要
    void AsyncGenerateSummary(const std::vector<std::shared_ptr<UserConversation>>& conversations, WriteParams params);

    // 配置参数解析方法
    void InitConfigParams();

private:
    ContextService() = default;

    struct AddStatus {
        bool is_adding = false;
        std::mutex mtx;
        std::condition_variable cv;
    };

    static std::shared_ptr<CMFrm::ThreadPool::ThreadPool> m_ThreadPool;
    mutable std::shared_mutex _mtuex;
    std::map<std::string, std::shared_ptr<UserMemCache>> caches;

    std::mutex _map_mtx;  // 保护整个 map
    std::unordered_map<std::string, AddStatus> _adding_map;

    // 新增：上下文结果缓存管理
    std::map<std::string, std::shared_ptr<ContextCache>> m_contextCacheMap;
    mutable std::shared_mutex m_contextCacheMutex;

    // 新增：添加上下文缓存
    void AddContextCache(const std::string& responseId, const std::shared_ptr<ContextCache>& cache);

    // 新增：获取上下文缓存
    std::shared_ptr<ContextCache> GetContextCache(const std::string& responseId);

    // 查询配置结构体
    struct QueryConfig {
        std::string mode = "quality";
        int maxDbQueryTurns = 20;
    };

    static QueryConfig& GetQueryConfig();

    // 新增：清理过期缓存
    void CleanExpiredCache(int64_t expireSeconds = 300);

    // 新增：根据responseId删除单个上下文缓存
    void RemoveContextCache(const std::string& responseId);

    // 新增：定时清理过期上下文缓存任务
    void InitCleanExpiredContextCacheTimer();
    std::shared_ptr<CMFrm::Timer::TimerTask> m_contextCacheCleanTimer;

    std::thread m_writeCacheThread;            // 缓存写入线程
    std::atomic<bool> m_writeCacheRunning{false};

    // 新增：写入缓存相关 - 使用 unordered_map 实现 O(1) 查找
    std::unordered_map<std::string, std::shared_ptr<UserConversation>> m_writeCache;  // 写入缓存队列
    std::mutex m_writeCacheMutex;
    std::condition_variable m_writeCacheCv;

    std::shared_mutex m_processMemoryMutex;

    // 添加到写入缓存
    void AddToWriteCache(const std::vector<std::shared_ptr<UserConversation>>& conversations);

    // 更新写入缓存中的数据（根据GetId查找，缓存中有则更新，没有返回false）
    bool UpdateInWriteCache(const std::string& convId, const std::shared_ptr<UserConversation>& conversation);

    // 定时刷新缓存到数据库
    void FlushWriteCache();

    // 启动缓存相关定时任务
    void StartCacheTimer();

    // 停止缓存相关定时任务
    void StopCacheTimer();
};

    void buildAndWriteWriteLog(const std::string userId,
                               const std::string serviceId,
                               const std::string contextId,
                               const std::string uuId,
                               const std::string body,
                               const std::string responseJson,
                               const std::string turn,
                               const std::string qaExtractDelay,
                               const std::string qaSummaryDelay,
                               const std::string ts,
                               const std::string delay,
                               bool  flag);

    void buildAndWriteDeleteLog(const std::string userId,
                                const std::string serviceId,
                                const std::string contextId,
                                bool ret,
                                std::string turn,     // 对话轮次，用于标识同一轮对话
                                std::string ts,
                                std::string delay,
                                bool  flag);
    void buildAndWriteQueryLog(const ContextService::QueryParams& params,
                               rapidjson::Value* responseJson,
                               rapidjson::Value* abstractQAJson,
                               rapidjson::Value* qaListJson,
                               const std::string rewriteInput,
                               const std::string rewrittenQuery,
                               const std::string durationRewriteQuery,
                               const std::string query,
                               const std::string memoryResult,
                               const std::string durationMemory,
                               const std::string turnTimes,
                               const std::string ts,
                               const std::string durationDbQuery,
                               const std::string delay,
                               bool flag);

    bool ParseWriteApiRequest(const std::string& jsonStr, WriteApiRequest& request);
    bool ParseWriteApiResponse(const std::string& jsonStr, WriteApiResponse& response);
    bool ParseBiometricIdentity(const rapidjson::Value& biometricIdentityJson, BiometricIdentity& biometricIdentity);
    bool ParseAbstractQA(rapidjson::Value* abstractQAJson, AbstractQA& abstractQA);
    bool ParseQAItem(rapidjson::Value* qaItemJson, QAItem& qaItem);
    bool ParseResponseInfo(rapidjson::Value* responseDoc, ResponseInfo& responseInfo);
    rapidjson::Value BuildHistoryResultForJson(
        const rapidjson::Value& qaList,
        const rapidjson::Value& abstractQA,
        const HistoryBuildOptions& opts,
        const HistoryResultMeta& meta,
        rapidjson::Document::AllocatorType& alloc);

}  // namespace DMContext
#endif  // DMCONTEXTSERVICE_CONTEXT_SERVICE_H