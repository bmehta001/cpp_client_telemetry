// Copyright (c) Microsoft Corporation. All rights reserved.

#include "common/Common.hpp"
#include "common/MockIHttpClient.hpp"
#include "http/HttpClientManager.hpp"

#include "NullObjects.hpp"
#include "ILogManager.hpp"

#include <chrono>

using namespace testing;
using namespace MAT;

static NullLogManager dummyLogManager;

class HttpClientManager4Test : public HttpClientManager {
  public:
    HttpClientManager4Test(IHttpClient& httpClient)
      : HttpClientManager(dummyLogManager, httpClient, *PAL::getDefaultTaskDispatcher())
    {
    }

    virtual void scheduleOnHttpResponse(HttpCallback* callback) override
    {
        onHttpResponse(callback);
    }
};

class HttpClientManagerTests : public StrictMock<Test> {
  protected:
    MockIHttpClient        httpClientMock;
    HttpClientManager4Test hcm;

    RouteSink<HttpClientManagerTests, EventsUploadContextPtr const&> requestDone{this, &HttpClientManagerTests::resultRequestDone};

  protected:
    HttpClientManagerTests()
      : hcm(httpClientMock)
    {
        hcm.requestDone >> requestDone;
    }

    MOCK_METHOD1(resultRequestDone, void(EventsUploadContextPtr const &));
};

class HttpClientManagerDeferredResponse4Test : public HttpClientManager {
  public:
    HttpClientManagerDeferredResponse4Test(IHttpClient& httpClient)
      : HttpClientManager(dummyLogManager, httpClient, *PAL::getDefaultTaskDispatcher())
    {
    }

    virtual void scheduleOnHttpResponse(HttpCallback* callback) override
    {
        onHttpResponse(callback);
    }

    virtual std::chrono::milliseconds getCancelAllRequestsDrainTimeout() const override
    {
        return std::chrono::milliseconds(20);
    }
};

class HttpClientManagerDeferredResponseTests : public StrictMock<Test> {
  protected:
    MockIHttpClient                        httpClientMock;
    HttpClientManagerDeferredResponse4Test hcm;

    RouteSink<HttpClientManagerDeferredResponseTests, EventsUploadContextPtr const&> requestDone{this, &HttpClientManagerDeferredResponseTests::resultRequestDone};

  protected:
    HttpClientManagerDeferredResponseTests()
      : hcm(httpClientMock)
    {
        hcm.requestDone >> requestDone;
    }

    MOCK_METHOD1(resultRequestDone, void(EventsUploadContextPtr const &));
};


TEST_F(HttpClientManagerTests, HandlesRequestFlow)
{
    SimpleHttpRequest* req = new SimpleHttpRequest("HttpClientManagerTests");

    auto ctx = std::make_shared<EventsUploadContext>();
    ctx->httpRequestId = req->GetId();
    ctx->httpRequest = req;
    ctx->recordIdsAndTenantIds["r1"] = "t1"; ctx->recordIdsAndTenantIds["r2"] = "t1";
    ctx->latency = EventLatency_Normal;
    ctx->packageIds["tenant1-token"] = 0;

    IHttpResponseCallback* callback = nullptr;
    EXPECT_CALL(httpClientMock, SendRequestAsync(ctx->httpRequest, _))
        .WillOnce(SaveArg<1>(&callback));
    hcm.sendRequest(ctx);
    ASSERT_THAT(callback, NotNull());

    PAL::sleep(200);

    std::unique_ptr<SimpleHttpResponse> rsp(new SimpleHttpResponse("HttpClientManagerTests"));
    rsp->m_result = HttpResult_OK;
    rsp->m_statusCode = 200;

    EXPECT_CALL(*this, resultRequestDone(ctx))
        .WillOnce(Return());
    IHttpResponse* rspRef = rsp.get();
    callback->OnHttpResponse(rsp.release());

    EXPECT_THAT(ctx->httpResponse, rspRef);
    EXPECT_THAT(ctx->durationMs, Gt(199));
}

TEST_F(HttpClientManagerDeferredResponseTests, CancelAllRequestsReturnsWhenResponseCallbackIsPending)
{
    SimpleHttpRequest* req = new SimpleHttpRequest("HttpClientManagerDeferredResponseTests");

    auto ctx = std::make_shared<EventsUploadContext>();
    ctx->httpRequestId = req->GetId();
    ctx->httpRequest = req;
    ctx->recordIdsAndTenantIds["r1"] = "t1";
    ctx->latency = EventLatency_Normal;
    ctx->packageIds["tenant1-token"] = 0;

    IHttpResponseCallback* callback = nullptr;
    EXPECT_CALL(httpClientMock, SendRequestAsync(ctx->httpRequest, _))
        .WillOnce(SaveArg<1>(&callback));
    hcm.sendRequest(ctx);
    ASSERT_THAT(callback, NotNull());

    std::unique_ptr<SimpleHttpResponse> rsp(new SimpleHttpResponse(req->GetId()));
    rsp->m_result = HttpResult_Aborted;
    rsp->m_statusCode = 0;

    IHttpResponse* rspRef = rsp.get();
    EXPECT_THAT(hcm.requestCount(), Eq(1u));

    const auto start = std::chrono::steady_clock::now();
    hcm.cancelAllRequests();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_THAT(elapsedMs, Lt(1000));
    EXPECT_THAT(hcm.requestCount(), Eq(1u));

    EXPECT_CALL(*this, resultRequestDone(ctx))
        .WillOnce(Return());
    callback->OnHttpResponse(rsp.release());

    EXPECT_THAT(ctx->httpResponse, rspRef);
    EXPECT_THAT(hcm.requestCount(), Eq(0u));
}
