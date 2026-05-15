# HLog 项目学习指南

这份文档用于把 `high-performance-logger` 学成一个能上简历、能讲细节、能经得住追问的 C++ 项目。学习重点不是“会调用日志库”，而是理解为什么异步日志能降低业务线程开销、无锁队列如何工作、后台线程如何安全退出、flush 怎么保证完成、payload 热路径为什么能优化，以及 benchmark 结论应该怎么严谨表达。

## 1. 项目定位

HLog 是一个面向高并发场景的 C++20 异步日志库。它的核心是：

- 业务线程只做日志级别判断、payload 构造和入队。
- 后台线程独占消费队列并写入 sink。
- producer 与 consumer 之间用基于 CAS 的有界无锁环形队列连接。
- worker 唤醒使用 `condition_variable + wake_signal`，flush ticket 等待使用 C++20 原子等待。
- 通过 inline payload、`std::to_chars`、thread local 线程 ID、FileSink staging buffer 优化热路径。

可以这样介绍：

> HLog 是我实现的 C++20 高性能异步日志库。它采用 Logger + Sink + 后台消费线程架构，业务线程把日志写入基于 CAS 的有界无锁环形队列，后台线程顺序 drain 并写入文件、控制台、轮转文件或多个 sink。项目重点优化多线程日志写入中的锁竞争、payload 构造、线程唤醒和文件批量写路径，并提供 benchmark、sanitizer、install/export 和 find_package 示例。

## 2. 要主动讲清楚的边界

它是一个可复用的 C++ 日志库项目，但不要把它包装成完整替代所有工业日志生态的生产级框架。

当前已实现：

- 异步 logger。
- 多生产者单消费者模型。
- CAS 有界无锁队列。
- `Block` / `DropNewest` 满队列策略。
- `Flush()` 同步刷盘。
- `Stop()` 安全停止后台线程。
- 日志级别过滤。
- source location 宏。
- `FileSink`、`ConsoleSink`、`RotatingFileSink`、`MultiSink`。
- `PatternFormatter` 和 `LoggerConfig`。
- inline payload 和热路径优化。
- benchmark 和 CI 验证。
- 安装导出和 `find_package` consumer。

还可以继续生产化的点：

- 更完整的 pattern 语法。
- 异常处理策略可配置。
- 日志文件压缩、清理、保留策略。
- 多 consumer 或分片队列。
- 跨进程日志汇聚。
- 动态配置热更新。
- 结构化 JSON 日志。
- 异步错误回调和监控指标。
- 更严格的崩溃场景持久化策略。

## 3. 仓库结构

```text
include/hlog/
  async_logger.h              异步日志器 API、配置、统计、热路径模板
  detail/lock_free_ring_buffer.h 基于槽位序号的有界无锁队列
  log_payload.h               256B inline payload，小日志避免堆分配
  log_message.h               日志消息结构和 source location
  log_level.h                 日志等级
  sink.h                      sink 抽象接口
  console_sink.h              控制台输出
  file_sink.h                 文件输出和批量写配置
  rotating_file_sink.h        文件轮转输出
  multi_sink.h                多 sink fan-out
  pattern_formatter.h         格式化模板
  logger_config.h             配置装配层

src/
  async_logger.cpp            后台线程、Publish、Flush、Stop
  file_sink.cpp               文件 staging buffer 和 flush
  rotating_file_sink.cpp      文件轮转
  console_sink.cpp            控制台 sink
  pattern_formatter.cpp       格式化实现
  logger_config.cpp           配置解析/构建

examples/
  basic_example.cpp           基础使用示例
  service_example.cpp         服务化示例
  benchmark_main.cpp          吞吐 benchmark
  compare_benchmark.cpp       mutex 队列 vs lock-free 队列
  latency_benchmark.cpp       producer 调用延迟
  payload_benchmark.cpp       payload 热路径对比
  file_sink_benchmark.cpp     文件写路径 benchmark
  spdlog_compare_benchmark.cpp 可选 spdlog 对比
  find_package_consumer/      安装导出验证示例

tests/
  async_logger_test.cpp       异步行为、flush、drop、sink 等测试

docs/
  perf.md                     性能报告
  简历材料.md                 简历材料
```

## 4. 构建和验证

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

常用 benchmark：

```bash
./build/hlog_benchmark 8 20000 5 1
./build/hlog_compare_benchmark 8 20000 5 1
./build/hlog_latency_benchmark 8 20000 5 1
./build/hlog_payload_benchmark 8 20000 5 1
./build/hlog_file_sink_benchmark 8 1000 2 0
```

