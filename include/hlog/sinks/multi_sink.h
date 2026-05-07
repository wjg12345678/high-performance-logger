#pragma once

#include "hlog/sink.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hlog {

class MultiSink final : public Sink {
public:
  MultiSink() = default;

  explicit MultiSink(std::vector<std::unique_ptr<Sink>> sinks) {
    for (auto& sink : sinks) {
      AddSink(std::move(sink));
    }
  }

  void AddSink(std::unique_ptr<Sink> sink) {
    if (!sink) {
      throw std::invalid_argument("MultiSink requires non-null child sinks");
    }
    sinks_.push_back(std::move(sink));
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return sinks_.size();
  }

  [[nodiscard]] bool empty() const noexcept {
    return sinks_.empty();
  }

  void Write(const LogMessage& message) override {
    for (auto& sink : sinks_) {
      sink->Write(message);
    }
  }

  void Flush() override {
    for (auto& sink : sinks_) {
      sink->Flush();
    }
  }

  Sink::Clock::time_point NextAutoFlushTime() const override {
    auto next_flush = Sink::Clock::time_point::max();
    for (const auto& sink : sinks_) {
      next_flush = std::min(next_flush, sink->NextAutoFlushTime());
    }
    return next_flush;
  }

  bool FlushIfDue(Sink::Clock::time_point now) override {
    bool flushed = false;
    for (auto& sink : sinks_) {
      flushed = sink->FlushIfDue(now) || flushed;
    }
    return flushed;
  }

private:
  std::vector<std::unique_ptr<Sink>> sinks_;
};

}  // namespace hlog
