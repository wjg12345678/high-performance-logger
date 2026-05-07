#include "hlog/sinks/console_sink.h"

#include <iostream>
#include <ostream>
#include <string>

namespace hlog {

namespace {

std::ostream& ResolveConsoleStream(ConsoleStream stream) {
  return stream == ConsoleStream::Stderr ? std::cerr : std::cout;
}

}  // namespace

ConsoleSink::ConsoleSink(ConsoleSinkOptions options)
    : ConsoleSink(ResolveConsoleStream(options.stream), options) {}

ConsoleSink::ConsoleSink(std::ostream& output, ConsoleSinkOptions options)
    : output_(output),
      auto_flush_(options.auto_flush),
      formatter_(std::move(options.pattern)) {}

void ConsoleSink::Write(const LogMessage& message) {
  std::string formatted;
  formatted.reserve(256);
  formatter_.FormatTo(formatted, message);
  formatted.push_back('\n');
  output_.get().write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
  if (auto_flush_) {
    output_.get().flush();
  }
}

void ConsoleSink::Flush() {
  output_.get().flush();
}

}  // namespace hlog
