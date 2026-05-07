#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace hlog {

class LogPayload {
public:
  static constexpr std::size_t kInlineCapacity = 256;

  LogPayload() {
    ResetInline();
  }

  LogPayload(std::string_view value) : LogPayload() {
    assign(value);
  }

  LogPayload(const std::string& value) : LogPayload(std::string_view(value)) {}

  LogPayload(const char* value) : LogPayload() {
    if (value != nullptr) {
      assign(std::string_view(value));
    }
  }

  LogPayload(const LogPayload& other) : LogPayload() {
    assign(other.view());
  }

  LogPayload(LogPayload&& other) noexcept : LogPayload() {
    MoveFrom(std::move(other));
  }

  LogPayload& operator=(const LogPayload& other) {
    if (this != &other) {
      assign(other.view());
    }
    return *this;
  }

  LogPayload& operator=(LogPayload&& other) noexcept {
    if (this != &other) {
      MoveFrom(std::move(other));
    }
    return *this;
  }

  LogPayload& operator=(std::string_view value) {
    assign(value);
    return *this;
  }

  LogPayload& operator=(const std::string& value) {
    assign(value);
    return *this;
  }

  LogPayload& operator=(const char* value) {
    if (value == nullptr) {
      clear();
    } else {
      assign(std::string_view(value));
    }
    return *this;
  }

  void clear() {
    size_ = 0;
    data_[0] = '\0';
  }

  bool empty() const {
    return size_ == 0;
  }

  std::size_t size() const {
    return size_;
  }

  const char* data() const {
    return data_;
  }

  std::string_view view() const {
    return std::string_view(data_, size_);
  }

  std::string str() const {
    return std::string(view());
  }

  bool is_inline() const {
    return data_ == inline_storage_.data();
  }

  void assign(std::string_view value) {
    EnsureCapacity(value.size());
    if (!value.empty()) {
      std::memcpy(data_, value.data(), value.size());
    }
    size_ = value.size();
    data_[size_] = '\0';
  }

  void append(std::string_view value) {
    if (value.empty()) {
      return;
    }
    EnsureCapacity(size_ + value.size());
    std::memcpy(data_ + size_, value.data(), value.size());
    size_ += value.size();
    data_[size_] = '\0';
  }

  void append(const std::string& value) {
    append(std::string_view(value));
  }

  void append(const char* value) {
    if (value != nullptr) {
      append(std::string_view(value));
    }
  }

  void push_back(char value) {
    EnsureCapacity(size_ + 1);
    data_[size_] = value;
    ++size_;
    data_[size_] = '\0';
  }

private:
  void ResetInline() {
    heap_storage_.reset();
    data_ = inline_storage_.data();
    capacity_ = kInlineCapacity;
    size_ = 0;
    inline_storage_[0] = '\0';
  }

  void MoveFrom(LogPayload&& other) {
    if (other.is_inline()) {
      ResetInline();
      assign(other.view());
    } else {
      heap_storage_ = std::move(other.heap_storage_);
      data_ = heap_storage_.get();
      capacity_ = other.capacity_;
      size_ = other.size_;
      other.ResetInline();
    }
  }

  void EnsureCapacity(std::size_t required_size) {
    if (required_size <= capacity_) {
      return;
    }

    std::size_t new_capacity = std::max(capacity_ * 2, required_size);
    auto storage = std::make_unique<char[]>(new_capacity + 1);
    if (size_ != 0) {
      std::memcpy(storage.get(), data_, size_);
    }
    storage[size_] = '\0';

    heap_storage_ = std::move(storage);
    data_ = heap_storage_.get();
    capacity_ = new_capacity;
  }

  std::array<char, kInlineCapacity + 1> inline_storage_{};
  std::unique_ptr<char[]> heap_storage_;
  char* data_ = inline_storage_.data();
  std::size_t size_ = 0;
  std::size_t capacity_ = kInlineCapacity;
};

inline std::ostream& operator<<(std::ostream& output, const LogPayload& payload) {
  return output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

}  // namespace hlog
