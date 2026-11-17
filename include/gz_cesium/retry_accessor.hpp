// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetResponse.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace gz_cesium {
// Native's JSON loader treats HTTP tile failures as permanent. Keep a transient
// GET in flight across bounded, tick-driven retries so it can recover without
// rebuilding the tileset or disturbing retained contact terrain.
class RetryAccessor final : public CesiumAsync::IAssetAccessor,
    public std::enable_shared_from_this<RetryAccessor> {
 public:
  using Request = std::shared_ptr<CesiumAsync::IAssetRequest>;
  using Clock = std::chrono::steady_clock;
  explicit RetryAccessor(std::shared_ptr<CesiumAsync::IAssetAccessor> inner,
      unsigned attempts=8, double delay=.5)
      : inner_(std::move(inner)), attempts_(attempts), delay_(delay) {}
  std::atomic<std::uint64_t> requests{0}, retries{0}, failures{0}, pending{0};

  CesiumAsync::Future<Request> get(const CesiumAsync::AsyncSystem& async,
      const std::string& url, const std::vector<THeader>& headers={}) override {
    auto job = std::make_shared<Job>(async, url, headers);
    auto future = job->promise.getFuture();
    ++pending;
    start(job);
    return future;
  }
  CesiumAsync::Future<Request> request(const CesiumAsync::AsyncSystem& async,
      const std::string& verb, const std::string& url,
      const std::vector<THeader>& headers={},
      const std::span<const std::byte>& payload={}) override {
    // Only GETs are idempotently retried. Never replay mutations.
    if (verb == "GET" && payload.empty()) return get(async, url, headers);
    return inner_->request(async, verb, url, headers, payload);
  }
  void tick() noexcept override {
    inner_->tick();
    std::vector<std::shared_ptr<Job>> ready;
    {
      std::lock_guard guard(mutex_);
      std::erase_if(queue_, [&](const auto& job) {
        if (job->due > Clock::now()) return false;
        ready.push_back(job);
        return true;
      });
    }
    for (const auto& job : ready) start(job);
  }
  void stop() {
    stopping_ = true;
    std::vector<std::shared_ptr<Job>> cancelled;
    { std::lock_guard guard(mutex_); cancelled.swap(queue_); }
    for (const auto& job : cancelled) reject(job, std::make_exception_ptr(
        std::runtime_error("Cesium asset accessor stopped")));
  }

 private:
  struct Job {
    CesiumAsync::AsyncSystem async;
    std::string url;
    std::vector<THeader> headers;
    CesiumAsync::Promise<Request> promise;
    unsigned attempt{};
    Clock::time_point due;
    Job(const CesiumAsync::AsyncSystem& system, const std::string& address,
        const std::vector<THeader>& fields)
        : async(system), url(address), headers(fields),
          promise(system.createPromise<Request>()) {}
  };
  bool schedule(const std::shared_ptr<Job>& job) {
    std::lock_guard guard(mutex_);
    if (stopping_ || job->attempt >= attempts_) return false;
    const double seconds = std::min(8., delay_ * (1U << std::min(16U, job->attempt-1)));
    job->due = Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    queue_.push_back(job);
    ++retries;
    return true;
  }
  void reject(const std::shared_ptr<Job>& job, const std::exception_ptr& error) {
    ++failures;
    --pending;
    job->promise.reject(error);
  }
  void start(const std::shared_ptr<Job>& job) {
    if (stopping_) {
      reject(job, std::make_exception_ptr(std::runtime_error("Cesium asset accessor stopped")));
      return;
    }
    ++job->attempt;
    ++requests;
    auto self = shared_from_this();
    try {
      inner_->get(job->async, job->url, job->headers)
          .thenInMainThread([self, job](Request&& response) {
            const auto* data = response ? response->response() : nullptr;
            const auto code = data ? data->statusCode() : 0;
            const bool transient = !data || code == 408 || code == 429 || (code >= 500 && code <= 599);
            if (transient && self->schedule(job)) return;
            if (!data || (code && (code < 200 || code >= 300))) ++self->failures;
            --self->pending;
            job->promise.resolve(std::move(response));
          }).catchInMainThread([self, job](const std::exception& error) {
            if (!self->schedule(job)) self->reject(job, std::make_exception_ptr(std::runtime_error(error.what())));
          });
    } catch (...) {
      if (!schedule(job)) reject(job, std::current_exception());
    }
  }
  std::shared_ptr<CesiumAsync::IAssetAccessor> inner_;
  unsigned attempts_;
  double delay_;
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::vector<std::shared_ptr<Job>> queue_;
};
}  // namespace gz_cesium
