#include "benchmark_support.h"
#include "hlog/async_logger.h"
#include "hlog/file_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

BenchmarkResult RunBenchmark(
    std::string name,
    const hlog::FileSinkOptions& sink_options,
    int thread_count,
    int messages_per_thread) {
  const auto path =
      std::filesystem::temp_directory_path() / (name + "_benchmark.log");
  std::filesystem::remove(path);

  hlog::AsyncLoggerOptions options;
  options.queue_size = 1 << 15;
  options.overflow_policy = hlog::OverflowPolicy::Block;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  hlog::AsyncLogger logger(
      std::move(name),
      std::make_unique<hlog::FileSink>(path.string(), sink_options),
      options);

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    producers.emplace_back([thread_id, messages_per_thread, &logger]() {
      for (int index = 0; index < messages_per_thread; ++index) {
        logger.Info(
            "producer=",
            thread_id,
            " seq=",
            index,
            " payload=",
            hlog_bench::LongPayloadBlob());
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  logger.Flush();
  const auto end = std::chrono::steady_clock::now();
  const auto stats = logger.Stats();
  logger.Stop();
  std::filesystem::remove(path);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  return BenchmarkResult{
      logger.name(),
      stats.written,
      seconds,
      seconds > 0.0 ? static_cast<double>(stats.written) / seconds : 0.0,
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

std::vector<BenchmarkResult> RunMeasuredRounds(
    int measured_rounds,
    std::string_view name,
    const hlog::FileSinkOptions& sink_options,
    int thread_count,
    int messages_per_thread) {
  std::vector<BenchmarkResult> results;
  results.reserve(measured_rounds);
  for (int round = 1; round <= measured_rounds; ++round) {
    auto result = RunBenchmark(std::string(name), sink_options, thread_count, messages_per_thread);
    PrintRoundResult("measured", round, result);
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
  const int messages_per_thread = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5000;
  const int measured_rounds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 5;
  const int warmup_rounds = argc > 4 ? std::max(0, std::atoi(argv[4])) : 1;

  hlog::FileSinkOptions unbatched_options;
  unbatched_options.truncate = true;
  unbatched_options.max_batch_size = 1;
  unbatched_options.flush_interval = std::chrono::milliseconds{0};

  hlog::FileSinkOptions batched_options;
  batched_options.truncate = true;
  batched_options.max_batch_size = 64 * 1024;
  batched_options.flush_interval = std::chrono::milliseconds{250};

  std::cout << "config threads=" << thread_count
            << " messages_per_thread=" << messages_per_thread
            << " measured_rounds=" << measured_rounds
            << " warmup_rounds=" << warmup_rounds
            << " payload_bytes=" << hlog_bench::LongPayloadBlob().size()
            << " unbatched_max_batch_size=" << unbatched_options.max_batch_size
            << " batched_max_batch_size=" << batched_options.max_batch_size
            << " batched_flush_interval_ms=" << batched_options.flush_interval.count()
            << '\n';

  for (int round = 1; round <= warmup_rounds; ++round) {
    PrintRoundResult(
        "warmup",
        round,
        RunBenchmark(
            "unbatched_file_sink",
            unbatched_options,
            thread_count,
            messages_per_thread));
    PrintRoundResult(
        "warmup",
        round,
        RunBenchmark(
            "batched_file_sink",
            batched_options,
            thread_count,
            messages_per_thread));
  }

  const auto unbatched_results = RunMeasuredRounds(
      measured_rounds,
      "unbatched_file_sink",
      unbatched_options,
      thread_count,
      messages_per_thread);
  const auto batched_results = RunMeasuredRounds(
      measured_rounds,
      "batched_file_sink",
      batched_options,
      thread_count,
      messages_per_thread);

  const auto unbatched_summary = Summarize(unbatched_results);
  const auto batched_summary = Summarize(batched_results);
  PrintSummary(unbatched_summary);
  PrintSummary(batched_summary);

  if (unbatched_summary.median_throughput > 0.0) {
    const double improvement =
        (batched_summary.median_throughput - unbatched_summary.median_throughput) /
        unbatched_summary.median_throughput * 100.0;
    std::cout << "improvement_percent_median=" << improvement << '\n';
  }

  return 0;
}