如果本机安装了 spdlog，CMake 会构建可选对比：

```bash
./build/hlog_spdlog_compare_benchmark 8 20000 5 1
```

安装导出验证：

```bash
cmake --install build --prefix /tmp/hlog-install
./scripts/install_smoke_test.sh /tmp/hlog-install
```

## 5. 基本使用

典型使用方式：

```cpp
#include "hlog/async_logger.h"
#include "hlog/file_sink.h"

auto sink = std::make_unique<hlog::FileSink>("app.log", true);
hlog::AsyncLogger logger("app", std::move(sink));

logger.Info("server started port=", 8080);
HLOG_WARN(logger, "slow request id=", request_id, " cost_ms=", cost);
logger.Flush();
logger.Stop();
```

注意：

- `Info("a=", value)` 是 variadic API，会走模板拼接。
- `HLOG_WARN` 这类宏会携带 `__FILE__`、`__LINE__`、`__func__`。
- `Flush()` 等待后台线程处理 flush 控制消息。
- 析构时会调用 `Stop()`。

## 6. 整体架构

```text
Producer threads
  |
  | level check
  | build LogPayload
  | fill LogMessage
  v
AsyncLogger::Publish
  |
  v
LockFreeRingBuffer<QueueItem>
  |
  | wake_signal + condition_variable
  v
worker thread
  |
  | TryDequeue loop
  | HandleItem
  v
Sink
  |
  +-- ConsoleSink
  +-- FileSink
  +-- RotatingFileSink
  +-- MultiSink
```

关键设计是“多生产者 + 单消费者”：

- 多个业务线程可以并发写日志。
- 后台线程一个人消费队列。
- sink 主要由后台线程访问，避免文件写路径上的多线程竞争。

## 7. AsyncLogger 核心 API

看 `include/hlog/async_logger.h`。

### 配置项

```cpp
struct AsyncLoggerOptions {
  std::size_t queue_size = 8192;
  OverflowPolicy overflow_policy = OverflowPolicy::Block;
  LogLevel level = LogLevel::Info;
  LogLevel flush_level = LogLevel::Error;
};
```

- `queue_size`：队列容量，会在 ring buffer 内部向上取 2 的幂。
- `overflow_policy`：队列满时阻塞或丢弃新日志。
- `level`：日志过滤等级。
- `flush_level`：达到该等级时后台线程主动 flush sink。

### 统计项

```cpp
struct LoggerStats {
  std::uint64_t enqueued;
  std::uint64_t dropped;
  std::uint64_t written;
  std::uint64_t pending;
};
```

统计指标可以回答：

- 入队了多少。
- 丢了多少。
- 实际写出了多少。
- 队列里估算还积压多少。

### 日志接口

`Trace / Debug / Info / Warn / Error / Critical` 都是模板函数，最终调用私有 `Log()`。

带 source location 的接口：

- `TraceAt`
- `DebugAt`
- `InfoAt`
- `WarnAt`
- `ErrorAt`
- `CriticalAt`

宏 `HLOG_INFO(logger, ...)` 会把源码位置传进去。

## 8. Log 热路径

`Log()` 是 producer 侧核心路径。

它做的事情：

1. 创建 `OperationGuard`，记录当前有一个活跃 producer 操作。
2. 检查 logger 是否仍在运行。
3. 做日志级别过滤。
4. 填充 `QueueItem`。
5. 设置时间戳、等级、logger 名称、线程 ID、source location。
6. 构造 `LogPayload`。
7. 调用 `Publish()` 入队。

关键优化：

- 级别不够的日志直接返回，不构造 payload。
- 线程 ID 用 `thread_local` 缓存 hash，避免每次重复计算。
- payload 对整数用 `std::to_chars`，避免 iostream 开销。
- 常见短日志使用 256B inline buffer，避免堆分配。

## 9. OperationGuard 的作用

`OperationGuard` 由 `TryStartOperation()` 返回。进入日志或 flush 路径前，`TryStartOperation()` 会在 `operation_state_` 中增加活跃操作计数；析构时调用 `FinishOperation()` 减少计数。

它主要解决停止过程中的竞态：

- 当 `Stop()` 开始时，`operation_state_` 的 gate bit 会被关闭，新调用无法再获得 `OperationGuard`。
- 但已经进入 `Log()` 的 producer 可能还没完成入队。
- worker 退出前必须等 gate 已关闭且活跃 producer 数量变成 0。
- `FinishOperation()` 在最后一个活跃操作结束时唤醒 worker。

