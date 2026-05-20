// Copyright (c) Microsoft Corporation. All rights reserved.

#include "common/Common.hpp"
#include "common/MockIHttpClient.hpp"
#include "http/HttpClientManager.hpp"

#include "NullObjects.hpp"
#include "ILogManager.hpp"

#include <atomic>
#include <ctime>
#include <thread>
#include <vector>

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

namespace {

class CapturingHttpClient : public IHttpClient
{
  public:
    virtual IHttpRequest* CreateRequest() override
    {
        return new SimpleHttpRequest("CapturingHttpClient");
    }

    virtual void SendRequestAsync(IHttpRequest*, IHttpResponseCallback* callback) override
    {
        callbacks.push_back(callback);
    }

    virtual void CancelRequestAsync(std::string const&) override
    {
    }

    std::vector<IHttpResponseCallback*> callbacks;
};

EventsUploadContextPtr makeUploadContext(size_t index)
{
    auto ctx = std::make_shared<EventsUploadContext>();
    auto id = std::string("CancelAllRequestsRaceRepro") + std::to_string(index);
    auto req = new SimpleHttpRequest(id);

    ctx->httpRequestId = req->GetId();
    ctx->httpRequest = req;
    ctx->recordIdsAndTenantIds["r1"] = "t1";
    ctx->latency = EventLatency_Normal;
    ctx->packageIds["tenant1-token"] = 0;

    return ctx;
}

class BlockingRequestDone
{
  public:
    RouteSink<BlockingRequestDone, EventsUploadContextPtr const&> sink{this, &BlockingRequestDone::onRequestDone};

    void onRequestDone(EventsUploadContextPtr const&)
    {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            PAL::sleep(1);
        }
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

uint64_t getCurrentThreadCpuTimeNs()
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts = {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }
#endif
    return 0;
}

} // namespace


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

// Reproduces the #1437 / #1429 100% CPU spin.
// HttpClientManager::onHttpResponse invokes requestDone while holding m_httpCallbacksMtx.
// Before #1429, cancelAllRequests() ignored that mutex and yield-spun on m_httpCallbacks.empty().
// Run with:
//   out/tests/unittests/UnitTests --gtest_filter=HttpClientManagerSpinReproTests.CancelAllRequestsDoesNotSpinWhileResponseCallbackHoldsCallbackLock
TEST(HttpClientManagerSpinReproTests, CancelAllRequestsDoesNotSpinWhileResponseCallbackHoldsCallbackLock)
{
    CapturingHttpClient httpClient;
    HttpClientManager4Test hcm(httpClient);
    BlockingRequestDone blockingRequestDone;
    hcm.requestDone >> blockingRequestDone.sink;

    auto firstCtx = makeUploadContext(0);
    hcm.sendRequest(firstCtx);
    ASSERT_EQ(1U, hcm.requestCount());

    std::thread responseThread([&]() {
        auto response = new SimpleHttpResponse(firstCtx->httpRequestId);
        response->m_result = HttpResult_Aborted;
        httpClient.callbacks[0]->OnHttpResponse(response);
    });

    while (!blockingRequestDone.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<uint64_t> cancelCpuNs(0);
    std::thread cancelThread([&]() {
        auto startCpuNs = getCurrentThreadCpuTimeNs();
        hcm.cancelAllRequests();
        cancelCpuNs.store(getCurrentThreadCpuTimeNs() - startCpuNs, std::memory_order_release);
    });

    PAL::sleep(250);
    blockingRequestDone.release.store(true, std::memory_order_release);

    responseThread.join();
    cancelThread.join();

    EXPECT_LT(cancelCpuNs.load(std::memory_order_acquire), 50000000ULL);
    EXPECT_EQ(0U, hcm.requestCount());
}
