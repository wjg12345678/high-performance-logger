#include "hlog/rotating_file_sink.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace hlog {

namespace {

constexpr std::size_t kMinBatchReserve = 4 * 1024;

std::size_t NormalizeSize(std::size_t value) {
  return value == 0 ? 1 : value;
}

std::filesystem::path ArchivePath(const std::string& path, std::size_t index) {
  return std::filesystem::path(path + "." + std::to_string(index));
}

void RemoveIfExists(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("failed to remove path during rotation: " + path.string());
  }
}

void RenameIfExists(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  const bool exists = std::filesystem::exists(source, error);
  if (error) {
    throw std::runtime_error("failed to inspect path during rotation: " + source.string());
  }
  if (!exists) {
    return;
  }

  RemoveIfExists(destination);
  std::filesystem::rename(source, destination, error);
  if (error) {
    throw std::runtime_error(
        "failed to rotate log file from " + source.string() + " to " + destination.string());
  }
}

std::size_t ExistingFileSize(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return 0;
    }
    throw std::runtime_error("failed to query log file size: " + path);
  }
  return static_cast<std::size_t>(size);
}

}  // namespace

RotatingFileSink::RotatingFileSink(std::string path, RotatingFileSinkOptions options)
    : path_(std::move(path)),
      options_(options),
      formatter_(options.pattern) {
  options_.max_file_size = NormalizeSize(options_.max_file_size);
  options_.max_batch_size = NormalizeSize(options_.max_batch_size);

  if (path_.empty()) {
    throw std::invalid_argument("RotatingFileSink requires a non-empty path");
  }

  staging_buffer_.reserve(std::max(options_.max_batch_size, kMinBatchReserve));
  ResetOnStartup();
}

void RotatingFileSink::Open(std::ios::openmode mode) {
  output_.open(path_, std::ios::out | mode);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open log file: " + path_);
  }
}

void RotatingFileSink::ResetOnStartup() {
  if (options_.truncate_on_open) {
    RemoveIfExists(path_);
    for (std::size_t index = 1; index <= options_.max_files; ++index) {
      RemoveIfExists(ArchivePath(path_, index));
    }
    current_size_ = 0;
    Open(std::ios::trunc);
    return;
  }

  current_size_ = ExistingFileSize(path_);
  Open(std::ios::app);
}

void RotatingFileSink::RotateFiles() {
  output_.close();
  if (options_.max_files == 0) {
    current_size_ = 0;
    Open(std::ios::trunc);
    return;
  }

  for (std::size_t index = options_.max_files; index > 1; --index) {
    RenameIfExists(ArchivePath(path_, index - 1), ArchivePath(path_, index));
  }
  RenameIfExists(path_, ArchivePath(path_, 1));

  current_size_ = 0;
  Open(std::ios::trunc);
}

void RotatingFileSink::DrainStagingBuffer(bool flush_stream) {
  if (!staging_buffer_.empty()) {
    output_.write(staging_buffer_.data(), static_cast<std::streamsize>(staging_buffer_.size()));
    if (!output_) {
      throw std::runtime_error("failed to write rotating log batch");
    }
    current_size_ += staging_buffer_.size();
    staging_buffer_.clear();
    batch_started_at_ = {};
  }

  if (flush_stream) {
    output_.flush();
    if (!output_) {
      throw std::runtime_error("failed to flush rotating log file");
    }
  }
}

void RotatingFileSink::Write(const LogMessage& message) {
  std::string formatted;
  formatted.reserve(256);
  formatter_.FormatTo(formatted, message);
  formatted.push_back('\n');

  if (current_size_ + staging_buffer_.size() + formatted.size() > options_.max_file_size &&
      current_size_ + staging_buffer_.size() > 0) {
    DrainStagingBuffer(true);
    RotateFiles();
  }

  const auto now = Clock::now();
  if (staging_buffer_.empty()) {
    batch_started_at_ = now;
  }

  staging_buffer_.append(formatted);

  const bool flush_for_size = staging_buffer_.size() >= options_.max_batch_size;
  const bool flush_for_interval =
      options_.flush_interval.count() > 0 &&
      batch_started_at_ != Clock::time_point{} &&
      now - batch_started_at_ >= options_.flush_interval;
  if (flush_for_size || flush_for_interval) {
    DrainStagingBuffer(flush_for_interval);
  }
}

void RotatingFileSink::Flush() {
  DrainStagingBuffer(true);
}

}  // namespace hlog