没有这个机制，Stop 可能在线程还在 Publish 时提前退出，导致日志丢失或对象生命周期风险。

## 10. Publish 入队逻辑

`src/async_logger.cpp` 中的 `Publish()` 负责把 `QueueItem` 放入无锁队列。

逻辑：

- 如果是普通日志，先增加 `enqueued_`。
- 循环尝试 `queue_.TryEnqueue()`。
- 成功后增加 `wake_signal_` 并 `notify_one()` 唤醒 worker。
- 如果队列满且策略是 `DropNewest`，撤销 enqueued，增加 dropped，返回 false。
- 如果队列满且策略是 `Block`，先自旋，再 yield，再 sleep 50 微秒。
- 如果 logger 停止，则撤销计数并返回 false。

这种 backoff 策略避免一上来就睡眠，但在长期满队列时也避免空转烧 CPU。

## 11. 为什么使用有界队列

日志队列不能无限增长。无限队列在日志洪峰时会持续占用内存，最终影响业务进程稳定性。

有界队列的好处：

- 内存上限可控。
- 能提供明确背压策略。
- 环形数组预分配，避免节点频繁分配释放。
- 容量取 2 的幂后可以用 `index & mask` 回绕。

代价：

- 队列满时必须在阻塞和丢弃之间做取舍。
- 需要让用户理解满队列策略带来的语义差异。

## 12. 无锁环形队列原理

看 `include/hlog/detail/lock_free_ring_buffer.h`。

它是一个有界 MPMC 队列风格实现，核心是每个槽位都有独立 `sequence`。

### 初始化

```cpp
for (std::size_t i = 0; i < capacity_; ++i) {
  buffer_[i].sequence.store(i, std::memory_order_relaxed);
}
```

初始化时，第 i 个槽位的 sequence 等于 i，表示它可以被 position i 的 producer 写入。

### 入队判断

producer 读取 `enqueue_pos_` 得到 position，然后看：

```cpp
sequence - position
```

- `diff == 0`：槽位可写，尝试 CAS 抢占 enqueue position。
- `diff < 0`：槽位还没被 consumer 释放，队列满。
- `diff > 0`：说明其他 producer 推进了位置，重新加载。

抢占成功后：

- 写入 `cell.value`。
- `cell.sequence.store(position + 1, release)` 发布给 consumer。

### 出队判断

consumer 读取 `dequeue_pos_`，看：

```cpp
sequence - (position + 1)
```

- `diff == 0`：槽位可读，CAS 抢占 dequeue position。
- `diff < 0`：还没有 producer 发布，队列空。
- `diff > 0`：位置落后，重新加载。

出队后：

- move 出 value。
- `cell.sequence.store(position + capacity_, release)` 释放槽位给下一轮 producer。

## 13. 为什么需要 sequence

如果只用 head/tail，很难区分：

- 某个槽位当前这一轮是否可写。
- 某个槽位上一轮数据是否已被消费。
- 环形数组回绕后 ABA 类问题。

sequence 把槽位状态和全局 position 绑定起来。每绕一圈，sequence 增加 capacity，producer 和 consumer 就能判断槽位属于哪一轮。

## 14. 内存序怎么理解

队列中最关键的是：

- producer 写 `cell.value` 后，用 release store 更新 `sequence`。
- consumer acquire load `sequence` 后，看到可读状态，才能安全读取 `cell.value`。
- consumer move 出 value 后，用 release store 更新 `sequence`，表示槽位可被 producer 复用。
- producer acquire load 看到可写状态后，才能安全写入。

`enqueue_pos_` / `dequeue_pos_` 的 CAS 使用 relaxed，因为它们主要用于抢占位置；真正保护槽位数据可见性的是 cell sequence 的 acquire/release。

面试时不要把所有原子都说成 seq_cst。更好的回答是：游标 CAS 不承载数据发布语义，所以 relaxed 足够；槽位 sequence 承载生产者和消费者之间的数据可见性，所以需要 acquire/release。

## 15. false sharing 处理

`enqueue_pos_` 和 `dequeue_pos_` 使用 `alignas(64)`。

原因是多个 producer 高频写 `enqueue_pos_`，consumer 高频写 `dequeue_pos_`。如果两个原子落在同一个 cache line，会导致不必要的缓存一致性流量。对齐到 64 字节可以降低 false sharing。

## 16. worker 唤醒和 flush 等待

