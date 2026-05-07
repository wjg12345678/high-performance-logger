#pragma once

#include <cctype>
#include <cstdint>
#include <iosfwd>
#include <string_view>

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

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }

  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_char = static_cast<unsigned char>(left[index]);
    const auto right_char = static_cast<unsigned char>(right[index]);
    if (std::tolower(left_char) != std::tolower(right_char)) {
      return false;
    }
  }
  return true;
}

inline bool TryParseLogLevel(std::string_view text, LogLevel& level) {
  if (EqualsIgnoreCase(text, "trace")) {
    level = LogLevel::Trace;
    return true;
  }
  if (EqualsIgnoreCase(text, "debug")) {
    level = LogLevel::Debug;
    return true;
  }
  if (EqualsIgnoreCase(text, "info")) {
    level = LogLevel::Info;
    return true;
  }
  if (EqualsIgnoreCase(text, "warn") || EqualsIgnoreCase(text, "warning")) {
    level = LogLevel::Warn;
    return true;
  }
  if (EqualsIgnoreCase(text, "error")) {
    level = LogLevel::Error;
    return true;
  }
  if (EqualsIgnoreCase(text, "critical")) {
    level = LogLevel::Critical;
    return true;
  }
  if (EqualsIgnoreCase(text, "off")) {
    level = LogLevel::Off;
    return true;
  }
  return false;
}

inline LogLevel ParseLogLevel(std::string_view text, LogLevel fallback = LogLevel::Info) {
  LogLevel level = fallback;
  return TryParseLogLevel(text, level) ? level : fallback;
}

inline std::ostream& operator<<(std::ostream& output, LogLevel level) {
  return output << ToString(level);
}

}  // namespace hlog
