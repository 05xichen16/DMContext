/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */


#ifndef HTTP_HELPER_H
#define HTTP_HELPER_H
#include <memory>
#include "common_define.h"
#include <service_router/route_context.h>

namespace DMContext {
class HttpHelper {
public:
    static void WriteResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, const std::string &body,
                              CMFrm::COM::HttpStatus code);

    static void WriteResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, int code,
                              const std::string &content);

    static void WriteResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, int code,
                              const std::string &content, const std::string &dataListString);

    static void WriteResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, std::string &filterRet,
                              const std::string &content, bool isFilterRetFormat = false);

    static void WriteRespWithPageInfo(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                      std::string &filterRet, const std::string &content, std::string &pageInfo);

    static bool Http2SendRequest(const std::string &url, const std::string &body);

    static void WriteRespWithMap(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                 CMFrm::COM::HttpStatus code, const std::string &retMsg,
                                 const std::map<std::string, std::string> &retMap);

    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp);

    static std::tuple<std::string, ErrorCode> InnerCurlService(const std::string &url, const std::string &requestBody,
                                                               const std::string &userId);
};
}

#endif  //HTTP_HELPER_H