最新实现中，worker 休眠使用 `condition_variable`，同时保留 `wake_signal_` 作为无锁可读的版本号。flush 完成票据仍使用 C++20 原子等待。

producer 入队成功会调用 `NotifyWorker()`：

```cpp
wake_signal_.fetch_add(1);
wake_condition_.notify_one();
```

worker drain 完队列后读取当前 signal，再进入 `WaitForWakeup(signal)`：

```cpp
auto signal = wake_signal_.load();
if (queue_.TryDequeue(item)) continue;
WaitForWakeup(signal);
```

这里有两个重要细节：

- worker 在 wait 前会再次 TryDequeue，避免“检查空队列”和“进入等待”之间有新日志到达导致漏唤醒。
- `WaitForWakeup()` 会查看 sink 的 `NextAutoFlushTime()`，如果 FileSink 的 flush deadline 到期，即使没有新日志也会醒来执行 `FlushIfDue()`。

## 17. WorkerLoop

`WorkerLoop()` 的工作模式：

1. 尽可能 drain 队列。
2. 对每条 item 调用 `HandleItem()`。
3. 如果某条日志等级达到 flush level，则本轮 drain 后 flush。
4. 队列空后读取 wake signal。
5. 再尝试出队一次，避免竞态。
6. 如果 stopping 且 active operations 为 0，则退出。
7. 否则 wait。
8. 退出前最后 `sink_->Flush()`。

这条路径体现了异步日志库的核心：producer 快速入队，consumer 批量处理。

## 18. Flush 机制

`Flush()` 不是简单直接调用 sink flush，因为 sink 由后台线程写入。如果 producer 线程直接 flush sink，可能和 worker 并发访问 sink。

当前实现：

1. 生成一个递增 ticket。
2. 构造 `QueueItemType::Flush` 控制消息。
3. 用阻塞策略发布到队列。
4. 等待 `flush_complete_ticket_ >= ticket`。
5. worker 处理 Flush item 时调用 `sink_->Flush()`。
6. worker 更新 complete ticket 并 notify。

这样可以保证：Flush item 之前入队的日志都已经被 worker 处理，并且 sink 已经 flush。

## 19. Stop 机制

`Stop()`：

- 用 CAS 设置 `operation_state_` 的高位 gate bit，保证只执行一次停止流程。
- gate bit 关闭后，新的 `Log()/Flush()` 无法获得 `OperationGuard`。
- 唤醒 worker。
- join worker。

worker 只有在：

- `StopRequested() == true`
- `ActiveOperationCount() == 0`
- 队列已经 drain

时才退出。

析构函数调用 `Stop()`，因此正常生命周期下 logger 析构会尽量 flush 剩余日志。

## 20. 溢出策略

### Block

队列满时业务线程等待，直到入队成功或 logger 停止。

优点：

- 尽量不丢日志。
- 适合审计、安全、错误日志不能丢的场景。

缺点：

- 日志系统可能反压业务线程。
- sink 很慢时会放大业务延迟。

### DropNewest

队列满时丢弃当前新日志。

优点：

- 保护业务线程延迟。
- 适合高频调试日志、指标类日志。

缺点：

- 会丢日志。
- 需要通过 `dropped` 指标观测丢弃情况。

## 21. LogPayload 优化

`LogPayload` 默认有 `256B` inline buffer。

短日志直接写入对象内部数组，不需要堆分配。超过 256B 时才扩容到 heap storage。

这适合日志场景，因为大量日志是短字符串，比如：

```cpp
logger.Info("uid=", uid, " status=", status);
```

这种日志如果每次都 `ostringstream` + `std::string`，会产生较多动态分配和格式化开销。

## 22. BuildPayload 模板拼接

`BuildPayload(args...)` 对每个参数调用 `AppendPayloadPart()`。

处理策略：

- `std::string` / `std::string_view` / C 字符串：直接 append。
- `char`：push_back。
- `bool`：写 `'1'` 或 `'0'`。
- 整数：使用 `std::to_chars` 写入栈上 buffer。
- 浮点和 streamable 类型：退回 `std::ostringstream`。
- 不支持类型：编译期 static_assert。

这是一种分层优化：

- 热点类型走低开销路径。
- 复杂类型保持可用性。
- 不支持的类型在编译期暴露。

## 23. 为什么整数用 to_chars

`std::to_chars`：

- 不依赖 locale。
- 不分配内存。
- 直接写入用户提供的 buffer。
- 比 iostream 更适合日志热路径。

整数是日志中非常常见的参数，比如 id、端口、耗时、状态码，所以优化收益明显。

