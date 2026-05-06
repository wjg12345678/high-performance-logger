#pragma once

#include "hlog/lock_free_ring_buffer.h"
#include "hlog/log_message.h"
#include "hlog/sink.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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
  template <typename... Args>
  bool Log(LogLevel level, SourceLocation source, Args&&... args) {
    if (!running_.load(std::memory_order_acquire) ||
        !ShouldLog(level, level_.load(std::memory_order_acquire))) {
      return false;
    }

    QueueItem item;
    item.type = QueueItemType::Log;
    item.message.timestamp = std::chrono::system_clock::now();
    item.message.level = level;
    item.message.logger_name = name_;
    item.message.thread_id =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    item.message.source = source;
    item.message.payload = BuildPayload(std::forward<Args>(args)...);
    return Publish(std::move(item), overflow_policy_ == OverflowPolicy::Block);
  }

  template <typename... Args>
  static std::string BuildPayload(Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
      return {};
    } else {
      std::ostringstream stream;
      (stream << ... << std::forward<Args>(args));
      return stream.str();
    }
  }

  bool Publish(QueueItem&& item, bool block_on_full);
  bool HandleItem(const QueueItem& item);
  void WorkerLoop();

  std::string name_;
  std::unique_ptr<Sink> sink_;
  LockFreeRingBuffer<QueueItem> queue_;
  OverflowPolicy overflow_policy_;
  std::thread worker_;

  std::atomic<bool> running_{true};
  std::atomic<bool> stopping_{false};
  std::atomic<LogLevel> level_{LogLevel::Info};
  std::atomic<LogLevel> flush_level_{LogLevel::Error};

  std::atomic<std::uint64_t> pending_items_{0};
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
