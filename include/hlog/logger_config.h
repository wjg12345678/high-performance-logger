#pragma once

#include "hlog/async_logger.h"
#include "hlog/sink.h"
#include "hlog/sinks/console_sink.h"
#include "hlog/sinks/file_sink.h"
#include "hlog/sinks/rotating_file_sink.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hlog {

struct ConsoleSinkConfig {
  ConsoleSinkOptions options{};
};

struct FileSinkConfig {
  std::string path;
  FileSinkOptions options{};
};

struct RotatingFileSinkConfig {
  std::string path;
  RotatingFileSinkOptions options{};
};

struct SinkConfig {
  using Variant = std::variant<ConsoleSinkConfig, FileSinkConfig, RotatingFileSinkConfig>;

  Variant value;

  SinkConfig(ConsoleSinkConfig config) : value(std::move(config)) {}
  SinkConfig(FileSinkConfig config) : value(std::move(config)) {}
  SinkConfig(RotatingFileSinkConfig config) : value(std::move(config)) {}
};

struct LoggerConfig {
  std::string name = "hlog";
  AsyncLoggerOptions async_options{};
  std::vector<SinkConfig> sinks;
};

std::unique_ptr<Sink> CreateSink(const SinkConfig& config);
std::unique_ptr<Sink> CreateSink(const std::vector<SinkConfig>& configs);
std::unique_ptr<AsyncLogger> CreateLogger(LoggerConfig config);

}  // namespace hlog
