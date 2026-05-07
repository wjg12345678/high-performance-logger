#include "hlog/file_sink.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace hlog {

namespace {

constexpr std::size_t kMinBatchReserve = 4 * 1024;

std::size_t NormalizeBatchSize(std::size_t batch_size) {
  return batch_size == 0 ? 1 : batch_size;
}

FileSinkOptions MakeFileSinkOptions(bool truncate) {
  FileSinkOptions options;
  options.truncate = truncate;
  return options;
}

}  // namespace

FileSink::FileSink(const std::string& path, bool truncate)
    : FileSink(path, MakeFileSinkOptions(truncate)) {}

FileSink::FileSink(const std::string& path, FileSinkOptions options)
    : options_(options),
      formatter_(options.pattern) {
  options_.max_batch_size = NormalizeBatchSize(options_.max_batch_size);
  const auto mode = std::ios::out | (options_.truncate ? std::ios::trunc : std::ios::app);
  output_.open(path, mode);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open log file: " + path);
  }
  staging_buffer_.reserve(std::max(options_.max_batch_size, kMinBatchReserve));
}

void FileSink::DrainStagingBuffer(bool flush_stream) {
  if (!staging_buffer_.empty()) {
    output_.write(staging_buffer_.data(), static_cast<std::streamsize>(staging_buffer_.size()));
    if (!output_) {
      throw std::runtime_error("failed to write log batch");
    }
    staging_buffer_.clear();
    batch_started_at_ = {};
  }

  if (flush_stream) {
    output_.flush();
    if (!output_) {
      throw std::runtime_error("failed to flush log file");
    }
  }
}

void FileSink::Write(const LogMessage& message) {
  const auto now = Clock::now();
  if (staging_buffer_.empty()) {
    batch_started_at_ = now;
  }

  formatter_.FormatTo(staging_buffer_, message);
  staging_buffer_.push_back('\n');

  const bool flush_for_size = staging_buffer_.size() >= options_.max_batch_size;
  const bool flush_for_interval =
      options_.flush_interval.count() > 0 &&
      batch_started_at_ != Clock::time_point{} &&
      now - batch_started_at_ >= options_.flush_interval;
  if (flush_for_size || flush_for_interval) {
    DrainStagingBuffer(flush_for_interval);
  }
}

void FileSink::Flush() {
  DrainStagingBuffer(true);
}

}  // namespace hlog