## 24. Sink 设计

`Sink` 是输出端抽象。logger 不关心具体写到哪里，只调用 `Write()` 和 `Flush()`。

内置 sink：

- `ConsoleSink`：写 stdout/stderr。
- `FileSink`：写文件。
- `RotatingFileSink`：按大小轮转。
- `MultiSink`：把同一条日志 fan-out 到多个 sink。

这种设计让异步核心和输出目标解耦。

## 25. FileSink batching

`FileSink` 内部有 `staging_buffer_`。

写入时：

1. formatter 把日志追加到 staging buffer。
2. 加换行。
3. 如果 buffer 大小超过 `max_batch_size`，写入文件。
4. 如果超过 `flush_interval`，写入并 flush。

目的：

- 把多条日志合并成连续内存。
- 减少小块写文件次数。
- 让后台线程 drain 出来的日志可以更好地批量输出。

注意 benchmark 中真实文件路径会受 OS cache、磁盘、文件系统影响，所以结论要谨慎表达。

## 26. PatternFormatter

formatter 负责把 `LogMessage` 变成字符串。常见字段包括：

- 时间戳。
- 日志等级。
- logger 名称。
- 线程 ID。
- source location。
- payload。

面试时可以说：formatter 属于可扩展点，但格式化也在后台线程执行，所以 producer 热路径只构造 payload，不负责完整文本格式化。

## 27. LoggerConfig

`LoggerConfig` 用来从配置组装 logger 和 sink。它体现工程完整度：不是只能在代码里手动 new sink，而是支持更统一的配置入口。

这类组件在简历里不是性能亮点，但能说明项目是“库”而不是“单文件 demo”。

## 28. benchmark 怎么看

仓库 benchmark 主要回答不同问题：

### hlog_compare_benchmark

对比：

- `mutex + condition_variable + deque`
- `CAS lock-free ring buffer + wake_signal/condition_variable`

目标是隔离队列和线程同步开销。通常使用内存 sink，避免磁盘 I/O 干扰。

### hlog_latency_benchmark

测 producer 侧单次 `Log()` 调用延迟。它包含计时开销，适合看相对比较，不适合当成函数净耗时。

### hlog_payload_benchmark

对比旧的 `ostringstream + string` payload 方式和新的 variadic inline payload 方式，回答 producer payload 构造优化是否有效。

### hlog_file_sink_benchmark

测试真实文件输出路径，观察 batching 对端到端落盘路径的影响。这个结果最容易受环境影响，面试中要强调机器、文件系统、payload 大小都会影响。

### spdlog benchmark

如果安装 spdlog，可以做外部库对比。但外部库对比要谨慎，不能只拿单次结果说绝对碾压。更合理的说法是：在本项目设置的相似 producer payload 和内存 sink 场景下，HLog 的队列热路径有优势。

## 29. 性能结论怎么严谨表达

好的表达：

> 在仓库内 benchmark 中，我用内存 sink 隔离磁盘影响，对比 mutex 队列和 CAS ring buffer。结果显示多线程写日志时，lock-free 队列在吞吐和 producer 延迟上明显优于 mutex 基线。这个结论主要说明队列和线程同步路径优化有效，绝对数值会受机器、编译器和参数影响。

避免这样说：

- “我的日志库一定比 spdlog 快十倍。”
- “无锁一定比有锁快。”
- “文件落盘性能一定更好。”

无锁队列在高竞争场景常有优势，但低并发、单线程、I/O 主导场景下优势可能不明显。

## 30. 代码阅读路线

### 第一遍：使用和架构

1. `examples/basic_example.cpp`
2. `include/hlog/async_logger.h`
3. `src/async_logger.cpp`
4. `include/hlog/sink.h`
5. `src/file_sink.cpp`

目标：理解用户怎么用，日志怎么从 API 进入后台线程。

### 第二遍：队列

1. `include/hlog/detail/lock_free_ring_buffer.h`
2. 画出 enqueue/dequeue 的 sequence 变化。
3. 理解 capacity、mask、position、sequence 的关系。
4. 理解 acquire/release 的位置。

目标：能讲清无锁队列，不停留在“用了 CAS”。

### 第三遍：生命周期

1. `OperationGuard`
2. `Publish()`
3. `Flush()`
4. `Stop()`
5. `WorkerLoop()`

目标：能讲清 flush 为什么可靠，stop 为什么不会提前退出。

### 第四遍：热路径优化

