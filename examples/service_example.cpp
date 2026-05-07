#include "hlog/logger_config.h"
#include "hlog/log_level.h"
#include "runtime_paths.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)

#include <iostream>

int main() {
  std::cerr << "hlog_service_example is only supported on POSIX platforms.\n";
  return 1;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <cstring>
#include <iostream>
#include <sstream>

namespace {

struct AcceptedConnection {
  int fd = -1;
  std::string remote;
};

class ConnectionQueue {
public:
  void Push(AcceptedConnection connection) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(connection));
    }
    condition_.notify_one();
  }

  bool Pop(AcceptedConnection& connection) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() {
      return stopped_ || !queue_.empty();
    });

    if (queue_.empty()) {
      return false;
    }

    connection = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<AcceptedConnection> queue_;
  bool stopped_ = false;
};

struct ServiceConfig {
  std::uint16_t port = 8080;
  int worker_count = 4;
  int max_requests = 0;
  hlog::LoggerConfig logger;
};

class FileDescriptor {
public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}

  ~FileDescriptor() {
    Reset();
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const {
    return fd_;
  }

  int Release() {
    const int released = fd_;
    fd_ = -1;
    return released;
  }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

std::atomic<bool> g_running{true};

void HandleSignal(int) {
  g_running.store(false, std::memory_order_relaxed);
}

std::string GetEnvOrDefault(const char* name, std::string_view fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string(fallback) : std::string(value);
}

template <typename Integer>
Integer ParseInteger(std::string_view text, Integer fallback) {
  Integer value = fallback;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() ? value : fallback;
}

std::string BuildJsonResponse(std::string_view body) {
  return std::string(body);
}

std::string BuildHttpResponse(
    int status_code,
    std::string_view status_text,
    std::string_view content_type,
    std::string_view body) {
  std::ostringstream output;
  output << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body;
  return output.str();
}

std::string PeerAddress(const sockaddr_in& address) {
  char buffer[INET_ADDRSTRLEN];
  if (::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
    return "unknown";
  }
  return std::string(buffer);
}

std::pair<std::string, std::string> ParseRequest(std::string_view request) {
  const std::size_t line_end = request.find("\r\n");
  const std::string_view first_line =
      line_end == std::string_view::npos ? request : request.substr(0, line_end);

  const std::size_t first_space = first_line.find(' ');
  const std::size_t second_space = first_line.find(' ', first_space == std::string_view::npos ? 0 : first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
    return {"", ""};
  }

  return {
      std::string(first_line.substr(0, first_space)),
      std::string(first_line.substr(first_space + 1, second_space - first_space - 1)),
  };
}

void SendAll(int fd, std::string_view response) {
  const char* data = response.data();
  std::size_t remaining = response.size();
  while (remaining > 0) {
    const ssize_t written = ::send(fd, data, remaining, 0);
    if (written <= 0) {
      return;
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

void HandleConnection(
    AcceptedConnection connection,
    hlog::AsyncLogger& logger,
    std::atomic<std::uint64_t>& total_requests) {
  FileDescriptor client(connection.fd);

  char buffer[4096];
  const ssize_t received = ::recv(client.get(), buffer, sizeof(buffer), 0);
  if (received <= 0) {
    logger.Warn("remote=", connection.remote, " recv_error=", std::strerror(errno));
    return;
  }

  const auto [method, path] =
      ParseRequest(std::string_view(buffer, static_cast<std::size_t>(received)));

  ++total_requests;

  int status_code = 200;
  std::string status_text = "OK";
  std::string content_type = "application/json";
  std::string body;

  if (method.empty() || path.empty()) {
    status_code = 400;
    status_text = "Bad Request";
    body = BuildJsonResponse("{\"error\":\"invalid request\"}\n");
  } else if (path == "/healthz" || path == "/readyz") {
    body = BuildJsonResponse("{\"status\":\"ok\"}\n");
  } else if (path == "/metrics") {
    content_type = "text/plain; version=0.0.4";
    body = "service_requests_total " + std::to_string(total_requests.load(std::memory_order_relaxed)) + "\n";
  } else {
    body =
        BuildJsonResponse("{\"service\":\"hlog\",\"message\":\"request processed\"}\n");
  }

  const std::string response = BuildHttpResponse(status_code, status_text, content_type, body);
  SendAll(client.get(), response);

  logger.Info(
      "remote=",
      connection.remote,
      " method=",
      method,
      " path=",
      path,
      " status=",
      status_code,
      " bytes=",
      response.size());
}

ServiceConfig LoadConfig() {
  ServiceConfig config;
  config.port = ParseInteger<std::uint16_t>(GetEnvOrDefault("HLOG_PORT", "8080"), 8080);
  config.worker_count = std::max(1, ParseInteger<int>(GetEnvOrDefault("HLOG_WORKERS", "4"), 4));
  config.max_requests = std::max(0, ParseInteger<int>(GetEnvOrDefault("HLOG_MAX_REQUESTS", "0"), 0));

  const std::string pattern = GetEnvOrDefault("HLOG_PATTERN", hlog::kDefaultLogPattern);

  config.logger.name = "hlog-http-service";
  config.logger.async_options.queue_size =
      static_cast<std::size_t>(std::max(256, ParseInteger<int>(GetEnvOrDefault("HLOG_QUEUE_SIZE", "8192"), 8192)));
  config.logger.async_options.level =
      hlog::ParseLogLevel(GetEnvOrDefault("HLOG_LEVEL", "info"), hlog::LogLevel::Info);
  config.logger.async_options.flush_level =
      hlog::ParseLogLevel(GetEnvOrDefault("HLOG_FLUSH_LEVEL", "error"), hlog::LogLevel::Error);

  hlog::ConsoleSinkConfig console;
  console.options.stream = hlog::ConsoleStream::Stderr;
  console.options.auto_flush = false;
  console.options.pattern = pattern;
  config.logger.sinks.emplace_back(console);

  const std::string log_path =
      GetEnvOrDefault("HLOG_LOG_PATH", hlog_examples::RuntimeLogPath("service.log"));
  if (!log_path.empty()) {
    hlog::RotatingFileSinkConfig file;
    file.path = log_path;
    file.options.pattern = pattern;
    file.options.max_file_size = static_cast<std::size_t>(
        std::max(1024, ParseInteger<int>(GetEnvOrDefault("HLOG_ROTATE_BYTES", "10485760"), 10485760)));
    file.options.max_files =
        static_cast<std::size_t>(std::max(1, ParseInteger<int>(GetEnvOrDefault("HLOG_ROTATE_FILES", "5"), 5)));
    file.options.max_batch_size = static_cast<std::size_t>(
        std::max(1, ParseInteger<int>(GetEnvOrDefault("HLOG_BATCH_BYTES", "65536"), 65536)));
    file.options.flush_interval = std::chrono::milliseconds(
        std::max(0, ParseInteger<int>(GetEnvOrDefault("HLOG_FLUSH_MS", "250"), 250)));
    config.logger.sinks.emplace_back(std::move(file));
  }

  return config;
}

}  // namespace

int main() {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ServiceConfig config = LoadConfig();
  auto logger = hlog::CreateLogger(std::move(config.logger));

  FileDescriptor server(::socket(AF_INET, SOCK_STREAM, 0));
  if (server.get() < 0) {
    std::cerr << "failed to create socket\n";
    return 1;
  }

  const int reuse = 1;
  ::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(config.port);

  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "failed to bind port " << config.port << ": " << std::strerror(errno) << '\n';
    return 1;
  }

  if (::listen(server.get(), 128) != 0) {
    std::cerr << "failed to listen on port " << config.port << ": " << std::strerror(errno) << '\n';
    return 1;
  }

  logger->Info(
      "service_started port=",
      config.port,
      " workers=",
      config.worker_count,
      " max_requests=",
      config.max_requests);

  ConnectionQueue queue;
  std::atomic<std::uint64_t> total_requests{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.worker_count));
  for (int worker = 0; worker < config.worker_count; ++worker) {
    workers.emplace_back([worker, &queue, &logger, &total_requests]() {
      AcceptedConnection connection;
      while (queue.Pop(connection)) {
        HandleConnection(std::move(connection), *logger, total_requests);
      }
      logger->Info("worker_stopped worker=", worker);
    });
  }

  int accepted_requests = 0;
  while (g_running.load(std::memory_order_relaxed) &&
         (config.max_requests == 0 || accepted_requests < config.max_requests)) {
    sockaddr_in client_address{};
    socklen_t client_length = sizeof(client_address);
    const int client_fd =
        ::accept(server.get(), reinterpret_cast<sockaddr*>(&client_address), &client_length);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      logger->Warn("accept_failed errno=", errno, " detail=", std::strerror(errno));
      continue;
    }

    queue.Push(AcceptedConnection{client_fd, PeerAddress(client_address)});
    ++accepted_requests;
  }

  queue.Stop();
  for (auto& worker : workers) {
    worker.join();
  }

  logger->Info(
      "service_stopped accepted_requests=",
      accepted_requests,
      " processed_requests=",
      total_requests.load(std::memory_order_relaxed));
  logger->Flush();
  logger->Stop();
  return 0;
}

#endif
