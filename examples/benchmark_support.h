#pragma once

#include "hlog/async_logger.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace hlog_bench {

class CountingSink final : public hlog::Sink {
public:
  void Write(const hlog::LogMessage&) override {
    written_.fetch_add(1, std::memory_order_relaxed);
  }

  void Flush() override {}

  std::uint64_t written() const {
    return written_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> written_{0};
};

class MutexAsyncLogger {
public:
  explicit MutexAsyncLogger(std::unique_ptr<hlog::Sink> sink) : sink_(std::move(sink)) {
    worker_ = std::thread(&MutexAsyncLogger::WorkerLoop, this);
  }

  ~MutexAsyncLogger() {
    Stop();
  }

  MutexAsyncLogger(const MutexAsyncLogger&) = delete;
  MutexAsyncLogger& operator=(const MutexAsyncLogger&) = delete;

  void Log(std::string payload) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(payload));
    }
    condition_.notify_one();
  }

  void Flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++flush_request_ticket_;
    condition_.notify_one();
    flush_done_.wait(lock, [this]() {
      return flush_complete_ticket_ >= flush_request_ticket_;
    });
  }

  std::uint64_t written() const {
    return written_.load(std::memory_order_relaxed);
  }

  void Stop() {
    bool expected = false;
    if (stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
      }
      condition_.notify_all();
    }

    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  void WorkerLoop() {
    while (true) {
      std::deque<std::string> batch;
      std::uint64_t flush_ticket = 0;

      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
          return stopping_.load(std::memory_order_acquire) ||
              flush_complete_ticket_ < flush_request_ticket_ ||
              !queue_.empty();
        });

        if (queue_.empty() && stopping_.load(std::memory_order_acquire)) {
          break;
        }

        batch.swap(queue_);
        if (flush_complete_ticket_ < flush_request_ticket_) {
          flush_ticket = flush_request_ticket_;
        }
      }

      for (const auto& payload : batch) {
        hlog::LogMessage message;
        message.payload = payload;
        sink_->Write(message);
        written_.fetch_add(1, std::memory_order_relaxed);
      }

      if (flush_ticket != 0) {
        sink_->Flush();
        std::lock_guard<std::mutex> lock(mutex_);
        flush_complete_ticket_ = flush_ticket;
        flush_done_.notify_all();
      }
    }

    std::deque<std::string> tail;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tail.swap(queue_);
    }

    for (const auto& payload : tail) {
      hlog::LogMessage message;
      message.payload = payload;
      sink_->Write(message);
      written_.fetch_add(1, std::memory_order_relaxed);
    }
    sink_->Flush();
  }

  std::unique_ptr<hlog::Sink> sink_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable flush_done_;
  std::deque<std::string> queue_;
  std::atomic<bool> stopping_{false};
  std::uint64_t flush_request_ticket_ = 0;
  std::uint64_t flush_complete_ticket_ = 0;
  std::atomic<std::uint64_t> written_{0};
};

inline std::string MakePayload(int thread_id, int index) {
  return "thread=" + std::to_string(thread_id) + " seq=" + std::to_string(index);
}

inline const std::string& LongPayloadBlob() {
  static const std::string payload(512, 'x');
  return payload;
}

inline std::string MakeLargePayload(int thread_id, int index) {
  return MakePayload(thread_id, index) + " payload=" + LongPayloadBlob();
}

inline std::string MakeStreamPayload(int thread_id, int index) {
  std::ostringstream stream;
  stream << "thread=" << thread_id << " seq=" << index;
  return stream.str();
}

inline std::string MakeStreamLargePayload(int thread_id, int index) {
  std::ostringstream stream;
  stream << "thread=" << thread_id << " seq=" << index << " payload=" << LongPayloadBlob();
  return stream.str();
}

}  // namespace hlog_bench
