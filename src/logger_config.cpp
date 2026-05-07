#include "hlog/logger_config.h"

#include "hlog/multi_sink.h"

#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace hlog {

namespace {

template <typename... Visitors>
struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

}  // namespace

std::unique_ptr<Sink> CreateSink(const SinkConfig& config) {
  return std::visit(
      Overloaded{
          [](const ConsoleSinkConfig& console) -> std::unique_ptr<Sink> {
            return std::make_unique<ConsoleSink>(console.options);
          },
          [](const FileSinkConfig& file) -> std::unique_ptr<Sink> {
            return std::make_unique<FileSink>(file.path, file.options);
          },
          [](const RotatingFileSinkConfig& rotating) -> std::unique_ptr<Sink> {
            return std::make_unique<RotatingFileSink>(rotating.path, rotating.options);
          },
      },
      config.value);
}

std::unique_ptr<Sink> CreateSink(const std::vector<SinkConfig>& configs) {
  if (configs.empty()) {
    throw std::invalid_argument("CreateSink requires at least one sink config");
  }

  if (configs.size() == 1) {
    return CreateSink(configs.front());
  }

  std::vector<std::unique_ptr<Sink>> sinks;
  sinks.reserve(configs.size());
  for (const auto& config : configs) {
    sinks.push_back(CreateSink(config));
  }
  return std::make_unique<MultiSink>(std::move(sinks));
}

std::unique_ptr<AsyncLogger> CreateLogger(LoggerConfig config) {
  return std::make_unique<AsyncLogger>(
      std::move(config.name),
      CreateSink(config.sinks),
      config.async_options);
}

}  // namespace hlog
