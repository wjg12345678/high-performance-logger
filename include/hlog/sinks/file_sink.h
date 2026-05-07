#pragma once

#include "hlog/pattern_formatter.h"
#include "hlog/sink.h"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>

namespace hlog {

struct FileSinkOptions {
  bool truncate = false;
  std::size_t max_batch_size = 64 * 1024;
  std::chrono::milliseconds flush_interval{50};
  std::string pattern = std::string(kDefaultLogPattern);
};

class FileSink final : public Sink {
public:
  explicit FileSink(const std::string& path, bool truncate = false);
  FileSink(const std::string& path, FileSinkOptions options);

  void Write(const LogMessage& message) override;
  void Flush() override;
  Sink::Clock::time_point NextAutoFlushTime() const override;
  bool FlushIfDue(Sink::Clock::time_point now) override;

private:
  using Clock = Sink::Clock;

  void DrainStagingBuffer(bool flush_stream);

  FileSinkOptions options_{};
  std::ofstream output_;
  std::string staging_buffer_;
  Clock::time_point batch_started_at_{};
  PatternFormatter formatter_;
};

}  // namespace hlog
