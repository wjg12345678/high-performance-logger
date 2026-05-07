#pragma once

#include "hlog/pattern_formatter.h"
#include "hlog/sink.h"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>

namespace hlog {

struct RotatingFileSinkOptions {
  bool truncate_on_open = false;
  std::size_t max_file_size = 10 * 1024 * 1024;
  std::size_t max_files = 3;
  std::size_t max_batch_size = 64 * 1024;
  std::chrono::milliseconds flush_interval{50};
  std::string pattern = std::string(kDefaultLogPattern);
};

class RotatingFileSink final : public Sink {
public:
  explicit RotatingFileSink(std::string path, RotatingFileSinkOptions options = {});

  void Write(const LogMessage& message) override;
  void Flush() override;

private:
  using Clock = std::chrono::steady_clock;

  void Open(std::ios::openmode mode);
  void ResetOnStartup();
  void RotateFiles();
  void DrainStagingBuffer(bool flush_stream);

  std::string path_;
  RotatingFileSinkOptions options_{};
  std::ofstream output_;
  std::string staging_buffer_;
  std::size_t current_size_ = 0;
  Clock::time_point batch_started_at_{};
  PatternFormatter formatter_;
};

}  // namespace hlog
