cmake_minimum_required(VERSION 3.20)
project(hlog_find_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(hlog CONFIG REQUIRED)

add_executable(hlog_find_package_consumer main.cpp)
target_link_libraries(hlog_find_package_consumer PRIVATE HLog::hlog)
