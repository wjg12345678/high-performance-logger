#include "benchmark_support.h"
#include "hlog/async_logger.h"

#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class CountingSpdlogSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  std::uint64_t written() const {
    return written_.load(std::memory_order_relaxed);
  }

protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    written_.fetch_add(1, std::memory_order_relaxed);
  }

  void flush_() override {}

private:
  std::atomic<std::uint64_t> written_{0};
};

class SpdlogAsyncLogger {
public:
  SpdlogAsyncLogger() {
    sink_ = std::make_shared<CountingSpdlogSink>();
    thread_pool_ = std::make_shared<spdlog::details::thread_pool>(1 << 15, 1);
    logger_ = std::make_shared<spdlog::async_logger>(
        "spdlog-benchmark",
        sink_,
        thread_pool_,
        spdlog::async_overflow_policy::block);
    logger_->set_level(spdlog::level::info);
    logger_->flush_on(spdlog::level::off);
  }

  void Log(const std::string& payload) {
    logger_->info(payload);
  }

  void Flush() {
    if (!stopped_) {
      logger_->flush();
      logger_.reset();
      thread_pool_.reset();
      stopped_ = true;
    }
  }

  std::uint64_t written() const {
    return sink_->written();
  }

  void Stop() {
    Flush();
  }

private:
  std::shared_ptr<CountingSpdlogSink> sink_;
  std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
  std::shared_ptr<spdlog::async_logger> logger_;
  bool stopped_ = false;
};

struct BenchmarkResult {
  std::string name;
  std::uint64_t written = 0;
  double seconds = 0.0;
  double throughput = 0.0;
};

struct BenchmarkSummary {
  std::string name;
  std::uint64_t written = 0;
  int measured_rounds = 0;
  double median_seconds = 0.0;
  double average_seconds = 0.0;
  double median_throughput = 0.0;
  double average_throughput = 0.0;
  double min_throughput = 0.0;
  double max_throughput = 0.0;
};

double Average(const std::vector<double>& values) {
  if (values.empty()) {
    return 0.0;
  }

  double sum = 0.0;
  for (const double value : values) {
    sum += value;
  }
  return sum / static_cast<double>(values.size());
}

double Median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 == 0) {
    return (values[middle - 1] + values[middle]) * 0.5;
  }
  return values[middle];
}

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

void PrintRoundResult(std::string_view phase, int round, const BenchmarkResult& result) {
  std::cout << phase
            << " round=" << round
            << " ";
  PrintResult(result);
}

BenchmarkSummary Summarize(const std::vector<BenchmarkResult>& results) {
  BenchmarkSummary summary;
  if (results.empty()) {
    return summary;
  }

  summary.name = results.front().name;
  summary.written = results.front().written;
  summary.measured_rounds = static_cast<int>(results.size());

  std::vector<double> seconds;
  std::vector<double> throughputs;
  seconds.reserve(results.size());
  throughputs.reserve(results.size());
  for (const auto& result : results) {
    seconds.push_back(result.seconds);
    throughputs.push_back(result.throughput);
  }

  summary.median_seconds = Median(seconds);
  summary.average_seconds = Average(seconds);
  summary.median_throughput = Median(throughputs);
  summary.average_throughput = Average(throughputs);
  summary.min_throughput = *std::min_element(throughputs.begin(), throughputs.end());
  summary.max_throughput = *std::max_element(throughputs.begin(), throughputs.end());
  return summary;
}

void PrintSummary(const BenchmarkSummary& summary) {
  std::cout << summary.name
            << "_summary written=" << summary.written
            << " measured_rounds=" << summary.measured_rounds
            << " median_elapsed_seconds=" << summary.median_seconds
            << " average_elapsed_seconds=" << summary.average_seconds
            << " median_throughput_msgs_per_sec=" << summary.median_throughput
            << " average_throughput_msgs_per_sec=" << summary.average_throughput
            << " min_throughput_msgs_per_sec=" << summary.min_throughput
            << " max_throughput_msgs_per_sec=" << summary.max_throughput
            << '\n';
}

