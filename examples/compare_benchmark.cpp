#include "hlog/async_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class CountingSink final : public hlog::Sink {
public:
  void Write(const hlog::LogMessage&) override {
    ++written_;
  }

  void Flush() override {}

  std::uint64_t written() const {
    return written_;
  }

private:
  std::uint64_t written_ = 0;
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

struct BenchmarkResult {
  std::string name;
  std::uint64_t written = 0;
  double seconds = 0.0;
  double throughput = 0.0;
};

template <typename LoggerFactory, typename LogFn, typename FlushFn, typename WrittenFn>
BenchmarkResult RunBenchmark(
    std::string name,
    int thread_count,
    int messages_per_thread,
    LoggerFactory&& factory,
    LogFn&& log_fn,
    FlushFn&& flush_fn,
    WrittenFn&& written_fn) {
  auto logger = factory();
  const auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    producers.emplace_back([thread_id, messages_per_thread, &logger, &log_fn]() {
      for (int index = 0; index < messages_per_thread; ++index) {
        log_fn(*logger, thread_id, index);
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  flush_fn(*logger);
  const auto end = std::chrono::steady_clock::now();

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  const std::uint64_t written = written_fn(*logger);
  logger->Stop();

  return BenchmarkResult{
      std::move(name),
      written,
      seconds,
      seconds > 0.0 ? static_cast<double>(written) / seconds : 0.0,
  };
}

void PrintResult(const BenchmarkResult& result) {
  std::cout << result.name
            << " written=" << result.written
            << " elapsed_seconds=" << result.seconds
            << " throughput_msgs_per_sec=" << result.throughput
            << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
  const int messages_per_thread = argc > 2 ? std::max(1, std::atoi(argv[2])) : 50000;

  const auto mutex_result = RunBenchmark(
      "mutex_async_logger",
      thread_count,
      messages_per_thread,
      []() {
        return std::make_unique<MutexAsyncLogger>(std::make_unique<CountingSink>());
      },
      [](MutexAsyncLogger& logger, int thread_id, int index) {
        const std::string payload =
            "thread=" + std::to_string(thread_id) + " seq=" + std::to_string(index);
        logger.Log(payload);
      },
      [](MutexAsyncLogger& logger) {
        logger.Flush();
      },
      [](MutexAsyncLogger& logger) {
        return logger.written();
      });

  const auto cas_result = RunBenchmark(
      "cas_async_logger",
      thread_count,
      messages_per_thread,
      []() {
        hlog::AsyncLoggerOptions options;
        options.queue_size = 1 << 15;
        options.overflow_policy = hlog::OverflowPolicy::Block;
        options.level = hlog::LogLevel::Info;
        options.flush_level = hlog::LogLevel::Off;
        return std::make_unique<hlog::AsyncLogger>(
            "cas-benchmark",
            std::make_unique<CountingSink>(),
            options);
      },
      [](hlog::AsyncLogger& logger, int thread_id, int index) {
        const std::string payload =
            "thread=" + std::to_string(thread_id) + " seq=" + std::to_string(index);
        logger.Info(payload);
      },
      [](hlog::AsyncLogger& logger) {
        logger.Flush();
      },
      [](hlog::AsyncLogger& logger) {
        return logger.Stats().written;
      });

  PrintResult(mutex_result);
  PrintResult(cas_result);

  if (mutex_result.throughput > 0.0) {
    const double improvement =
        (cas_result.throughput - mutex_result.throughput) / mutex_result.throughput * 100.0;
    std::cout << "improvement_percent=" << improvement << '\n';
  }

  return 0;
}
