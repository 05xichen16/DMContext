/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#include <framework_starter.h>
#include <route_mgr.h>
#include <utils/retry_util.h>
#include "embedding_service.h"
#include "database/rag/rag_mgr.h"
#include "logger.h"

#include "model_mgr.h"
#include "common_define.h"
#include "config_mgr.h"
#include "datatable/database_mgr.h"
#include "microservice/microservice_mgr.h"
#include "qa_short_memory_mgr.h"

using namespace DMContext;
int main(int argc, char *argv[])
{
    // # 注册 rest请求
    RouteMgr::GetInstance()->InitRestRouter();
    CMFrm::Starter::FrameworkInitParam frameworkInitParam;
    frameworkInitParam.SetProcessArgs(argc, argv);

    frameworkInitParam.AddServiceRouter(RouteMgr::GetInstance());

    auto ret = CMFrm::Starter::FrameworkStarter::InitFramework(frameworkInitParam);
    CHECK_ACT_RET(!ret, LOG_ERR("Starter Framework fail"), -1);

    // 初始化配置管理模块
    ConfigMgr::GetInstance()->InitConfigMgr();

    // 初始化rag sdk
    ret = CMFrm::Utils::Retry([]() -> bool { return RagMgr::GetInstance()->Init(); }, CMFrm::Constants::RetryCount10,
        CMFrm::Constants::RetryBackOffSecond5);
    CHECK_ACT_RET(!ret, LOG_ERR("RagMgr init fail"), -1);

    DatabaseMgr::GetInstance()->InitDatabaseMgr();
    LOG_INFO("finish databasemgr init");

    MicroserviceMgr::GetInstance()->Init();
    LOG_INFO("finish MicroserviceMgr init");

    // 模型模块初始化
    ret = CMFrm::Utils::Retry([]() -> bool { return ModelMgr::GetInstance()->Init(); }, CMFrm::Constants::RetryCount10,
                              CMFrm::Constants::RetryBackOffSecond5);
    CHECK_ACT_RET(!ret, LOG_ERR("ModelMgr init fail"), -1);
#if USE_REMOTE_EMBEDDING
    ret = CMFrm::Utils::Retry([]() -> bool { return AISF::EmbeddingService::Init(REMOTE_EMBEDDING_BASE_URL); },
                              CMFrm::Constants::RetryCount10, CMFrm::Constants::RetryBackOffSecond5);
    CHECK_ACT_RET(!ret, LOG_ERR("AISF EmbeddingService init fail"), -1);
#endif
    LOG_INFO("DMContextService finish");
    CMFrm::Starter::FrameworkStarter::WaitProcessExit();
}