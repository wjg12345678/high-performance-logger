#include "hlog/pattern_formatter.h"

#include "hlog/log_level.h"

#include <charconv>
#include <chrono>
#include <ctime>
#include <string_view>
#include <utility>

namespace hlog {

namespace {

struct TimestampParts {
  std::tm local_time{};
  int millisecond = 0;
};

TimestampParts MakeTimestampParts(std::chrono::system_clock::time_point timestamp) {
  TimestampParts parts;
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      timestamp.time_since_epoch()) %
      1000;
  parts.millisecond = static_cast<int>(millis.count());

  const std::time_t seconds = std::chrono::system_clock::to_time_t(timestamp);
#if defined(_WIN32)
  localtime_s(&parts.local_time, &seconds);
#else
  localtime_r(&seconds, &parts.local_time);
#endif
  return parts;
}

template <typename Integer>
void AppendInteger(std::string& output, Integer value, int width = 0) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) {
    return;
  }

  const int digits = static_cast<int>(end - buffer);
  for (int index = digits; index < width; ++index) {
    output.push_back('0');
  }
  output.append(buffer, static_cast<std::size_t>(digits));
}

std::string_view BaseName(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

PatternFormatter::PatternFormatter()
    : pattern_(DefaultPattern()) {
  RebuildTokens();
}

PatternFormatter::PatternFormatter(std::string pattern)
    : pattern_(std::move(pattern)) {
  RebuildTokens();
}

void PatternFormatter::SetPattern(std::string pattern) {
  pattern_ = std::move(pattern);
  RebuildTokens();
}

std::string PatternFormatter::Format(const LogMessage& message) const {
  std::string output;
  output.reserve(message.payload.size() + pattern_.size() + 64);
  FormatTo(output, message);
  return output;
}

void PatternFormatter::FormatTo(std::string& output, const LogMessage& message) const {
  const auto is_time_token = [](TokenType type) {
    switch (type) {
      case TokenType::Year:
      case TokenType::Month:
      case TokenType::Day:
      case TokenType::Hour:
      case TokenType::Minute:
      case TokenType::Second:
      case TokenType::Millisecond:
        return true;
      default:
        return false;
    }
  };

  bool has_time = false;
  TimestampParts timestamp_parts{};

  for (const auto& token : tokens_) {
    if (token.type == TokenType::Literal) {
      output.append(token.literal);
      continue;
    }

    if (is_time_token(token.type) && !has_time) {
      timestamp_parts = MakeTimestampParts(message.timestamp);
      has_time = true;
    }

    switch (token.type) {
      case TokenType::Literal:
        break;
      case TokenType::Year:
        AppendInteger(output, timestamp_parts.local_time.tm_year + 1900, 4);
        break;
      case TokenType::Month:
        AppendInteger(output, timestamp_parts.local_time.tm_mon + 1, 2);
        break;
      case TokenType::Day:
        AppendInteger(output, timestamp_parts.local_time.tm_mday, 2);
        break;
      case TokenType::Hour:
        AppendInteger(output, timestamp_parts.local_time.tm_hour, 2);
        break;
      case TokenType::Minute:
        AppendInteger(output, timestamp_parts.local_time.tm_min, 2);
        break;
      case TokenType::Second:
        AppendInteger(output, timestamp_parts.local_time.tm_sec, 2);
        break;
      case TokenType::Millisecond:
        AppendInteger(output, timestamp_parts.millisecond, 3);
        break;
      case TokenType::Level:
        output.append(ToString(message.level));
        break;
      case TokenType::LoggerName:
        output.append(message.logger_name.data(), message.logger_name.size());
        break;
      case TokenType::ThreadId:
        AppendInteger(output, message.thread_id);
        break;
      case TokenType::Payload:
        output.append(message.payload.data(), message.payload.size());
        break;
      case TokenType::SourceFile: {
        const std::string_view file_name = BaseName(message.source.file);
        output.append(file_name.data(), file_name.size());
        break;
      }
      case TokenType::SourceLine:
        if (message.source.line > 0) {
          AppendInteger(output, message.source.line);
        }
        break;
      case TokenType::SourceFunction:
        output.append(message.source.function.data(), message.source.function.size());
        break;
    }
  }
}

std::string PatternFormatter::DefaultPattern() {
  return std::string(kDefaultLogPattern);
}

void PatternFormatter::RebuildTokens() {
  tokens_.clear();
  const auto compile_pattern =
      [&](const auto& self, std::string_view pattern, bool allow_default_expansion) -> void {
    std::string literal;
    auto flush_literal = [&]() {
      if (literal.empty()) {
        return;
      }
      tokens_.push_back(Token{TokenType::Literal, std::move(literal)});
      literal.clear();
    };

    for (std::size_t index = 0; index < pattern.size(); ++index) {
      const char current = pattern[index];
      if (current != '%') {
        literal.push_back(current);
        continue;
      }

      if (index + 1 >= pattern.size()) {
        literal.push_back('%');
        break;
      }

      const char specifier = pattern[++index];
      auto push_token = [&](TokenType type) {
        flush_literal();
        tokens_.push_back(Token{type, {}});
      };

      switch (specifier) {
        case '%':
          literal.push_back('%');
          break;
        case '+':
          flush_literal();
          if (allow_default_expansion) {
            self(self, kDefaultLogPattern, false);
          } else {
            literal.append("%+");
          }
          break;
        case 'Y':
          push_token(TokenType::Year);
          break;
        case 'm':
          push_token(TokenType::Month);
          break;
        case 'd':
          push_token(TokenType::Day);
          break;
        case 'H':
          push_token(TokenType::Hour);
          break;
        case 'M':
          push_token(TokenType::Minute);
          break;
        case 'S':
          push_token(TokenType::Second);
          break;
        case 'e':
          push_token(TokenType::Millisecond);
          break;
        case 'l':
          push_token(TokenType::Level);
          break;
        case 'n':
          push_token(TokenType::LoggerName);
          break;
        case 't':
          push_token(TokenType::ThreadId);
          break;
        case 'v':
          push_token(TokenType::Payload);
          break;
        case 's':
          push_token(TokenType::SourceFile);
          break;
        case '#':
          push_token(TokenType::SourceLine);
          break;
        case '!':
          push_token(TokenType::SourceFunction);
          break;
        default:
          literal.push_back('%');
          literal.push_back(specifier);
          break;
      }
    }

    flush_literal();
  };

  compile_pattern(compile_pattern, pattern_, true);
}

}  // namespace hlog
