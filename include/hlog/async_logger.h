#pragma once

#include "hlog/lock_free_ring_buffer.h"
#include "hlog/log_message.h"
#include "hlog/sink.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace hlog {

enum class OverflowPolicy : std::uint8_t {
  Block = 0,
  DropNewest = 1,
};

struct AsyncLoggerOptions {
  std::size_t queue_size = 8192;
  OverflowPolicy overflow_policy = OverflowPolicy::Block;
  LogLevel level = LogLevel::Info;
  LogLevel flush_level = LogLevel::Error;
};

struct LoggerStats {
  std::uint64_t enqueued = 0;
  std::uint64_t dropped = 0;
  std::uint64_t written = 0;
  std::uint64_t pending = 0;
};

class AsyncLogger {
public:
  AsyncLogger(std::string name, std::unique_ptr<Sink> sink, AsyncLoggerOptions options = {});
  ~AsyncLogger();

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  template <typename... Args>
  bool Trace(Args&&... args) {
    return Log(LogLevel::Trace, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool Debug(Args&&... args) {
    return Log(LogLevel::Debug, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool Info(Args&&... args) {
    return Log(LogLevel::Info, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool Warn(Args&&... args) {
    return Log(LogLevel::Warn, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool Error(Args&&... args) {
    return Log(LogLevel::Error, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool Critical(Args&&... args) {
    return Log(LogLevel::Critical, {}, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool TraceAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Trace, source, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool DebugAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Debug, source, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool InfoAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Info, source, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool WarnAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Warn, source, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool ErrorAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Error, source, std::forward<Args>(args)...);
  }

  template <typename... Args>
  bool CriticalAt(SourceLocation source, Args&&... args) {
    return Log(LogLevel::Critical, source, std::forward<Args>(args)...);
  }

  bool Flush();
  void Stop();

  void SetLevel(LogLevel level);
  LogLevel level() const;

  void SetFlushLevel(LogLevel level);
  LogLevel flush_level() const;

  const std::string& name() const;
  LoggerStats Stats() const;

private:
  class OperationGuard {
  public:
    explicit OperationGuard(AsyncLogger& logger) : logger_(logger) {
      logger_.active_operations_.fetch_add(1, std::memory_order_acq_rel);
    }

    ~OperationGuard() {
      logger_.FinishOperation();
    }

  private:
    AsyncLogger& logger_;
  };

  template <typename... Args>
  bool Log(LogLevel level, SourceLocation source, Args&&... args) {
    OperationGuard guard(*this);
    if (!running_.load(std::memory_order_acquire) ||
        !ShouldLog(level, level_.load(std::memory_order_acquire))) {
      return false;
    }

    QueueItem item;
    item.type = QueueItemType::Log;
    item.message.timestamp = std::chrono::system_clock::now();
    item.message.level = level;
    item.message.logger_name = name_;
    item.message.thread_id = CurrentThreadId();
    item.message.source = source;
    item.message.payload = BuildPayload(std::forward<Args>(args)...);
    return Publish(std::move(item), overflow_policy_ == OverflowPolicy::Block);
  }

  template <typename... Args>
  static LogPayload BuildPayload(Args&&... args) {
    LogPayload payload;
    if constexpr (sizeof...(Args) == 0) {
      return payload;
    } else {
      (AppendPayloadPart(payload, std::forward<Args>(args)), ...);
      return payload;
    }
  }

  bool Publish(QueueItem&& item, bool block_on_full);
  bool HandleItem(const QueueItem& item);
  static std::uint64_t CurrentThreadId() {
    // Cache the hashed thread id once per producer thread to reduce hot-path work.
    static thread_local const std::uint64_t thread_id =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return thread_id;
  }
  void FinishOperation();
  void WorkerLoop();

  template <typename T>
  static void AppendPayloadPart(LogPayload& payload, T&& value) {
    using Decayed = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Decayed, LogPayload>) {
      payload.append(value.view());
    } else if constexpr (
        std::is_same_v<Decayed, std::string> || std::is_same_v<Decayed, std::string_view>) {
      payload.append(value);
    } else if constexpr (
        std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
      payload.append(value != nullptr ? std::string_view(value) : std::string_view("(null)"));
    } else if constexpr (
        std::is_array_v<Decayed> &&
        std::is_same_v<std::remove_cv_t<std::remove_extent_t<Decayed>>, char>) {
      payload.append(std::string_view(value));
    } else if constexpr (std::is_same_v<Decayed, char>) {
      payload.push_back(value);
    } else if constexpr (
        std::is_same_v<Decayed, signed char> || std::is_same_v<Decayed, unsigned char>) {
      payload.push_back(static_cast<char>(value));
    } else if constexpr (std::is_same_v<Decayed, bool>) {
      payload.push_back(value ? '1' : '0');
    } else if constexpr (std::is_integral_v<Decayed>) {
      char buffer[std::numeric_limits<Decayed>::digits10 + 3];
      const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
      if (error == std::errc{}) {
        payload.append(std::string_view(buffer, static_cast<std::size_t>(end - buffer)));
      }
    } else if constexpr (std::is_floating_point_v<Decayed>) {
      std::ostringstream stream;
      stream << value;
      payload.append(stream.str());
    } else if constexpr (requires(std::ostream& output, const Decayed& item) { output << item; }) {
      std::ostringstream stream;
      stream << value;
      payload.append(stream.str());
    } else {
      static_assert(!sizeof(T), "Log argument must be string-like, numeric, or streamable");
    }
  }

  std::string name_;
  std::unique_ptr<Sink> sink_;
  LockFreeRingBuffer<QueueItem> queue_;
  OverflowPolicy overflow_policy_;
  std::thread worker_;

  std::atomic<bool> running_{true};
  std::atomic<bool> stopping_{false};
  std::atomic<LogLevel> level_{LogLevel::Info};
  std::atomic<LogLevel> flush_level_{LogLevel::Error};

  std::atomic<std::uint64_t> active_operations_{0};
  std::atomic<std::uint64_t> wake_signal_{0};
  std::atomic<std::uint64_t> flush_request_ticket_{0};
  std::atomic<std::uint64_t> flush_complete_ticket_{0};

  std::atomic<std::uint64_t> enqueued_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> written_{0};
};

}  // namespace hlog

#define HLOG_TRACE(logger, ...) \
  (logger).TraceAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
#define HLOG_DEBUG(logger, ...) \
  (logger).DebugAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
#define HLOG_INFO(logger, ...) \
  (logger).InfoAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
#define HLOG_WARN(logger, ...) \
  (logger).WarnAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
#define HLOG_ERROR(logger, ...) \
  (logger).ErrorAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
#define HLOG_CRITICAL(logger, ...) \
  (logger).CriticalAt(::hlog::SourceLocation{__FILE__, __LINE__, __func__} __VA_OPT__(,) __VA_ARGS__)
