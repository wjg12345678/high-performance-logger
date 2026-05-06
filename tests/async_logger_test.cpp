#include "hlog/async_logger.h"
#include "hlog/file_sink.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class SlowSink final : public hlog::Sink {
public:
  explicit SlowSink(const std::string& path) : output_(path, std::ios::out | std::ios::trunc) {
    assert(output_.is_open());
  }

  void Write(const hlog::LogMessage& message) override {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    output_ << message.payload << '\n';
  }

  void Flush() override {
    output_.flush();
  }

private:
  std::ofstream output_;
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);) {
    lines.push_back(std::move(line));
  }
  return lines;
}

void TestLevelFiltering() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_level_filter.log";
  std::filesystem::remove(path);

  hlog::AsyncLoggerOptions options;
  options.queue_size = 64;
  options.level = hlog::LogLevel::Warn;
  options.flush_level = hlog::LogLevel::Critical;

  hlog::AsyncLogger logger(
      "level-test",
      std::make_unique<hlog::FileSink>(path.string(), true),
      options);

  HLOG_INFO(logger, "info should be filtered");
  HLOG_WARN(logger, "warn should stay");
  HLOG_ERROR(logger, "error should stay");

  logger.Flush();
  logger.Stop();

  const auto lines = ReadLines(path);
  const auto stats = logger.Stats();
  assert(lines.size() == 2);
  assert(lines[0].find("[WARN]") != std::string::npos);
  assert(lines[1].find("[ERROR]") != std::string::npos);
  assert(stats.enqueued == 2);
  assert(stats.written == 2);
  assert(stats.dropped == 0);
}

void TestConcurrentAsyncWrite() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_concurrent.log";
  std::filesystem::remove(path);

  constexpr int kThreadCount = 6;
  constexpr int kMessagesPerThread = 1500;
  constexpr std::uint64_t kExpected = static_cast<std::uint64_t>(kThreadCount) * kMessagesPerThread;

  hlog::AsyncLoggerOptions options;
  options.queue_size = 1 << 12;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Critical;

  hlog::AsyncLogger logger(
      "concurrency-test",
      std::make_unique<hlog::FileSink>(path.string(), true),
      options);

  std::vector<std::thread> producers;
  producers.reserve(kThreadCount);
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    producers.emplace_back([thread_id, &logger]() {
      for (int index = 0; index < kMessagesPerThread; ++index) {
        HLOG_INFO(logger, "thread=", thread_id, " seq=", index);
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  logger.Flush();
  logger.Stop();

  const auto lines = ReadLines(path);
  const auto stats = logger.Stats();
  assert(lines.size() == kExpected);
  assert(stats.enqueued == kExpected);
  assert(stats.written == kExpected);
  assert(stats.dropped == 0);
}

void TestDropPolicy() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_drop_policy.log";
  std::filesystem::remove(path);

  constexpr int kThreadCount = 4;
  constexpr int kMessagesPerThread = 400;
  constexpr std::uint64_t kAttempted = static_cast<std::uint64_t>(kThreadCount) * kMessagesPerThread;

  hlog::AsyncLoggerOptions options;
  options.queue_size = 8;
  options.overflow_policy = hlog::OverflowPolicy::DropNewest;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Critical;

  hlog::AsyncLogger logger(
      "drop-test",
      std::make_unique<SlowSink>(path.string()),
      options);

  std::vector<std::thread> producers;
  producers.reserve(kThreadCount);
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    producers.emplace_back([thread_id, &logger]() {
      for (int index = 0; index < kMessagesPerThread; ++index) {
        logger.Info("thread=", thread_id, " seq=", index);
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }

  logger.Flush();
  logger.Stop();

  const auto lines = ReadLines(path);
  const auto stats = logger.Stats();
  assert(stats.dropped > 0);
  assert(stats.enqueued + stats.dropped == kAttempted);
  assert(stats.written == lines.size());
}

}  // namespace

int main() {
  TestLevelFiltering();
  TestConcurrentAsyncWrite();
  TestDropPolicy();
  return 0;
}
