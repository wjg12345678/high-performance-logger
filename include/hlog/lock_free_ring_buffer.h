#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hlog {

template <typename T>
class LockFreeRingBuffer {
public:
  explicit LockFreeRingBuffer(std::size_t requested_capacity)
      : capacity_(RoundUpPowerOfTwo(requested_capacity)),
        mask_(capacity_ - 1),
        buffer_(std::make_unique<Cell[]>(capacity_)) {
    for (std::size_t i = 0; i < capacity_; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
  LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

  bool TryEnqueue(T&& value) {
    std::size_t position = enqueue_pos_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = buffer_[position & mask_];
      const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff =
          static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(
                position,
                position + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          cell.value = std::move(value);
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        position = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  bool TryDequeue(T& value) {
    std::size_t position = dequeue_pos_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = buffer_[position & mask_];
      const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff =
          static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(
                position,
                position + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          value = std::move(cell.value);
          cell.sequence.store(position + capacity_, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        position = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  std::size_t capacity() const {
    return capacity_;
  }

private:
  struct Cell {
    std::atomic<std::size_t> sequence{0};
    T value{};
  };

  static std::size_t RoundUpPowerOfTwo(std::size_t value) {
    std::size_t normalized = value < 2 ? 2 : value;
    --normalized;
    normalized |= normalized >> 1;
    normalized |= normalized >> 2;
    normalized |= normalized >> 4;
    normalized |= normalized >> 8;
    normalized |= normalized >> 16;
    if constexpr (sizeof(std::size_t) == 8) {
      normalized |= normalized >> 32;
    }
    return normalized + 1;
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<Cell[]> buffer_;
  alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace hlog
