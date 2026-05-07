#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace hlog_examples {

inline std::filesystem::path ArtifactRoot() {
  if (const char* configured = std::getenv("HLOG_ARTIFACT_DIR");
      configured != nullptr && configured[0] != '\0') {
    return std::filesystem::path(configured);
  }
  return std::filesystem::path("out");
}

inline std::filesystem::path RuntimeDir() {
  const std::filesystem::path directory = ArtifactRoot() / "runtime";
  std::filesystem::create_directories(directory);
  return directory;
}

inline std::string RuntimeLogPath(std::string_view file_name) {
  return (RuntimeDir() / std::string(file_name)).string();
}

}  // namespace hlog_examples
