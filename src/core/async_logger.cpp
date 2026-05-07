#include "hlog/async_logger.h"

#include <chrono>
#include <exception>
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
  OperationGuard guard = TryStartOperation();
  if (!guard || !worker_.joinable() || HasWorkerFailure()) {
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
    if (HasWorkerFailure()) {
      return false;
    }
    flush_complete_ticket_.wait(completed, std::memory_order_relaxed);
    completed = flush_complete_ticket_.load(std::memory_order_acquire);
  }
  return !HasWorkerFailure();
}

void AsyncLogger::Stop() {
  if (RequestStop()) {
    NotifyWorker();
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

bool AsyncLogger::failed() const {
  return HasWorkerFailure();
}

std::string AsyncLogger::failure_message() const {
  std::lock_guard<std::mutex> lock(failure_mutex_);
  return background_failure_message_;
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

  while (!HasWorkerFailure()) {
    if (queue_.TryEnqueue(std::move(item))) {
      NotifyWorker();
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

AsyncLogger::OperationGuard AsyncLogger::TryStartOperation() {
  std::uint64_t state = operation_state_.load(std::memory_order_acquire);
  while (true) {
    if ((state & kOperationGateClosedBit) != 0) {
      return OperationGuard{};
    }

    if (operation_state_.compare_exchange_weak(
            state,
            state + 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return OperationGuard(*this);
    }
  }
}

void AsyncLogger::FinishOperation() {
  const std::uint64_t previous = operation_state_.fetch_sub(1, std::memory_order_acq_rel);
  const std::uint64_t remaining = (previous & kActiveOperationMask) - 1;
  if ((previous & kOperationGateClosedBit) != 0 && remaining == 0) {
    NotifyWorker();
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

bool AsyncLogger::HasWorkerFailure() const {
  return worker_failed_.load(std::memory_order_acquire);
}

void AsyncLogger::NotifyWorker() {
  wake_signal_.fetch_add(1, std::memory_order_acq_rel);
  wake_condition_.notify_one();
}

void AsyncLogger::RecordWorkerFailure(std::exception_ptr error) {
  {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (!background_failure_) {
      background_failure_ = error;
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& exception) {
        background_failure_message_ = exception.what();
      } catch (...) {
        background_failure_message_ = "unknown async logger worker failure";
      }
    }
  }

  (void)RequestStop();
  worker_failed_.store(true, std::memory_order_release);
  flush_complete_ticket_.store(
      flush_request_ticket_.load(std::memory_order_acquire),
      std::memory_order_release);
  flush_complete_ticket_.notify_all();
  NotifyWorker();
}

bool AsyncLogger::RequestStop() {
  std::uint64_t state = operation_state_.load(std::memory_order_acquire);
  while (true) {
    if ((state & kOperationGateClosedBit) != 0) {
      return false;
    }

    if (operation_state_.compare_exchange_weak(
            state,
            state | kOperationGateClosedBit,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
}

bool AsyncLogger::StopRequested() const {
  return (operation_state_.load(std::memory_order_acquire) & kOperationGateClosedBit) != 0;
}

std::uint64_t AsyncLogger::ActiveOperationCount() const {
  return operation_state_.load(std::memory_order_acquire) & kActiveOperationMask;
}

void AsyncLogger::WaitForWakeup(std::uint64_t signal) {
  std::unique_lock<std::mutex> lock(wake_mutex_);
  const auto should_wake = [this, signal]() {
    return wake_signal_.load(std::memory_order_acquire) != signal ||
        HasWorkerFailure() ||
        (StopRequested() && ActiveOperationCount() == 0);
  };

  const auto deadline = sink_->NextAutoFlushTime();
  if (deadline == Sink::Clock::time_point::max()) {
    wake_condition_.wait(lock, should_wake);
    return;
  }

  wake_condition_.wait_until(lock, deadline, should_wake);
}

void AsyncLogger::WorkerLoop() {
  try {
    while (true) {
      QueueItem item;
      bool made_progress = false;
      bool flush_required = false;

      while (queue_.TryDequeue(item)) {
        made_progress = true;
        flush_required = HandleItem(item) || flush_required;
      }

      if (flush_required) {
        sink_->Flush();
        made_progress = true;
      }

      if (sink_->FlushIfDue(Sink::Clock::now())) {
        made_progress = true;
      }

      if (StopRequested() && ActiveOperationCount() == 0) {
        break;
      }

      if (made_progress) {
        continue;
      }

      const std::uint64_t signal = wake_signal_.load(std::memory_order_acquire);
      if (queue_.TryDequeue(item)) {
        flush_required = HandleItem(item);
        if (flush_required) {
          sink_->Flush();
        }
        continue;
      }

      WaitForWakeup(signal);
    }

    sink_->Flush();
  } catch (...) {
    RecordWorkerFailure(std::current_exception());
  }
}

}  // namespace hlog