template <typename LoggerFactory, typename LogFn, typename FlushFn, typename WrittenFn>
std::vector<BenchmarkResult> RunBenchmarks(
    int measured_rounds,
    std::string_view name,
    int thread_count,
    int messages_per_thread,
    LoggerFactory&& factory,
    LogFn&& log_fn,
    FlushFn&& flush_fn,
    WrittenFn&& written_fn) {
  std::vector<BenchmarkResult> results;
  results.reserve(measured_rounds);
  for (int round = 1; round <= measured_rounds; ++round) {
    auto result = RunBenchmark(
        std::string(name),
        thread_count,
        messages_per_thread,
        factory,
        log_fn,
        flush_fn,
        written_fn);
    PrintRoundResult("measured", round, result);
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
  const int messages_per_thread = argc > 2 ? std::max(1, std::atoi(argv[2])) : 50000;
  const int measured_rounds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 5;
  const int warmup_rounds = argc > 4 ? std::max(0, std::atoi(argv[4])) : 1;

  std::cout << "config threads=" << thread_count
            << " messages_per_thread=" << messages_per_thread
            << " measured_rounds=" << measured_rounds
            << " warmup_rounds=" << warmup_rounds
            << '\n';

  const auto spdlog_factory = []() {
    return std::make_unique<SpdlogAsyncLogger>();
  };
  const auto spdlog_log = [](SpdlogAsyncLogger& logger, int thread_id, int index) {
    logger.Log(hlog_bench::MakePayload(thread_id, index));
  };
  const auto spdlog_flush = [](SpdlogAsyncLogger& logger) {
    logger.Flush();
  };
  const auto spdlog_written = [](SpdlogAsyncLogger& logger) {
    return logger.written();
  };

  const auto cas_factory = []() {
    hlog::AsyncLoggerOptions options;
    options.queue_size = 1 << 15;
    options.overflow_policy = hlog::OverflowPolicy::Block;
    options.level = hlog::LogLevel::Info;
    options.flush_level = hlog::LogLevel::Off;
    return std::make_unique<hlog::AsyncLogger>(
        "cas-benchmark",
        std::make_unique<hlog_bench::CountingSink>(),
        options);
  };
  const auto cas_log = [](hlog::AsyncLogger& logger, int thread_id, int index) {
    logger.Info(hlog_bench::MakePayload(thread_id, index));
  };
  const auto cas_flush = [](hlog::AsyncLogger& logger) {
    logger.Flush();
  };
  const auto cas_written = [](hlog::AsyncLogger& logger) {
    return logger.Stats().written;
  };

  for (int round = 1; round <= warmup_rounds; ++round) {
    PrintRoundResult(
        "warmup",
        round,
        RunBenchmark(
            "spdlog_async_logger",
            thread_count,
            messages_per_thread,
            spdlog_factory,
            spdlog_log,
            spdlog_flush,
            spdlog_written));
    PrintRoundResult(
        "warmup",
        round,
        RunBenchmark(
            "cas_async_logger",
            thread_count,
            messages_per_thread,
            cas_factory,
            cas_log,
            cas_flush,
            cas_written));
  }

  const auto spdlog_results = RunBenchmarks(
      measured_rounds,
      "spdlog_async_logger",
      thread_count,
      messages_per_thread,
      spdlog_factory,
      spdlog_log,
      spdlog_flush,
      spdlog_written);

  const auto cas_results = RunBenchmarks(
      measured_rounds,
      "cas_async_logger",
      thread_count,
      messages_per_thread,
      cas_factory,
      cas_log,
      cas_flush,
      cas_written);

  const auto spdlog_summary = Summarize(spdlog_results);
  const auto cas_summary = Summarize(cas_results);
  PrintSummary(spdlog_summary);
  PrintSummary(cas_summary);

  if (spdlog_summary.median_throughput > 0.0) {
    const double improvement =
        (cas_summary.median_throughput - spdlog_summary.median_throughput) /
        spdlog_summary.median_throughput * 100.0;
    std::cout << "improvement_percent_median=" << improvement << '\n';
  }

  return 0;
}
