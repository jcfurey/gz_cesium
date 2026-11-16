// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CesiumAsync/ITaskProcessor.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace gz_cesium {
class TaskPool final : public CesiumAsync::ITaskProcessor {
 public:
  explicit TaskPool(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      workers_.emplace_back([this] {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
          }
          task();
        }
      });
    }
  }
  ~TaskPool() override {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
  }
  void startTask(std::function<void()> task) override {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
  }
 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_{};
};
}  // namespace gz_cesium
