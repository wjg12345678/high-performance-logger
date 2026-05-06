#pragma once

#include "hlog/log_message.h"

namespace hlog {

class Sink {
public:
  virtual ~Sink() = default;

  virtual void Write(const LogMessage& message) = 0;
  virtual void Flush() = 0;
};

}  // namespace hlog
