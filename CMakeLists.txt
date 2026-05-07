cmake_minimum_required(VERSION 3.20)
project(high_performance_logger VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HLOG_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(HLOG_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(HLOG_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(HLOG_BUILD_EXAMPLES "Build examples and benchmark executables" ON)
option(
  HLOG_BUILD_EXTERNAL_BENCHMARKS
  "Build optional benchmarks against external logging libraries when dependencies are available"
  ON
)

if (HLOG_ENABLE_TSAN AND HLOG_ENABLE_ASAN)
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
endif()

if (HLOG_ENABLE_TSAN AND HLOG_ENABLE_UBSAN)
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with UndefinedBehaviorSanitizer in this project")
endif()

add_library(hlog_build_options INTERFACE)

if (MSVC)
  target_compile_options(hlog_build_options INTERFACE /W4 /permissive-)
else()
  target_compile_options(hlog_build_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if (HLOG_ENABLE_ASAN)
    target_compile_options(hlog_build_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(hlog_build_options INTERFACE -fsanitize=address)
  endif()

  if (HLOG_ENABLE_TSAN)
    target_compile_options(hlog_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(hlog_build_options INTERFACE -fsanitize=thread)
  endif()

  if (HLOG_ENABLE_UBSAN)
    target_compile_options(hlog_build_options INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(hlog_build_options INTERFACE -fsanitize=undefined)
  endif()
elseif (HLOG_ENABLE_ASAN OR HLOG_ENABLE_TSAN OR HLOG_ENABLE_UBSAN)
  message(FATAL_ERROR "Sanitizers are only supported with Clang or GCC")
endif()

add_library(hlog
  src/async_logger.cpp
  src/console_sink.cpp
  src/file_sink.cpp
  src/logger_config.cpp
  src/pattern_formatter.cpp
  src/rotating_file_sink.cpp
)

add_library(HLog::hlog ALIAS hlog)

target_compile_features(hlog PUBLIC cxx_std_20)
target_include_directories(
  hlog
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(hlog PRIVATE "$<BUILD_INTERFACE:hlog_build_options>")

if (HLOG_BUILD_EXAMPLES)
  add_executable(hlog_example examples/basic_example.cpp)
  target_link_libraries(hlog_example PRIVATE hlog hlog_build_options)

  add_executable(hlog_service_example examples/service_example.cpp)
  target_link_libraries(hlog_service_example PRIVATE hlog hlog_build_options)

  add_executable(hlog_benchmark examples/benchmark_main.cpp)
  target_link_libraries(hlog_benchmark PRIVATE hlog hlog_build_options)

  add_executable(hlog_compare_benchmark examples/compare_benchmark.cpp)
  target_link_libraries(hlog_compare_benchmark PRIVATE hlog hlog_build_options)

  add_executable(hlog_latency_benchmark examples/latency_benchmark.cpp)
  target_link_libraries(hlog_latency_benchmark PRIVATE hlog hlog_build_options)

  add_executable(hlog_payload_benchmark examples/payload_benchmark.cpp)
  target_link_libraries(hlog_payload_benchmark PRIVATE hlog hlog_build_options)

  add_executable(hlog_file_sink_benchmark examples/file_sink_benchmark.cpp)
  target_link_libraries(hlog_file_sink_benchmark PRIVATE hlog hlog_build_options)

  if (HLOG_BUILD_EXTERNAL_BENCHMARKS)
    find_package(spdlog CONFIG QUIET)
    if (TARGET spdlog::spdlog_header_only)
      add_executable(hlog_spdlog_compare_benchmark examples/spdlog_compare_benchmark.cpp)
      target_link_libraries(
        hlog_spdlog_compare_benchmark
        PRIVATE
          hlog
          hlog_build_options
          spdlog::spdlog_header_only
      )
    elseif (TARGET spdlog::spdlog)
      add_executable(hlog_spdlog_compare_benchmark examples/spdlog_compare_benchmark.cpp)
      target_link_libraries(
        hlog_spdlog_compare_benchmark
        PRIVATE
          hlog
          hlog_build_options
          spdlog::spdlog
      )
    else()
      message(STATUS "spdlog not found; skipping hlog_spdlog_compare_benchmark")
    endif()
  endif()
endif()

if (BUILD_TESTING)
  add_executable(hlog_async_logger_test tests/async_logger_test.cpp)
  target_link_libraries(hlog_async_logger_test PRIVATE hlog hlog_build_options)

  add_test(NAME hlog_async_logger_test COMMAND hlog_async_logger_test)
endif()

install(
  TARGETS hlog
  EXPORT hlogTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
  cmake/hlogConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/hlogConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hlog
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/hlogConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(
  EXPORT hlogTargets
  FILE hlogTargets.cmake
  NAMESPACE HLog::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hlog
)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hlogConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hlogConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hlog
)
