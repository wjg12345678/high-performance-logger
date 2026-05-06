#pragma once

#include "hlog/sink.h"

#include <fstream>
#include <string>

namespace hlog {

class FileSink final : public Sink {
public:
  explicit FileSink(const std::string& path, bool truncate = false);

  void Write(const LogMessage& message) override;
  void Flush() override;

private:
  std::ofstream output_;
};

}  // namespace hlog
