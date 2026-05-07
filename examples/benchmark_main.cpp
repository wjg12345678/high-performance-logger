#include "hlog/async_logger.h"
#include "hlog/sinks/file_sink.h"
#include "runtime_paths.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  const int producer_count = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
  const int messages_per_thread = argc > 2 ? std::max(1, std::atoi(argv[2])) : 20000;

  hlog::AsyncLoggerOptions options;
  options.queue_size = 1 << 15;
  options.overflow_policy = hlog::OverflowPolicy::Block;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Critical;

  hlog::AsyncLogger logger(
      "benchmark",
      std::make_unique<hlog::FileSink>(hlog_examples::RuntimeLogPath("benchmark.log"), true),
      options);

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> producers;
  producers.reserve(producer_count);

  for (int producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([producer, messages_per_thread, &logger]() {
      for (int index = 0; index < messages_per_thread; ++index) {
        logger.Info("producer=", producer, " seq=", index, " payload=benchmark");
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  logger.Flush();
  const auto end = std::chrono::steady_clock::now();
  logger.Stop();

  const auto stats = logger.Stats();
  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  const double throughput = seconds > 0.0 ? static_cast<double>(stats.written) / seconds : 0.0;

  std::cout << "threads=" << producer_count
            << " messages_per_thread=" << messages_per_thread
            << " written=" << stats.written
            << " dropped=" << stats.dropped
            << " elapsed_seconds=" << seconds
            << " throughput_msgs_per_sec=" << throughput
            << '\n';
  return 0;
}
