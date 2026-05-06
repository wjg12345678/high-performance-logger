# High Performance Logger

一个独立实现的 C++20 高性能异步日志库，设计思路参考 `spdlog`，但把热路径上的互斥锁替换成了基于 CAS 的无锁环形队列。

## 设计目标

- 异步写入：业务线程只负责构造日志并入队，后台线程统一落盘
- 多线程安全：支持多生产者并发写日志、单消费者顺序刷盘
- 日志分级：支持 `trace/debug/info/warn/error/critical/off`
- 低锁开销：入队/出队使用 CAS 原子操作，不在热路径上使用 `std::mutex`
- 可控背压：队列满时支持阻塞等待或丢弃新日志两种策略

## 架构

```text
producer threads
      |
      v
AsyncLogger
  - level filter
  - CAS ring buffer
  - atomic wake/flush
      |
      v
background worker
      |
      v
Sink (FileSink / custom sink)
```

核心组件：

- `hlog::LockFreeRingBuffer<T>`：基于每个槽位的 sequence number 实现有界无锁队列
- `hlog::AsyncLogger`：对外提供日志 API、等级过滤、异步消费和 flush 协调
- `hlog::Sink`：抽象输出端
- `hlog::FileSink`：后台线程独占文件句柄，顺序写盘

## 关键实现点

### 1. CAS 无锁队列

- 使用类似 Dmitry Vyukov bounded queue 的思路
- `enqueue_pos_ / dequeue_pos_` 通过 `compare_exchange_weak` 抢占槽位
- 每个槽位维护独立 `sequence`，避免 ABA 式误判和额外锁竞争

### 2. 异步唤醒

- 生产者入队后递增 `wake_signal_`
- 消费线程用 `atomic::wait/notify_one` 休眠与唤醒
- 相比 `condition_variable + mutex`，少一次显式加锁路径

### 3. Flush 同步

- `Flush()` 会投递一个控制消息
- 后台线程完成刷盘后更新 `flush_complete_ticket_`
- 调用线程通过原子等待拿到 flush 完成信号

## 构建

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 运行示例

```bash
./build/hlog_example
./build/hlog_benchmark
./build/hlog_compare_benchmark
```

日志默认输出到工程目录下的 `example.log` 和 `benchmark.log`。

## 性能对比

`hlog_compare_benchmark` 使用同样的异步模型和同样的内存 sink，对比两种队列实现：

- `mutex_async_logger`：`std::mutex + std::condition_variable + std::deque`
- `cas_async_logger`：CAS 无锁环形队列 + `atomic::wait/notify`

示例命令：

```bash
./build/hlog_compare_benchmark 8 20000
```

我在当前机器上的一次样例结果：

- `mutex_async_logger`：约 `4.73e5 msg/s`
- `cas_async_logger`：约 `3.09e6 msg/s`
- 吞吐提升：约 `554%`

说明：

- 这个对比刻意去掉磁盘 I/O 干扰，主要观察并发入队和线程唤醒开销
- 绝对数值依赖机器配置和编译选项，但趋势能反映锁竞争和扩展性差异

## 简历可写表述

- 参考 `spdlog` 独立实现 C++20 高性能异步日志库，设计 `logger + sink + worker thread` 分层架构，支持多线程安全、日志分级、异步落盘与自定义输出端
- 使用基于 CAS 的无锁环形队列替代互斥锁队列，结合 `atomic::wait/notify` 实现低开销线程通信，降低高并发日志写入路径上的锁竞争
- 设计阻塞/丢弃两种队列满载策略，并补充并发测试与基准程序验证吞吐和正确性
