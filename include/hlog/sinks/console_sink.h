#pragma once

#include "hlog/pattern_formatter.h"
#include "hlog/sink.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace hlog {

enum class ConsoleStream {
  Stdout = 0,
  Stderr = 1,
};

struct ConsoleSinkOptions {
  ConsoleStream stream = ConsoleStream::Stdout;
  bool auto_flush = false;
  std::string pattern = std::string(kDefaultLogPattern);
};

class ConsoleSink final : public Sink {
public:
  explicit ConsoleSink(ConsoleSinkOptions options = {});
  ConsoleSink(std::ostream& output, ConsoleSinkOptions options = {});

  void Write(const LogMessage& message) override;
  void Flush() override;

private:
  std::reference_wrapper<std::ostream> output_;
  bool auto_flush_ = false;
  PatternFormatter formatter_;
};

}  // namespace hlog
