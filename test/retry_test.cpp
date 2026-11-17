// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/retry_accessor.hpp"
#include "gz_cesium/task_pool.hpp"
#include <gtest/gtest.h>

namespace {
class Response final : public CesiumAsync::IAssetResponse, public CesiumAsync::IAssetRequest {
 public:
  explicit Response(std::uint16_t code) : code_(code) {}
  std::uint16_t statusCode() const override { return code_; }
  std::string contentType() const override { return "model/gltf-binary"; }
  const CesiumAsync::HttpHeaders& headers() const override { return headers_; }
  std::span<const std::byte> data() const override { return {}; }
  const std::string& method() const override { return method_; }
  const std::string& url() const override { return url_; }
  const CesiumAsync::IAssetResponse* response() const override { return this; }
 private:
  std::uint16_t code_;
  CesiumAsync::HttpHeaders headers_;
  std::string method_{"GET"}, url_{"http://fixture/tile.glb"};
};
class Source final : public CesiumAsync::IAssetAccessor {
 public:
  std::vector<std::uint16_t> codes;
  unsigned calls{};
  CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
      const CesiumAsync::AsyncSystem& async, const std::string&, const std::vector<THeader>&) override {
    const auto code = codes.at(std::min<std::size_t>(calls++, codes.size()-1));
    if (code == 0) {
      auto promise = async.createPromise<std::shared_ptr<CesiumAsync::IAssetRequest>>();
      auto future = promise.getFuture();
      promise.reject(std::runtime_error("fixture connection interrupted"));
      return future;
    }
    return async.createResolvedFuture<std::shared_ptr<CesiumAsync::IAssetRequest>>(std::make_shared<Response>(code));
  }
  CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
      const CesiumAsync::AsyncSystem& async, const std::string&, const std::string& url,
      const std::vector<THeader>& headers, const std::span<const std::byte>&) override {
    return get(async, url, headers);
  }
  void tick() noexcept override {}
};
TEST(NetworkRecovery, TransientResponsesAndConnectionErrorsRecover) {
  CesiumAsync::AsyncSystem async(std::make_shared<gz_cesium::TaskPool>(1));
  auto source = std::make_shared<Source>();
  source->codes = {503, 0, 429, 200};
  auto retry = std::make_shared<gz_cesium::RetryAccessor>(source, 5, 0.);
  auto done = retry->get(async, "http://fixture/tile.glb");
  for (int i=0; i<20 && !done.isReady(); ++i) { async.dispatchMainThreadTasks(); retry->tick(); }
  ASSERT_TRUE(done.isReady());
  EXPECT_EQ(done.wait()->response()->statusCode(), 200);
  EXPECT_EQ(source->calls, 4U);
  EXPECT_EQ(retry->retries, 3U);
  EXPECT_EQ(retry->failures, 0U);
  EXPECT_EQ(retry->pending, 0U);
}
TEST(NetworkRecovery, PermanentResponsesAndMutationsAreNotRetried) {
  CesiumAsync::AsyncSystem async(std::make_shared<gz_cesium::TaskPool>(1));
  auto source = std::make_shared<Source>();
  source->codes = {404, 503};
  auto retry = std::make_shared<gz_cesium::RetryAccessor>(source, 5, 0.);
  auto missing = retry->get(async, "http://fixture/missing.glb");
  async.dispatchMainThreadTasks();
  EXPECT_EQ(missing.wait()->response()->statusCode(), 404);
  auto mutation = retry->request(async, "POST", "http://fixture", {}, {});
  EXPECT_EQ(mutation.wait()->response()->statusCode(), 503);
  EXPECT_EQ(source->calls, 2U);
  EXPECT_EQ(retry->retries, 0U);
}
TEST(NetworkRecovery, RetryBudgetAndShutdownAreBounded) {
  CesiumAsync::AsyncSystem async(std::make_shared<gz_cesium::TaskPool>(1));
  auto source = std::make_shared<Source>();
  source->codes = {503};
  auto retry = std::make_shared<gz_cesium::RetryAccessor>(source, 3, 0.);
  auto failed = retry->get(async, "http://fixture/tile.glb");
  for (int i=0; i<10 && !failed.isReady(); ++i) { async.dispatchMainThreadTasks(); retry->tick(); }
  ASSERT_TRUE(failed.isReady());
  EXPECT_EQ(failed.wait()->response()->statusCode(), 503);
  EXPECT_EQ(source->calls, 3U);
  auto stopping = std::make_shared<gz_cesium::RetryAccessor>(source, 8, 10.);
  auto cancelled = stopping->get(async, "http://fixture/tile.glb");
  async.dispatchMainThreadTasks();
  stopping->stop();
  EXPECT_THROW(cancelled.wait(), std::runtime_error);
  EXPECT_EQ(stopping->pending, 0U);
}
}
