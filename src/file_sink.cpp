#include "hlog/file_sink.h"

#include "hlog/log_level.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace hlog {

namespace {

std::string_view BaseName(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::string FormatTimestamp(std::chrono::system_clock::time_point timestamp) {
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      timestamp.time_since_epoch()) %
      1000;
  const std::time_t seconds = std::chrono::system_clock::to_time_t(timestamp);

  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &seconds);
#else
  localtime_r(&seconds, &local_time);
#endif

  std::ostringstream stream;
  stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << millis.count();
  return stream.str();
}

}  // namespace

FileSink::FileSink(const std::string& path, bool truncate) {
  const auto mode = std::ios::out | (truncate ? std::ios::trunc : std::ios::app);
  output_.open(path, mode);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open log file: " + path);
  }
}

void FileSink::Write(const LogMessage& message) {
  output_ << FormatTimestamp(message.timestamp)
          << " [" << ToString(message.level) << "]"
          << " [" << message.logger_name << "]"
          << " [tid=" << message.thread_id << "] "
          << message.payload;

  if (!message.source.file.empty()) {
    output_ << " (" << BaseName(message.source.file) << ':' << message.source.line;
    if (!message.source.function.empty()) {
      output_ << ' ' << message.source.function;
    }
    output_ << ')';
  }

  output_ << '\n';
}

void FileSink::Flush() {
  output_.flush();
}

}  // namespace hlog
