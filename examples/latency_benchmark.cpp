#include "benchmark_support.h"
#include "hlog/async_logger.h"

#include <algorithm>
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

struct LatencyResult {
  std::string name;
  std::uint64_t call_count = 0;
  double elapsed_seconds = 0.0;
  double throughput = 0.0;
  double mean_ns = 0.0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p95_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t max_ns = 0;
};

struct LatencySummary {
  std::string name;
  int measured_rounds = 0;
  std::uint64_t call_count = 0;
  double median_elapsed_seconds = 0.0;
  double median_throughput = 0.0;
  double median_mean_ns = 0.0;
  std::uint64_t median_p50_ns = 0;
  std::uint64_t median_p95_ns = 0;
  std::uint64_t median_p99_ns = 0;
  std::uint64_t median_max_ns = 0;
};

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

std::uint64_t Median(std::vector<std::uint64_t> values) {
  if (values.empty()) {
    return 0;
  }

  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::uint64_t Percentile(
    const std::vector<std::uint64_t>& sorted_values,
    std::size_t numerator,
    std::size_t denominator) {
  if (sorted_values.empty()) {
    return 0;
  }

  const std::size_t index =
      ((sorted_values.size() - 1) * numerator + denominator - 1) / denominator;
  return sorted_values[index];
}

LatencyResult AnalyzeLatencies(
    std::string name,
    std::vector<std::uint64_t> latencies_ns,
    double elapsed_seconds) {
  std::sort(latencies_ns.begin(), latencies_ns.end());

  std::uint64_t total_ns = 0;
  for (const std::uint64_t latency : latencies_ns) {
    total_ns += latency;
  }

  const std::uint64_t call_count = latencies_ns.size();
  return LatencyResult{
      std::move(name),
      call_count,
      elapsed_seconds,
      elapsed_seconds > 0.0 ? static_cast<double>(call_count) / elapsed_seconds : 0.0,
      call_count > 0 ? static_cast<double>(total_ns) / static_cast<double>(call_count) : 0.0,
      Percentile(latencies_ns, 50, 100),
      Percentile(latencies_ns, 95, 100),
      Percentile(latencies_ns, 99, 100),
      latencies_ns.empty() ? 0 : latencies_ns.back(),
  };
}

template <typename LoggerFactory, typename LogFn, typename FlushFn>
LatencyResult RunLatencyBenchmark(
    std::string name,
    int thread_count,
    int messages_per_thread,
    LoggerFactory&& factory,
    LogFn&& log_fn,
    FlushFn&& flush_fn) {
  auto logger = factory();
  std::vector<std::vector<std::uint64_t>> thread_latencies(thread_count);
  for (auto& latencies : thread_latencies) {
    latencies.reserve(messages_per_thread);
  }

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    producers.emplace_back(
        [thread_id, messages_per_thread, &logger, &log_fn, &thread_latencies]() {
          auto& latencies = thread_latencies[thread_id];
          for (int index = 0; index < messages_per_thread; ++index) {
            const auto call_start = std::chrono::steady_clock::now();
            (void)log_fn(*logger, thread_id, index);
            const auto call_end = std::chrono::steady_clock::now();
            latencies.push_back(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(call_end - call_start)
                        .count()));
          }
        });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  flush_fn(*logger);
  const auto end = std::chrono::steady_clock::now();
  logger->Stop();

  std::vector<std::uint64_t> latencies_ns;
  latencies_ns.reserve(static_cast<std::size_t>(thread_count) * messages_per_thread);
  for (auto& latencies : thread_latencies) {
    latencies_ns.insert(latencies_ns.end(), latencies.begin(), latencies.end());
  }

  const double elapsed_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  return AnalyzeLatencies(std::move(name), std::move(latencies_ns), elapsed_seconds);
}

void PrintResult(const LatencyResult& result) {
  std::cout << result.name
            << " call_count=" << result.call_count
            << " elapsed_seconds=" << result.elapsed_seconds
            << " throughput_msgs_per_sec=" << result.throughput
            << " mean_call_ns=" << result.mean_ns
            << " p50_call_ns=" << result.p50_ns
            << " p95_call_ns=" << result.p95_ns
            << " p99_call_ns=" << result.p99_ns
            << " max_call_ns=" << result.max_ns
            << '\n';
}

void PrintRoundResult(std::string_view phase, int round, const LatencyResult& result) {
  std::cout << phase
            << " round=" << round
            << " ";
  PrintResult(result);
}

