#pragma once

#include "hlog/detail/log_message.h"

#include <chrono>

namespace hlog {

class Sink {
public:
  using Clock = std::chrono::steady_clock;

  virtual ~Sink() = default;

  virtual void Write(const LogMessage& message) = 0;
  virtual void Flush() = 0;
  virtual Clock::time_point NextAutoFlushTime() const {
    return Clock::time_point::max();
  }
  virtual bool FlushIfDue(Clock::time_point) {
    return false;
  }
};

}  // namespace hlog
