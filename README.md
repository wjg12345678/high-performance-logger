# High Performance Logger

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake](https://img.shields.io/badge/build-CMake-064F8C)
![Async Logging](https://img.shields.io/badge/design-async%20logging-0A7E8C)
![Lock Free Queue](https://img.shields.io/badge/concurrency-lock--free%20queue-BF5B04)

A C++20 high-performance asynchronous logging library inspired by `spdlog`, built around a CAS-based lock-free ring buffer instead of a `mutex`-protected queue.

一个参考 `spdlog` 设计实现的 C++20 高性能异步日志库，核心热路径使用基于 CAS 的无锁环形队列替代 `mutex` 队列，重点解决高并发日志写入下的锁竞争问题。

## Resume Snapshot

- Built a C++20 asynchronous logger with a layered `logger + sink + background worker` architecture, supporting log levels, async flushing, and pluggable sinks.
- Replaced a `mutex + condition_variable` queue with a CAS-based bounded lock-free ring buffer and `atomic::wait/notify`, reducing contention on the logging hot path.
- Benchmarked against a mutex-based async baseline on the same workload; in one `8-thread / 160000-message` run, throughput improved from about `4.73e5 msg/s` to `3.09e6 msg/s`, a gain of about `554%`.

## Why This Project

Synchronous logging is easy to write but becomes a bottleneck under contention:

- producer threads serialize on a shared lock
- queue nodes trigger repeated allocations
- frequent wakeups add coordination overhead
- log spikes can push both latency and memory usage upward

This project isolates those costs and optimizes the hot path with a bounded ring buffer, a single consumer thread, and atomic wake/flush coordination.

## Core Features

- Asynchronous write path: producer threads only format and enqueue log messages; a background worker performs ordered sink writes.
- Multi-thread safety: supports multiple concurrent producers and a single consumer sink thread.
- Log levels: `trace/debug/info/warn/error/critical/off`.
- Low-overhead concurrency: enqueue/dequeue use CAS instead of `std::mutex` on the hot path.
- Overflow handling: supports `Block` and `DropNewest` policies when the queue is full.
- Flush coordination: supports explicit `Flush()` with atomic ticket-based synchronization.

## Architecture

```text
producer threads
      |
      v
AsyncLogger
  - level filter
  - payload formatting
  - CAS ring buffer
  - atomic wake/flush
      |
      v
background worker
      |
      v
Sink (FileSink / custom sink)
```

Key components:

- `hlog::LockFreeRingBuffer<T>`: bounded MPSC-style queue using per-slot sequence numbers.
- `hlog::AsyncLogger`: public logging API, level filtering, async publish, flush, and statistics.
- `hlog::Sink`: sink abstraction for output destinations.
- `hlog::FileSink`: single-consumer ordered file writer.

## Design Decisions

### 1. Why a bounded ring buffer

- Pre-allocates storage to avoid per-message node allocation in the hot path.
- Keeps memory usage bounded during traffic spikes.
- Uses power-of-two capacity so index wrapping can use bit masking instead of modulo.

### 2. Why CAS instead of mutex

- Multiple producers compete only on atomic cursor updates, not a shared critical section.
- Reduces convoying under contention compared with `std::mutex + std::condition_variable`.
- Sequence numbers on each slot distinguish ready/full states without extra locks.

### 3. Why a single sink consumer

- File writes remain ordered.
- Sink implementations avoid extra write-side locking.
- The concurrency problem is isolated to the queue rather than mixing queue contention with sink contention.

### 4. How thread safety is handled

- Producers publish `QueueItem` objects through `compare_exchange_weak`.
- The worker drains the queue and owns sink writes.
- `Flush()` sends a control message and waits on an atomic completion ticket.
- Wakeups use `atomic::wait/notify` rather than a separate condition-variable lock path.

## Performance Comparison

`hlog_compare_benchmark` compares two async designs under the same workload and with the same in-memory sink:

- `mutex_async_logger`: `std::mutex + std::condition_variable + std::deque`
- `cas_async_logger`: CAS lock-free ring buffer + `atomic::wait/notify`

Sample command:

```bash
./build/hlog_compare_benchmark 8 20000
```

Sample result on the current machine:

| Implementation | Throughput |
| --- | ---: |
| `mutex_async_logger` | `4.73e5 msg/s` |
| `cas_async_logger` | `3.09e6 msg/s` |
| Improvement | `+554%` |

Notes:

- This benchmark intentionally uses an in-memory sink to isolate queueing and synchronization overhead from disk I/O.
- Absolute numbers depend on CPU, compiler, and optimization flags; the important signal is the scalability trend under contention.

## Repository Layout

```text
include/hlog/
  async_logger.h
  file_sink.h
  lock_free_ring_buffer.h
  log_level.h
  log_message.h
  sink.h
src/
  async_logger.cpp
  file_sink.cpp
examples/
  basic_example.cpp
  benchmark_main.cpp
  compare_benchmark.cpp
tests/
  async_logger_test.cpp
docs/
  resume-kit.md
```

## Build And Run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Examples:

```bash
./build/hlog_example
./build/hlog_benchmark
./build/hlog_compare_benchmark
```

Generated logs are written to `example.log` and `benchmark.log`.

## Tests

The test suite covers:

- level filtering correctness
- concurrent asynchronous writes
- overflow behavior under `DropNewest`

Run:

```bash
ctest --test-dir build --output-on-failure
```

## Chinese Project Introduction

这是一个面向高并发场景的 C++20 异步日志库，设计上参考 `spdlog`，但针对日志热路径做了更聚焦的并发优化。项目使用基于 CAS 的有界无锁环形队列承接多线程日志写入，通过后台线程异步刷盘，并提供日志分级、阻塞/丢弃两种背压策略以及显式 `Flush()` 同步机制。为了验证优化是否真实有效，项目额外实现了与 `mutex + condition_variable` 异步队列的同模型对照 benchmark，在 `8` 线程 `160000` 条日志写入场景下，吞吐从约 `4.73e5 msg/s` 提升到约 `3.09e6 msg/s`。

## English Project Introduction

This project is a C++20 asynchronous logging library for high-concurrency workloads. Inspired by `spdlog`, it focuses on optimizing the logging hot path with a CAS-based bounded lock-free ring buffer, a background sink thread, log-level filtering, explicit flush coordination, and configurable overflow policies. To make the optimization measurable rather than anecdotal, the repository also includes a like-for-like benchmark against a `mutex + condition_variable` async baseline. In one `8-thread / 160000-message` run on the current machine, throughput improved from roughly `4.73e5 msg/s` to `3.09e6 msg/s`.

## Resume / Interview Materials

Shorter resume-ready and interview-ready descriptions are collected in [docs/resume-kit.md](docs/resume-kit.md).
