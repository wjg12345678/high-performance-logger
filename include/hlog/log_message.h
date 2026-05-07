#pragma once

#include "hlog/log_payload.h"
#include "hlog/log_level.h"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace hlog {

struct SourceLocation {
  std::string_view file;
  int line = 0;
  std::string_view function;
};

struct LogMessage {
  std::chrono::system_clock::time_point timestamp{};
  LogLevel level = LogLevel::Info;
  std::string_view logger_name;
  std::uint64_t thread_id = 0;
  SourceLocation source{};
  LogPayload payload;
};

enum class QueueItemType : std::uint8_t {
  Log = 0,
  Flush = 1,
};

struct QueueItem {
  QueueItemType type = QueueItemType::Log;
  std::uint64_t ticket = 0;
  LogMessage message{};
};

}  // namespace hlog
