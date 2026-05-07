#include "hlog/logger_config.h"

#include <filesystem>
#include <fstream>
#include <string>

int main() {
  namespace fs = std::filesystem;

  const fs::path log_path = fs::current_path() / "consumer.log";
  fs::remove(log_path);
  fs::remove(log_path.string() + ".1");

  hlog::LoggerConfig config;
  config.name = "consumer-example";
  config.async_options.queue_size = 256;
  config.async_options.level = hlog::LogLevel::Info;
  config.async_options.flush_level = hlog::LogLevel::Error;

  hlog::ConsoleSinkConfig console;
  config.sinks.emplace_back(console);

  hlog::RotatingFileSinkConfig file;
  file.path = log_path.string();
  file.options.truncate_on_open = true;
  file.options.max_file_size = 1024 * 1024;
  file.options.max_files = 1;
  config.sinks.emplace_back(std::move(file));

  auto logger = hlog::CreateLogger(std::move(config));
  logger->Info("find_package smoke test");
  logger->Flush();
  logger->Stop();

  std::ifstream input(log_path);
  std::string line;
  if (!std::getline(input, line)) {
    return 1;
  }
  return line.find("find_package smoke test") == std::string::npos ? 1 : 0;
}
