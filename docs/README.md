# HLog 文档导航

这份文档是 high-performance-logger / HLog 的阅读入口。HLog 文档已经覆盖性能报告、学习路线、源码导读、面试问答、接入指南、调优手册和生产化路线。阅读时不要只盯着 benchmark 数字，要把并发模型、生命周期、背压和边界一起讲清楚。

## 1. 项目一句话

```text
HLog 是一个面向高并发场景的 C++20 异步日志库，采用多生产者单消费者模型和基于 CAS 的有界无锁环形队列承接日志写入，后台线程统一输出到 Console/File/Rotating/MultiSink，并支持日志分级、Block/DropNewest 背压、Flush/Stop 生命周期、PatternFormatter、LoggerConfig、CMake install/export 和多组 benchmark。
```

学习主线：

```text
并发：MPSC + CAS ring buffer
生命周期：Log / Flush / Stop / worker drain
性能：队列竞争、payload 构造、sink 写入
边界：丢失窗口、队列满、Flush 不等于 fsync
```

## 2. 文档分类

| 目标 | 推荐文档 |
| --- | --- |
| 性能报告 | [perf.md](perf.md) |
| 简历写法 | [简历材料.md](简历材料.md) |
| 系统学习 | [project-study-guide-complete.md](project-study-guide-complete.md) |
| 面试问答 | [面试问题完整回答.md](面试问题完整回答.md) |
| 源码导读 | [source-code-walkthrough-complete.md](source-code-walkthrough-complete.md) |
| 接入其他项目 | [integration-guide.md](integration-guide.md) |
| 性能调优 | [performance-tuning-playbook.md](performance-tuning-playbook.md) |
| 生产化路线 | [production-hardening-roadmap.md](production-hardening-roadmap.md) |

## 3. 新手阅读顺序

第一次看项目：

```text
1. README.md
2. docs/project-study-guide-complete.md 的 1-12 章
3. docs/source-code-walkthrough-complete.md
4. docs/perf.md
5. docs/面试问题完整回答.md
```

目标是先搞懂：为什么要异步日志、为什么用有界队列、无锁环形队列的 sequence 是什么、`Flush()` 为什么不能直接在调用线程写 sink、`Stop()` 为什么需要两阶段关停、benchmark 到底测的是哪一层。

## 4. 面试准备路径

### 4.1 只有 30 分钟

只看：

```text
1. README.md 的产品特性和关键设计思考
2. docs/简历材料.md
3. docs/面试问题完整回答.md 的项目整体、无锁队列、Flush/Stop、benchmark
4. docs/performance-tuning-playbook.md 的性能表达边界
```

必须会讲：HLog 不是简单封装 `ofstream`，异步日志有好处也有丢失窗口，无锁不一定永远更快，`Flush()` 不等于 `fsync()`，性能数据必须带测试口径。

### 4.2 有 2 小时

按这个顺序：

```text
1. docs/project-study-guide-complete.md
2. docs/source-code-walkthrough-complete.md
3. docs/performance-tuning-playbook.md
4. docs/integration-guide.md
5. docs/production-hardening-roadmap.md
```

重点看：`AsyncLogger::Log()` 热路径、`OperationGuard` 的生命周期保护、`Publish()` 入队逻辑、`WorkerLoop()` 如何 drain 和 flush、`LockFreeRingBuffer::TryEnqueue/TryDequeue`、`LogPayload` 如何减少短日志分配。

## 5. 源码阅读路径

### 5.1 用户 API

```text
include/hlog/hlog.h
include/hlog/async_logger.h
include/hlog/logger_config.h
```

要回答：用户怎么创建 logger、配置项有哪些、队列满策略怎么设置、统计信息能看到什么。

### 5.2 核心 logger

```text
src/core/async_logger.cpp
```

按函数读：

```text
Log
TryStartOperation
Publish
Flush
Stop
WorkerLoop
RecordWorkerFailure
```

要回答：日志级别过滤为什么在最前面、Stop 时如何拒绝新调用、已经进入 Log 的线程如何安全收尾、Flush 如何保证此前日志被处理。

### 5.3 无锁队列

```text
include/hlog/detail/lock_free_ring_buffer.h
```

要回答：capacity 为什么要 2 的幂、每个 cell 的 sequence 做什么、acquire/release 分别保证什么、CAS 为什么可以 relaxed、这个队列是 lock-free 还是 wait-free。

### 5.4 payload 和格式化

```text
include/hlog/detail/log_payload.h
src/core/pattern_formatter.cpp
```

要回答：256B inline buffer 为什么有价值、`std::to_chars` 为什么比流式格式化轻、长日志 spill 怎么处理、PatternFormatter 在哪个线程执行。

### 5.5 sink

```text
include/hlog/sink.h
src/sinks/file_sink.cpp
src/sinks/rotating_file_sink.cpp
src/sinks/console_sink.cpp
include/hlog/sinks/multi_sink.h
```

要回答：sink 抽象有什么好处、FileSink batching 为什么不一定总是更快、RotatingFileSink 解决什么、MultiSink 的慢 sink 拖累问题怎么生产化解决。

## 6. benchmark 阅读路径

先看：

```text
docs/perf.md
docs/performance-tuning-playbook.md
examples/compare_benchmark.cpp
examples/latency_benchmark.cpp
examples/payload_benchmark.cpp
examples/file_sink_benchmark.cpp
examples/spdlog_compare_benchmark.cpp
```

必须知道每个 benchmark 的边界：

| benchmark | 看什么 |
| --- | --- |
| compare | 队列和同步机制差异 |
| latency | producer 侧调用延迟 |
| payload | 日志内容构造成本 |
| file sink | 真实文件写路径 |
| spdlog compare | 特定口径外部基线 |

不要用单个数字宣称全面超过成熟日志库。

## 7. 最重要的 10 个问题

1. 为什么异步日志能降低业务线程开销？
2. 为什么队列要有界？
3. `Block` 和 `DropNewest` 怎么取舍？
4. 无锁环形队列的 sequence 有什么作用？
5. acquire/release 内存序怎么理解？
6. `Flush()` 为什么通过后台线程做？
7. `Stop()` 如何避免丢已接受日志？
8. 异步日志什么时候会丢？
9. FileSink batching 为什么不一定更快？
10. spdlog 对比怎么严谨表达？

## 8. 生产化边界

优先看：

```text
docs/integration-guide.md
docs/performance-tuning-playbook.md
docs/production-hardening-roadmap.md
```

必须主动承认：`Log()` 返回不代表落盘，`Flush()` 不等于 `fsync()`，进程崩溃可能丢队列中日志，DropNewest 会主动丢日志，当前结构化日志、动态配置、远端 sink、采样和 sink 隔离还需要补。

更好的说法：

```text
HLog 已经具备可复用异步日志库核心能力，但生产级日志系统还要补结构化、动态配置、远端采集、采样、fsync 策略和更完整的故障测试。
```

## 9. 最终建议

HLog 的学习重点是：

```text
并发结构：无锁有界队列
生命周期：Log/Flush/Stop 和后台 worker
性能验证：分层 benchmark 和测试口径
工程边界：背压、丢失窗口、sink 故障、生产化差距
```

能把这四条讲清楚，比单纯背“吞吐提升多少”更有说服力。