LatencySummary Summarize(const std::vector<LatencyResult>& results) {
  LatencySummary summary;
  if (results.empty()) {
    return summary;
  }

  summary.name = results.front().name;
  summary.measured_rounds = static_cast<int>(results.size());
  summary.call_count = results.front().call_count;

  std::vector<double> elapsed_seconds;
  std::vector<double> throughputs;
  std::vector<double> mean_latencies;
  std::vector<std::uint64_t> p50_latencies;
  std::vector<std::uint64_t> p95_latencies;
  std::vector<std::uint64_t> p99_latencies;
  std::vector<std::uint64_t> max_latencies;

  elapsed_seconds.reserve(results.size());
  throughputs.reserve(results.size());
  mean_latencies.reserve(results.size());
  p50_latencies.reserve(results.size());
  p95_latencies.reserve(results.size());
  p99_latencies.reserve(results.size());
  max_latencies.reserve(results.size());

  for (const auto& result : results) {
    elapsed_seconds.push_back(result.elapsed_seconds);
    throughputs.push_back(result.throughput);
    mean_latencies.push_back(result.mean_ns);
    p50_latencies.push_back(result.p50_ns);
    p95_latencies.push_back(result.p95_ns);
    p99_latencies.push_back(result.p99_ns);
    max_latencies.push_back(result.max_ns);
  }

  summary.median_elapsed_seconds = Median(elapsed_seconds);
  summary.median_throughput = Median(throughputs);
  summary.median_mean_ns = Median(mean_latencies);
  summary.median_p50_ns = Median(p50_latencies);
  summary.median_p95_ns = Median(p95_latencies);
  summary.median_p99_ns = Median(p99_latencies);
  summary.median_max_ns = Median(max_latencies);
  return summary;
}

void PrintSummary(const LatencySummary& summary) {
  std::cout << summary.name
            << "_summary measured_rounds=" << summary.measured_rounds
            << " call_count=" << summary.call_count
            << " median_elapsed_seconds=" << summary.median_elapsed_seconds
            << " median_throughput_msgs_per_sec=" << summary.median_throughput
            << " median_mean_call_ns=" << summary.median_mean_ns
            << " median_p50_call_ns=" << summary.median_p50_ns
            << " median_p95_call_ns=" << summary.median_p95_ns
            << " median_p99_call_ns=" << summary.median_p99_ns
            << " median_max_call_ns=" << summary.median_max_ns
            << '\n';
}

template <typename LoggerFactory, typename LogFn, typename FlushFn>
std::vector<LatencyResult> RunMeasuredRounds(
    int measured_rounds,
    std::string_view name,
    int thread_count,
    int messages_per_thread,
    LoggerFactory&& factory,
    LogFn&& log_fn,
    FlushFn&& flush_fn) {
  std::vector<LatencyResult> results;
  results.reserve(measured_rounds);
  for (int round = 1; round <= measured_rounds; ++round) {
    auto result = RunLatencyBenchmark(
        std::string(name),
        thread_count,
        messages_per_thread,
        factory,
        log_fn,
        flush_fn);
    PrintRoundResult("measured", round, result);
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
  const int messages_per_thread = argc > 2 ? std::max(1, std::atoi(argv[2])) : 20000;
  const int measured_rounds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 3;
  const int warmup_rounds = argc > 4 ? std::max(0, std::atoi(argv[4])) : 1;

  std::cout << "config threads=" << thread_count
            << " messages_per_thread=" << messages_per_thread
            << " measured_rounds=" << measured_rounds
            << " warmup_rounds=" << warmup_rounds
            << '\n';

  const auto mutex_factory = []() {
    return std::make_unique<hlog_bench::MutexAsyncLogger>(
        std::make_unique<hlog_bench::CountingSink>());
  };
  const auto mutex_log = [](hlog_bench::MutexAsyncLogger& logger, int thread_id, int index) {
    logger.Log(hlog_bench::MakePayload(thread_id, index));
    return true;
  };
  const auto mutex_flush = [](hlog_bench::MutexAsyncLogger& logger) {
    logger.Flush();
  };

  const auto cas_factory = []() {
    hlog::AsyncLoggerOptions options;
    options.queue_size = 1 << 15;
    options.overflow_policy = hlog::OverflowPolicy::Block;
    options.level = hlog::LogLevel::Info;
    options.flush_level = hlog::LogLevel::Off;
    return std::make_unique<hlog::AsyncLogger>(
        "cas-latency",
        std::make_unique<hlog_bench::CountingSink>(),
        options);
  };
  const auto cas_log = [](hlog::AsyncLogger& logger, int thread_id, int index) {
    return logger.Info(hlog_bench::MakePayload(thread_id, index));
  };
  const auto cas_flush = [](hlog::AsyncLogger& logger) {
    logger.Flush();
  };

  for (int round = 1; round <= warmup_rounds; ++round) {
    PrintRoundResult(
        "warmup",
        round,
        RunLatencyBenchmark(
            "mutex_async_logger",
            thread_count,
            messages_per_thread,
            mutex_factory,
            mutex_log,
            mutex_flush));
    PrintRoundResult(
        "warmup",
        round,
        RunLatencyBenchmark(
            "cas_async_logger",
            thread_count,
            messages_per_thread,
            cas_factory,
            cas_log,
            cas_flush));
  }

  const auto mutex_results = RunMeasuredRounds(
      measured_rounds,
      "mutex_async_logger",
      thread_count,
      messages_per_thread,
      mutex_factory,
      mutex_log,
      mutex_flush);
  const auto cas_results = RunMeasuredRounds(
      measured_rounds,
      "cas_async_logger",
      thread_count,
      messages_per_thread,
      cas_factory,
      cas_log,
      cas_flush);

  PrintSummary(Summarize(mutex_results));
  PrintSummary(Summarize(cas_results));
  return 0;
}
