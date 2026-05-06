cmake_minimum_required(VERSION 3.20)
project(high_performance_logger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hlog
  src/async_logger.cpp
  src/file_sink.cpp
)

target_include_directories(hlog PUBLIC include)

if (MSVC)
  target_compile_options(hlog PRIVATE /W4 /permissive-)
else()
  target_compile_options(hlog PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(hlog_example examples/basic_example.cpp)
target_link_libraries(hlog_example PRIVATE hlog)

add_executable(hlog_benchmark examples/benchmark_main.cpp)
target_link_libraries(hlog_benchmark PRIVATE hlog)

add_executable(hlog_compare_benchmark examples/compare_benchmark.cpp)
target_link_libraries(hlog_compare_benchmark PRIVATE hlog)

enable_testing()

add_executable(hlog_async_logger_test tests/async_logger_test.cpp)
target_link_libraries(hlog_async_logger_test PRIVATE hlog)

add_test(NAME hlog_async_logger_test COMMAND hlog_async_logger_test)
