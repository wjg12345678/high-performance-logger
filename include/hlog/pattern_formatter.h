#pragma once

#include "hlog/detail/log_message.h"

#include <string>
#include <string_view>
#include <vector>

namespace hlog {

inline constexpr char kDefaultLogPattern[] = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] [tid=%t] %v";

class PatternFormatter {
public:
  PatternFormatter();
  explicit PatternFormatter(std::string pattern);

  void SetPattern(std::string pattern);

  [[nodiscard]] const std::string& pattern() const noexcept {
    return pattern_;
  }

  [[nodiscard]] std::string Format(const LogMessage& message) const;
  void FormatTo(std::string& output, const LogMessage& message) const;

  [[nodiscard]] static std::string DefaultPattern();

private:
  enum class TokenType {
    Literal,
    Year,
    Month,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Level,
    LoggerName,
    ThreadId,
    Payload,
    SourceFile,
    SourceLine,
    SourceFunction,
  };

  struct Token {
    TokenType type = TokenType::Literal;
    std::string literal;
  };

  void RebuildTokens();

  std::string pattern_;
  std::vector<Token> tokens_;
};

}  // namespace hlog
