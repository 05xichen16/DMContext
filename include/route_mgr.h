/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */
#ifndef DMCONTEXT_SERVICE_SRC_ROUTE_ROUTE_H_
#define DMCONTEXT_SERVICE_SRC_ROUTE_ROUTE_H_
#include "singleton.h"
#include "service_router/router_manager.h"
#include "threadpool/threadpool.h"

namespace DMContext {
class RouteMgr final : public CMFrm::ServiceRouter::APIRouter {
public:
    static std::shared_ptr<RouteMgr> GetInstance();
    static std::shared_ptr<RouteMgr> CreateInstance();

    void RegisterRouter(const CMFrm::ServiceRouter::UsrServiceRouter &router);

    std::string GetAPIProvider() override;

    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> URLPatterns() override;

    void InitRestRouter();

    std::shared_ptr<CMFrm::ThreadPool::ThreadPool> GetQueryThreadPool();

    std::shared_ptr<CMFrm::ThreadPool::ThreadPool> GetCommThreadPool();
    static CMFrm::COM::ServerRespHandleMode HandleContextWrite(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    static CMFrm::COM::ServerRespHandleMode HandleContextQueryInternal(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    static CMFrm::COM::ServerRespHandleMode HandleContextDeleteInternal(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    static CMFrm::COM::ServerRespHandleMode HandleContextDelete(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 兼容旧接口（保留以便迁移）
    static CMFrm::COM::ServerRespHandleMode HandleContextAdd(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 新版API接口（/api/v1/contexts/xxx）
    static CMFrm::COM::ServerRespHandleMode HandleContextQueryX(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 查询改写规则管理接口（/api/v1/contexts/rewrite-rules/xxx）
    static CMFrm::COM::ServerRespHandleMode HandleRewriteRulesImport(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    static CMFrm::COM::ServerRespHandleMode HandleRewriteRuleQuery(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 获取日志文件列表接口
    static CMFrm::COM::ServerRespHandleMode HandleContextFiles(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    // 通用配置外部注入接口
    static CMFrm::COM::ServerRespHandleMode HandleConfigGetTypes(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandleConfigGetByType(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandleConfigGet(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandleConfigUpdate(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandlePromptGet(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandlePromptUpdate(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);
    static CMFrm::COM::ServerRespHandleMode HandlePromptBatchUpdate(
        const std::shared_ptr<CMFrm::ServiceRouter::Context> &context);

    RouteMgr() = default;

private:

    std::mutex m_routerLock;
    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> m_router;
    // 召回线程池，单请求耗时久
    std::shared_ptr<CMFrm::ThreadPool::ThreadPool> m_queryThreadPool;
    // 其他非耗时请求
    std::shared_ptr<CMFrm::ThreadPool::ThreadPool> m_threadPool;
};
}

#endif  // DMCONTEXT_SERVICE_SRC_ROUTE_ROUTE_H_