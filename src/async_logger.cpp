#include "hlog/async_logger.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hlog {

AsyncLogger::AsyncLogger(std::string name, std::unique_ptr<Sink> sink, AsyncLoggerOptions options)
    : name_(std::move(name)),
      sink_(std::move(sink)),
      queue_(options.queue_size),
      overflow_policy_(options.overflow_policy),
      level_(options.level),
      flush_level_(options.flush_level) {
  if (!sink_) {
    throw std::invalid_argument("AsyncLogger requires a valid sink");
  }
  worker_ = std::thread(&AsyncLogger::WorkerLoop, this);
}

AsyncLogger::~AsyncLogger() {
  Stop();
}

bool AsyncLogger::Flush() {
  OperationGuard guard(*this);
  if (!worker_.joinable()) {
    return false;
  }

  QueueItem item;
  item.type = QueueItemType::Flush;
  const std::uint64_t ticket =
      flush_request_ticket_.fetch_add(1, std::memory_order_acq_rel) + 1;
  item.ticket = ticket;
  if (!Publish(std::move(item), true)) {
    return false;
  }

  std::uint64_t completed = flush_complete_ticket_.load(std::memory_order_acquire);
  while (completed < ticket) {
    flush_complete_ticket_.wait(completed, std::memory_order_relaxed);
    completed = flush_complete_ticket_.load(std::memory_order_acquire);
  }
  return true;
}

void AsyncLogger::Stop() {
  bool expected = false;
  if (stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    running_.store(false, std::memory_order_release);
    wake_signal_.fetch_add(1, std::memory_order_acq_rel);
    wake_signal_.notify_all();
  }

  if (worker_.joinable()) {
    worker_.join();
  }
}

void AsyncLogger::SetLevel(LogLevel level) {
  level_.store(level, std::memory_order_release);
}

LogLevel AsyncLogger::level() const {
  return level_.load(std::memory_order_acquire);
}

void AsyncLogger::SetFlushLevel(LogLevel level) {
  flush_level_.store(level, std::memory_order_release);
}

LogLevel AsyncLogger::flush_level() const {
  return flush_level_.load(std::memory_order_acquire);
}

const std::string& AsyncLogger::name() const {
  return name_;
}

LoggerStats AsyncLogger::Stats() const {
  const std::uint64_t enqueued = enqueued_.load(std::memory_order_relaxed);
  const std::uint64_t written = written_.load(std::memory_order_relaxed);
  return LoggerStats{
      enqueued,
      dropped_.load(std::memory_order_relaxed),
      written,
      enqueued >= written ? enqueued - written : 0,
  };
}

bool AsyncLogger::Publish(QueueItem&& item, bool block_on_full) {
  const bool count_as_log = item.type == QueueItemType::Log;
  std::uint32_t attempts = 0;

  if (count_as_log) {
    enqueued_.fetch_add(1, std::memory_order_relaxed);
  }

  while (running_.load(std::memory_order_acquire) || (!stopping_.load(std::memory_order_acquire) && block_on_full)) {
    if (queue_.TryEnqueue(std::move(item))) {
      wake_signal_.fetch_add(1, std::memory_order_acq_rel);
      wake_signal_.notify_one();
      return true;
    }

    if (!block_on_full) {
      if (count_as_log) {
        enqueued_.fetch_sub(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }

    ++attempts;
    if (attempts < 64) {
      continue;
    }
    if (attempts < 256) {
      std::this_thread::yield();
      continue;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }

  if (count_as_log) {
    enqueued_.fetch_sub(1, std::memory_order_relaxed);
  }
  return false;
}

void AsyncLogger::FinishOperation() {
  const std::uint64_t remaining =
      active_operations_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0 && stopping_.load(std::memory_order_acquire)) {
    wake_signal_.fetch_add(1, std::memory_order_acq_rel);
    wake_signal_.notify_all();
  }
}

bool AsyncLogger::HandleItem(const QueueItem& item) {
  if (item.type == QueueItemType::Flush) {
    sink_->Flush();
    flush_complete_ticket_.store(item.ticket, std::memory_order_release);
    flush_complete_ticket_.notify_all();
    return false;
  }

  sink_->Write(item.message);
  written_.fetch_add(1, std::memory_order_relaxed);
  return ShouldLog(item.message.level, flush_level_.load(std::memory_order_acquire));
}

void AsyncLogger::WorkerLoop() {
  while (true) {
    QueueItem item;
    bool flush_required = false;

    while (queue_.TryDequeue(item)) {
      flush_required = HandleItem(item) || flush_required;
    }

    if (flush_required) {
      sink_->Flush();
    }

    const std::uint64_t signal = wake_signal_.load(std::memory_order_acquire);

    if (queue_.TryDequeue(item)) {
      flush_required = HandleItem(item);
      if (flush_required) {
        sink_->Flush();
      }
      continue;
    }

    if (stopping_.load(std::memory_order_acquire) &&
        active_operations_.load(std::memory_order_acquire) == 0) {
      break;
    }

    wake_signal_.wait(signal, std::memory_order_relaxed);
  }

  sink_->Flush();
}

}  // namespace hlog
