#include "hlog/async_logger.h"
#include "hlog/console_sink.h"
#include "hlog/file_sink.h"
#include "hlog/logger_config.h"
#include "hlog/multi_sink.h"
#include "hlog/pattern_formatter.h"
#include "hlog/rotating_file_sink.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T, typename U>
void ExpectEqual(const T& actual, const U& expected, std::string_view label) {
  if (!(actual == expected)) {
    std::ostringstream stream;
    stream << label << " expected=" << expected << " actual=" << actual;
    throw std::runtime_error(stream.str());
  }
}

class SlowSink final : public hlog::Sink {
public:
  explicit SlowSink(const std::string& path) : output_(path, std::ios::out | std::ios::trunc) {
    if (!output_.is_open()) {
      throw std::runtime_error("failed to open sink output: " + path);
    }
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

class CountingSink final : public hlog::Sink {
public:
  explicit CountingSink(std::chrono::microseconds write_delay = std::chrono::microseconds{0})
      : write_delay_(write_delay) {}

  void Write(const hlog::LogMessage&) override {
    if (write_delay_.count() > 0) {
      std::this_thread::sleep_for(write_delay_);
    }
    written_.fetch_add(1, std::memory_order_relaxed);
  }

  void Flush() override {
    flushed_.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t written() const {
    return written_.load(std::memory_order_relaxed);
  }

  std::uint64_t flushed() const {
    return flushed_.load(std::memory_order_relaxed);
  }

private:
  std::chrono::microseconds write_delay_;
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> flushed_{0};
};

class InspectingSink final : public hlog::Sink {
public:
  void Write(const hlog::LogMessage& message) override {
    payloads_.push_back(message.payload.str());
    inline_flags_.push_back(message.payload.is_inline());
  }

  void Flush() override {}

  const std::vector<std::string>& payloads() const {
    return payloads_;
  }

  const std::vector<bool>& inline_flags() const {
    return inline_flags_;
  }

private:
  std::vector<std::string> payloads_;
  std::vector<bool> inline_flags_;
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

  hlog::LoggerStats stats;

  {
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
    stats = logger.Stats();
  }

  const auto lines = ReadLines(path);
  ExpectEqual(lines.size(), std::size_t{2}, "level filter line count");
  Expect(lines[0].find("[WARN]") != std::string::npos, "warn log missing");
  Expect(lines[1].find("[ERROR]") != std::string::npos, "error log missing");
  ExpectEqual(stats.enqueued, std::uint64_t{2}, "level filter enqueued");
  ExpectEqual(stats.written, std::uint64_t{2}, "level filter written");
  ExpectEqual(stats.dropped, std::uint64_t{0}, "level filter dropped");
  ExpectEqual(stats.pending, std::uint64_t{0}, "level filter pending");
}

void TestConcurrentAsyncWrite() {
  constexpr int kThreadCount = 6;
  constexpr int kMessagesPerThread = 1500;
  constexpr std::uint64_t kExpected = static_cast<std::uint64_t>(kThreadCount) * kMessagesPerThread;

  auto sink = std::make_unique<CountingSink>();
  auto* sink_ptr = sink.get();

  hlog::AsyncLoggerOptions options;
  options.queue_size = 1 << 12;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  hlog::AsyncLogger logger("concurrency-test", std::move(sink), options);

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

  const auto stats = logger.Stats();
  ExpectEqual(stats.enqueued, kExpected, "concurrent enqueued");
  ExpectEqual(stats.written, kExpected, "concurrent written");
  ExpectEqual(stats.dropped, std::uint64_t{0}, "concurrent dropped");
  ExpectEqual(stats.pending, std::uint64_t{0}, "concurrent pending");
  ExpectEqual(sink_ptr->written(), kExpected, "concurrent sink written");
}

void TestDropPolicy() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_drop_policy.log";
  std::filesystem::remove(path);

  constexpr int kThreadCount = 4;
  constexpr int kMessagesPerThread = 400;
  constexpr std::uint64_t kAttempted = static_cast<std::uint64_t>(kThreadCount) * kMessagesPerThread;

  hlog::LoggerStats stats;

  {
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
    stats = logger.Stats();
  }

  const auto lines = ReadLines(path);
  Expect(stats.dropped > 0, "drop policy should drop messages");
  ExpectEqual(stats.enqueued + stats.dropped, kAttempted, "drop policy accounted messages");
  ExpectEqual(stats.written, static_cast<std::uint64_t>(lines.size()), "drop policy written");
  ExpectEqual(stats.pending, std::uint64_t{0}, "drop policy pending");
}

void TestFlushWaitsForOutstandingWrites() {
  constexpr std::uint64_t kMessageCount = 64;

  hlog::AsyncLoggerOptions options;
  options.queue_size = 8;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  auto sink = std::make_unique<CountingSink>(std::chrono::microseconds(150));
  auto* sink_ptr = sink.get();
  hlog::AsyncLogger logger("flush-test", std::move(sink), options);

  for (std::uint64_t index = 0; index < kMessageCount; ++index) {
    Expect(logger.Info("msg=", index), "flush test log should succeed");
  }

  Expect(logger.Flush(), "flush should succeed while worker is running");

  const auto stats = logger.Stats();
  ExpectEqual(sink_ptr->written(), kMessageCount, "flush sink written");
  Expect(sink_ptr->flushed() >= 1, "flush should reach sink");
  ExpectEqual(stats.written, kMessageCount, "flush stats written");
  ExpectEqual(stats.pending, std::uint64_t{0}, "flush stats pending");

  logger.Stop();
}

void TestStopRejectsNewWrites() {
  auto sink = std::make_unique<CountingSink>();
  auto* sink_ptr = sink.get();

  hlog::AsyncLogger logger(
      "stop-test",
      std::move(sink),
      hlog::AsyncLoggerOptions{});

  Expect(logger.Info("before stop"), "pre-stop log should succeed");
  Expect(logger.Flush(), "pre-stop flush should succeed");
  logger.Stop();

  const auto before = logger.Stats();
  Expect(!logger.Info("after stop"), "post-stop log should be rejected");
  Expect(!logger.Flush(), "post-stop flush should fail");

  const auto after = logger.Stats();
  ExpectEqual(after.enqueued, before.enqueued, "post-stop enqueued");
  ExpectEqual(after.written, before.written, "post-stop written");
  ExpectEqual(after.pending, std::uint64_t{0}, "post-stop pending");
  ExpectEqual(sink_ptr->written(), before.written, "post-stop sink written");
}

void TestStopDrainsQueuedWrites() {
  constexpr int kThreadCount = 8;
  constexpr int kMessagesPerThread = 3000;
  constexpr std::uint64_t kExpected =
      static_cast<std::uint64_t>(kThreadCount) * kMessagesPerThread;

  hlog::AsyncLoggerOptions options;
  options.queue_size = 32;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  auto sink = std::make_unique<CountingSink>(std::chrono::microseconds(20));
  auto* sink_ptr = sink.get();
  hlog::AsyncLogger logger("pending-test", std::move(sink), options);

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

  logger.Stop();

  const auto stats = logger.Stats();
  ExpectEqual(stats.enqueued, kExpected, "stop drain enqueued");
  ExpectEqual(stats.written, kExpected, "stop drain written");
  ExpectEqual(stats.pending, std::uint64_t{0}, "stop drain pending");
  ExpectEqual(sink_ptr->written(), kExpected, "stop drain sink written");
}

void TestBatchedFileSinkFlushesTailOnStop() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_batched_tail_flush.log";
  std::filesystem::remove(path);

  hlog::LoggerStats stats;
  {
    hlog::AsyncLoggerOptions options;
    options.queue_size = 64;
    options.level = hlog::LogLevel::Info;
    options.flush_level = hlog::LogLevel::Off;

    hlog::FileSinkOptions sink_options;
    sink_options.truncate = true;
    sink_options.max_batch_size = 1 << 20;
    sink_options.flush_interval = std::chrono::hours(1);

    hlog::AsyncLogger logger(
        "batched-file-stop-test",
        std::make_unique<hlog::FileSink>(path.string(), sink_options),
        options);

    for (int index = 0; index < 12; ++index) {
      Expect(logger.Info("batched tail msg=", index), "batched stop log should succeed");
    }

    logger.Stop();
    stats = logger.Stats();
  }

  const auto lines = ReadLines(path);
  ExpectEqual(lines.size(), std::size_t{12}, "batched stop line count");
  Expect(lines.front().find("batched tail msg=0") != std::string::npos, "batched stop first line");
  Expect(lines.back().find("batched tail msg=11") != std::string::npos, "batched stop last line");
  ExpectEqual(stats.enqueued, std::uint64_t{12}, "batched stop enqueued");
  ExpectEqual(stats.written, std::uint64_t{12}, "batched stop written");
  ExpectEqual(stats.pending, std::uint64_t{0}, "batched stop pending");
}

void TestShortPayloadUsesInlineStorage() {
  auto sink = std::make_unique<InspectingSink>();
  auto* sink_ptr = sink.get();

  hlog::AsyncLoggerOptions options;
  options.queue_size = 32;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  hlog::AsyncLogger logger("inline-payload-test", std::move(sink), options);
  Expect(logger.Info("thread=", 7, " seq=", 42), "short payload log should succeed");
  Expect(logger.Flush(), "short payload flush should succeed");
  logger.Stop();

  ExpectEqual(sink_ptr->payloads().size(), std::size_t{1}, "short payload sink count");
  ExpectEqual(sink_ptr->payloads().front(), std::string("thread=7 seq=42"), "short payload text");
  ExpectEqual(sink_ptr->inline_flags().size(), std::size_t{1}, "short payload inline flag count");
  Expect(sink_ptr->inline_flags().front(), "short payload should stay inline");
}

void TestLargePayloadSpillsToHeap() {
  auto sink = std::make_unique<InspectingSink>();
  auto* sink_ptr = sink.get();

  hlog::AsyncLoggerOptions options;
  options.queue_size = 32;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  const std::string large_payload(hlog::LogPayload::kInlineCapacity + 64, 'x');

  hlog::AsyncLogger logger("heap-payload-test", std::move(sink), options);
  Expect(logger.Info("prefix=", large_payload), "large payload log should succeed");
  Expect(logger.Flush(), "large payload flush should succeed");
  logger.Stop();

  ExpectEqual(sink_ptr->payloads().size(), std::size_t{1}, "large payload sink count");
  ExpectEqual(
      sink_ptr->payloads().front(),
      std::string("prefix=") + large_payload,
      "large payload text");
  ExpectEqual(sink_ptr->inline_flags().size(), std::size_t{1}, "large payload inline flag count");
  Expect(!sink_ptr->inline_flags().front(), "large payload should spill to heap");
}

void TestPatternFormatterExpandsTokens() {
  hlog::PatternFormatter formatter("%n|%l|%t|%v|%s|%#|%!");

  hlog::LogMessage message;
  message.level = hlog::LogLevel::Warn;
  message.logger_name = "pattern-test";
  message.thread_id = 42;
  message.payload = "payload";
  message.source = {"dir/file.cpp", 17, "Function"};

  const std::string formatted = formatter.Format(message);
  ExpectEqual(
      formatted,
      std::string("pattern-test|WARN|42|payload|file.cpp|17|Function"),
      "pattern formatter output");
}

void TestParseLogLevelAcceptsCommonForms() {
  hlog::LogLevel level = hlog::LogLevel::Off;
  Expect(hlog::TryParseLogLevel("debug", level), "parse debug");
  ExpectEqual(level, hlog::LogLevel::Debug, "debug level");
  Expect(hlog::TryParseLogLevel("WARNING", level), "parse warning");
  ExpectEqual(level, hlog::LogLevel::Warn, "warning level");
  ExpectEqual(
      hlog::ParseLogLevel("not-a-level", hlog::LogLevel::Error),
      hlog::LogLevel::Error,
      "fallback parse level");
}

void TestMultiSinkFansOutToChildren() {
  auto inspecting_sink = std::make_unique<InspectingSink>();
  auto* inspecting_sink_ptr = inspecting_sink.get();
  auto counting_sink = std::make_unique<CountingSink>();
  auto* counting_sink_ptr = counting_sink.get();

  auto multi_sink = std::make_unique<hlog::MultiSink>();
  multi_sink->AddSink(std::move(inspecting_sink));
  multi_sink->AddSink(std::move(counting_sink));

  hlog::AsyncLoggerOptions options;
  options.queue_size = 32;
  options.level = hlog::LogLevel::Info;
  options.flush_level = hlog::LogLevel::Off;

  hlog::AsyncLogger logger("multi-sink-test", std::move(multi_sink), options);
  Expect(logger.Info("fanout payload"), "multi sink log should succeed");
  Expect(logger.Flush(), "multi sink flush should succeed");
  logger.Stop();

  ExpectEqual(inspecting_sink_ptr->payloads().size(), std::size_t{1}, "multi sink payload count");
  ExpectEqual(
      inspecting_sink_ptr->payloads().front(),
      std::string("fanout payload"),
      "multi sink payload");
  ExpectEqual(counting_sink_ptr->written(), std::uint64_t{1}, "multi sink counting child");
}

void TestConsoleSinkFormatsMessages() {
  std::ostringstream output;
  hlog::ConsoleSinkOptions options;
  options.auto_flush = true;
  hlog::ConsoleSink sink(output, options);

  hlog::LogMessage message;
  message.timestamp = std::chrono::system_clock::from_time_t(0);
  message.level = hlog::LogLevel::Warn;
  message.logger_name = "console-test";
  message.thread_id = 42;
  message.payload = std::string("console payload");

  sink.Write(message);
  sink.Flush();

  const std::string text = output.str();
  Expect(text.find("[WARN]") != std::string::npos, "console sink level missing");
  Expect(text.find("[console-test]") != std::string::npos, "console sink logger missing");
  Expect(text.find("console payload") != std::string::npos, "console sink payload missing");
}

void TestRotatingFileSinkRotatesBySize() {
  const auto base_path = std::filesystem::temp_directory_path() / "hlog_rotating_size_test.log";
  std::filesystem::remove(base_path);
  std::filesystem::remove(base_path.string() + ".1");
  std::filesystem::remove(base_path.string() + ".2");

  hlog::RotatingFileSinkOptions options;
  options.truncate_on_open = true;
  options.max_file_size = 180;
  options.max_files = 2;
  options.max_batch_size = 1 << 20;
  options.flush_interval = std::chrono::milliseconds{0};

  hlog::RotatingFileSink sink(base_path.string(), options);
  for (int index = 0; index < 3; ++index) {
    hlog::LogMessage message;
    message.timestamp = std::chrono::system_clock::from_time_t(index + 1);
    message.level = hlog::LogLevel::Info;
    message.logger_name = "rotate-test";
    message.thread_id = static_cast<std::uint64_t>(index);
    message.payload = "msg=" + std::to_string(index) + " " + std::string(96, static_cast<char>('a' + index));
    sink.Write(message);
  }
  sink.Flush();

  const auto current_lines = ReadLines(base_path);
  const auto previous_lines = ReadLines(base_path.string() + ".1");
  const auto oldest_lines = ReadLines(base_path.string() + ".2");

  ExpectEqual(current_lines.size(), std::size_t{1}, "rotating sink current line count");
  ExpectEqual(previous_lines.size(), std::size_t{1}, "rotating sink previous line count");
  ExpectEqual(oldest_lines.size(), std::size_t{1}, "rotating sink oldest line count");
  Expect(current_lines.front().find("msg=2") != std::string::npos, "rotating sink current payload");
  Expect(previous_lines.front().find("msg=1") != std::string::npos, "rotating sink previous payload");
  Expect(oldest_lines.front().find("msg=0") != std::string::npos, "rotating sink oldest payload");
}

void TestLoggerFactoryBuildsConfiguredFileSink() {
  const auto path = std::filesystem::temp_directory_path() / "hlog_logger_factory.log";
  std::filesystem::remove(path);

  hlog::LoggerConfig config;
  config.name = "factory-test";
  config.async_options.queue_size = 64;
  config.async_options.level = hlog::LogLevel::Info;
  config.async_options.flush_level = hlog::LogLevel::Off;

  hlog::FileSinkConfig file;
  file.path = path.string();
  file.options.truncate = true;
  file.options.pattern = "%n|%l|%v|%s|%#|%!";
  config.sinks.emplace_back(std::move(file));

  auto logger = hlog::CreateLogger(std::move(config));
  Expect(logger != nullptr, "logger factory should create logger");
  Expect(HLOG_INFO(*logger, "factory payload"), "factory logger should write");
  Expect(logger->Flush(), "factory logger should flush");
  logger->Stop();

  const auto lines = ReadLines(path);
  ExpectEqual(lines.size(), std::size_t{1}, "logger factory line count");
  Expect(lines.front().find("factory-test|INFO|factory payload|async_logger_test.cpp|") != std::string::npos, "logger factory line format");
}

void TestCreateSinkRejectsEmptyConfig() {
  bool threw = false;
  try {
    const std::vector<hlog::SinkConfig> sinks;
    [[maybe_unused]] auto sink = hlog::CreateSink(sinks);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  Expect(threw, "empty sink config should throw");
}

}  // namespace

int main() {
  try {
    TestLevelFiltering();
    TestConcurrentAsyncWrite();
    TestDropPolicy();
    TestFlushWaitsForOutstandingWrites();
    TestStopRejectsNewWrites();
    TestStopDrainsQueuedWrites();
    TestBatchedFileSinkFlushesTailOnStop();
    TestShortPayloadUsesInlineStorage();
    TestLargePayloadSpillsToHeap();
    TestPatternFormatterExpandsTokens();
    TestParseLogLevelAcceptsCommonForms();
    TestMultiSinkFansOutToChildren();
    TestConsoleSinkFormatsMessages();
    TestRotatingFileSinkRotatesBySize();
    TestLoggerFactoryBuildsConfiguredFileSink();
    TestCreateSinkRejectsEmptyConfig();
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
