#pragma once

#include <cstdint>

namespace hlog {

enum class LogLevel : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Critical = 5,
  Off = 6,
};

constexpr const char* ToString(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return "TRACE";
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
    case LogLevel::Critical:
      return "CRITICAL";
    case LogLevel::Off:
      return "OFF";
  }
  return "UNKNOWN";
}

constexpr bool ShouldLog(LogLevel message_level, LogLevel configured_level) {
  return configured_level != LogLevel::Off &&
      static_cast<std::uint8_t>(message_level) >= static_cast<std::uint8_t>(configured_level) &&
      message_level != LogLevel::Off;
}

}  // namespace hlog