1. `LogPayload`
2. `BuildPayload`
3. `AppendPayloadPart`
4. `CurrentThreadId`
5. `FileSink::Write`

目标：能讲清为什么减少堆分配、减少 iostream、减少锁竞争。

### 第五遍：工程闭环

1. `tests/async_logger_test.cpp`
2. benchmark examples
3. `docs/perf.md`
4. install smoke test
5. CMake install/export

目标：能证明项目不是只写了核心类，还做了验证和可集成性。

## 31. 面试讲法模板

### 30 秒版本

> HLog 是我写的 C++20 异步日志库。业务线程只做级别过滤、payload 构造和入队，后台线程顺序消费并写 sink。核心队列是基于槽位 sequence 的有界无锁环形队列，worker 唤醒采用 `wake_signal + condition_variable`，flush ticket 使用原子等待。项目还优化了 producer payload，用 256B inline buffer 和 `std::to_chars` 减少短日志分配和 iostream 开销，并提供 FileSink batching、空闲期自动 flush、后台异常可观测、轮转、多 sink、benchmark 和安装导出。

### 2 分钟版本

> 这个项目解决的是高并发场景下同步日志锁竞争和业务线程延迟问题。整体架构是多 producer 单 consumer：多个业务线程把日志封装成 QueueItem 放入有界 lock-free ring buffer，后台线程 drain 队列并调用 sink。队列每个 cell 有 sequence，用 acquire/release 保证数据发布和可见性，enqueue/dequeue position 用 CAS 抢占位置。队列满时支持 Block 和 DropNewest。Flush 通过投递带 ticket 的控制消息实现，Stop 通过 running/stopping 和 active operation 计数保证退出前 drain 完。性能上通过 benchmark 分别验证队列同步、producer 延迟、payload 构造和文件输出路径。

## 32. 简历重点

推荐写 3 条即可，不要堆太多。

### 版本一

- 基于 C++20 实现高性能异步日志库，采用多生产者单消费者架构，业务线程将日志写入 CAS 有界无锁环形队列，后台线程批量消费并输出到 File/Console/Rotating/Multi Sink。
- 设计基于槽位 sequence 的 MPMC ring buffer，使用 acquire/release 内存序发布数据，配合 `wake_signal + condition_variable`、cache line 对齐和满队列 backoff 策略降低高并发写日志锁竞争。
- 优化 producer 热路径：实现 256B inline `LogPayload`、整数 `std::to_chars` 追加、`thread_local` 线程 ID 缓存和 FileSink staging buffer，并通过吞吐、延迟、payload、file sink benchmark 验证效果。

### 版本二

- 实现 C++20 异步日志库 HLog，支持日志分级、source location、显式 Flush、阻塞/丢弃溢出策略、文件轮转、多 sink fan-out、配置化构建和 CMake install/export。
- 自研有界无锁环形队列，使用 per-cell sequence 解决环形复用状态判断，基于 CAS 完成多 producer 并发入队，并通过 acquire/release 保证 producer-consumer 数据可见性。
- 构建完整性能验证体系，对比 mutex 队列基线、producer 调用延迟、payload 热路径、真实文件 sink 和可选 spdlog 场景，形成可复现实验报告。

## 33. 你现在怎么学最高效

不用一行行读所有文件。建议按“链路 + 难点”学：

1. 先跑 `basic_example`，知道 API 怎么用。
2. 画出 `Info()` 到 `Sink::Write()` 的路径。
3. 读 `AsyncLogger::Log()`、`Publish()`、`WorkerLoop()`。
4. 单独花时间理解 `LockFreeRingBuffer` 的 sequence 算法。
5. 读 `Flush()` 和 `Stop()`，理解生命周期安全。
6. 读 `LogPayload` 和 `AppendPayloadPart()`，理解热路径优化。
7. 跑 benchmark，看每个 benchmark 回答什么问题。
8. 准备面试时重点讲“为什么这样设计”，不要只说“我用了无锁队列”。

## 34. 最重要的追问准备

必须能答：

- 为什么异步日志能降低业务线程延迟？
- 为什么队列要有界？
- 队列满了怎么办？
- 这个无锁队列的 sequence 是怎么工作的？
- acquire/release 分别保证什么？
- Flush 怎么保证之前日志写完？
- Stop 怎么避免 producer 还在写时 worker 退出？
- 为什么短日志用 inline payload？
- benchmark 为什么用内存 sink？
- 无锁一定比有锁快吗？
- 日志库生产化还差什么？

如果这些问题能答清楚，这个项目就能讲得比较稳。
