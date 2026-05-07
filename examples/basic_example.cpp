#include "hlog/async_logger.h"
#include "hlog/sinks/file_sink.h"
#include "runtime_paths.h"

#include <memory>
#include <thread>
#include <vector>

int main() {
  hlog::AsyncLoggerOptions options;
  options.queue_size = 1 << 12;
  options.overflow_policy = hlog::OverflowPolicy::Block;
  options.level = hlog::LogLevel::Debug;
  options.flush_level = hlog::LogLevel::Error;

  hlog::AsyncLogger logger(
      "example",
      std::make_unique<hlog::FileSink>(hlog_examples::RuntimeLogPath("example.log"), true),
      options);

  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([worker, &logger]() {
      for (int index = 0; index < 500; ++index) {
        HLOG_INFO(logger, "worker=", worker, " seq=", index, " message=async logging");
      }
    });
  }

  HLOG_WARN(logger, "logger started with queue_size=", options.queue_size);

  for (auto& worker : workers) {
    worker.join();
  }

  HLOG_ERROR(logger, "flush on error path");
  logger.Flush();
  logger.Stop();
  return 0;
}
