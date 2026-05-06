# Resume Kit

## GitHub Repo Description

Chinese:

高性能 C++20 异步日志库，基于 CAS 无锁环形队列实现低开销并发日志写入。

English:

High-performance C++20 async logger using a CAS-based lock-free ring buffer for low-overhead concurrent logging.

## GitHub Topics

Recommended topics:

- `cpp`
- `cpp20`
- `logging`
- `async-logging`
- `lock-free`
- `concurrency`
- `multithreading`
- `ring-buffer`
- `cas`
- `cmake`

## Chinese Project Introduction

### One-line

参考 `spdlog` 独立实现的 C++20 高性能异步日志库，基于 CAS 无锁环形队列优化高并发日志写入性能。

### Short paragraph

该项目是一个面向高并发场景的 C++20 异步日志库，采用 `logger + sink + background worker` 分层架构，支持日志分级、异步落盘、自定义输出端和阻塞/丢弃两种队列背压策略。核心数据通道使用基于 CAS 的有界无锁环形缓冲区，结合 `atomic::wait/notify` 降低多线程日志写入时的锁竞争；并通过与 `mutex + condition_variable` 基线的对照 benchmark 验证吞吐提升。

## English Project Introduction

### One-line

A C++20 high-performance async logger inspired by `spdlog`, optimized with a CAS-based lock-free ring buffer for concurrent logging.

### Short paragraph

This project is a C++20 asynchronous logging library built for high-concurrency workloads. It uses a layered `logger + sink + background worker` architecture and supports log levels, asynchronous flushing, custom sinks, and both blocking and drop-newest overflow policies. Its core queue is a bounded lock-free ring buffer implemented with CAS and coordinated with `atomic::wait/notify`, and the repository includes a benchmark against a `mutex + condition_variable` async baseline to show measurable throughput gains.

## Resume Bullets

### Chinese

- 参考 `spdlog` 独立实现 C++20 高性能异步日志库，采用 `logger + sink + background worker` 分层架构，支持日志分级、异步落盘和自定义输出端。
- 基于 CAS 设计有界无锁环形队列，结合 `atomic::wait/notify` 替代 `mutex/condition_variable`，解决多线程日志写入中的锁竞争问题，并通过单消费者模型保证落盘线程安全。
- 以 `mutex + condition_variable + deque` 为基线完成对照压测，在 `8` 线程 `160000` 条日志写入场景下，日志写入吞吐由约 `4.73e5 msg/s` 提升至约 `3.09e6 msg/s`，提升约 `554%`。

### English

- Built a C++20 high-performance asynchronous logger inspired by `spdlog`, using a layered `logger + sink + background worker` architecture with log levels, async flushing, and pluggable sinks.
- Implemented a bounded lock-free ring buffer with CAS plus `atomic::wait/notify` to replace a `mutex/condition_variable` queue, reducing contention on the concurrent logging hot path while keeping sink writes thread-safe through a single-consumer model.
- Benchmarked the design against a `mutex + condition_variable + deque` async baseline; in one `8-thread / 160000-message` run, throughput improved from about `4.73e5 msg/s` to `3.09e6 msg/s`, a gain of about `554%`.

## One-Minute Interview Pitch

Chinese:

这个项目的出发点是同步日志在高并发场景下很容易因为共享锁成为瓶颈。我参考 `spdlog` 做了一个独立的 C++20 异步日志库，把整体拆成 `logger + sink + worker` 三层。核心优化点是把传统的 `mutex + condition_variable` 队列换成了基于 CAS 的有界无锁环形队列，并用 `atomic::wait/notify` 做线程唤醒；这样生产者线程只负责格式化和入队，后台线程顺序落盘，既减少了锁竞争，也保证了 sink 的线程安全。为了证明优化有效，我又实现了一个同模型的 mutex 基线 benchmark，在 `8` 线程 `160000` 条日志写入场景下，吞吐从大约 `4.73e5 msg/s` 提升到 `3.09e6 msg/s`。

English:

The project started from a simple observation: synchronous logging becomes a bottleneck under contention because producer threads serialize on a shared lock. I built a standalone C++20 asynchronous logger with a layered `logger + sink + worker` design. The main optimization was replacing a traditional `mutex + condition_variable` queue with a bounded CAS-based lock-free ring buffer, plus `atomic::wait/notify` for thread coordination. That lets producer threads focus on formatting and enqueueing while a background thread performs ordered sink writes. To make the result measurable, I also implemented a mutex-based async baseline benchmark; in one `8-thread / 160000-message` run, throughput improved from about `4.73e5 msg/s` to `3.09e6 msg/s`.
