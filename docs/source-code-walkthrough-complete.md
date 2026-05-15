# HLog 源码逐文件导读

这份文档用于把 HLog 学到“能被追问源码”的程度。学习目标是：问到异步日志、无锁队列、flush、stop、payload、sink 或 benchmark 时，能说出代码在哪里、关键状态是什么、为什么这样设计、边界在哪里。

## 1. 阅读总路线

推荐顺序：

1. `include/hlog/hlog.h`：公共聚合头。
2. `include/hlog/async_logger.h`：用户 API、模板热路径、状态字段。
3. `src/core/async_logger.cpp`：Publish、Flush、Stop、worker loop。
4. `include/hlog/detail/lock_free_ring_buffer.h`：无锁队列。
5. `include/hlog/detail/log_payload.h`：inline payload。
6. `include/hlog/detail/log_message.h`：消息结构。
7. `include/hlog/sink.h`：sink 抽象和自动 flush 扩展点。
8. `src/sinks/file_sink.cpp`：文件 batching 和 deadline flush。
9. `src/sinks/rotating_file_sink.cpp`：文件轮转。
10. `src/core/pattern_formatter.cpp`：格式化。
11. `src/config/logger_config.cpp`：配置化装配。
12. `tests/async_logger_test.cpp` 和 `examples/*benchmark.cpp`：验证和性能口径。

## 2. `include/hlog/hlog.h`

这是用户最可能 include 的公共入口，通常聚合 logger、level、config 和 sinks。面试里可以说：库对外暴露的是稳定 API，内部细节放在 `detail/` 下，避免用户依赖实现。

## 3. `include/hlog/async_logger.h`

这是核心头文件。

### 用户 API

- `Trace/Debug/Info/Warn/Error/Critical`：普通日志接口。
- `TraceAt/.../CriticalAt`：带 source location 的接口。
- `Flush()`：同步等待后台线程刷盘。
- `Stop()`：关闭 logger。
- `SetLevel()/level()`：动态日志等级。
- `SetFlushLevel()/flush_level()`：高等级日志触发 flush。
- `failed()/failure_message()`：后台 sink 失败可观测。
- `Stats()`：enqueued、dropped、written、pending。

### 配置

- `queue_size`：队列容量。
- `overflow_policy`：`Block` 或 `DropNewest`。
- `level`：过滤等级。
- `flush_level`：达到该等级后触发 flush。

### 关键字段

- `queue_`：无锁环形队列。
- `worker_`：后台消费线程。
- `operation_state_`：高位 gate bit + 低位活跃操作计数。
- `wake_signal_`：worker 唤醒版本号。
- `wake_condition_`：worker 休眠和 deadline 唤醒。
- `flush_request_ticket_ / flush_complete_ticket_`：flush 同步。
- `worker_failed_`、`background_failure_message_`：后台异常状态。
- `enqueued_ / dropped_ / written_`：统计。

面试追问：

- 为什么 `operation_state_` 用一个原子同时表示 gate 和计数？
- 为什么 `failed()` 要暴露给用户？
- 为什么 source location 用宏？

## 4. `AsyncLogger::Log()`

`Log()` 是 producer 热路径。

流程：

1. `TryStartOperation()` 尝试进入。
2. 如果 Stop 已关闭 gate，直接返回 false。
3. 如果后台 worker 已失败，返回 false。
4. 做日志等级过滤。
5. 填充 `QueueItem`。
6. 获取 timestamp、level、logger name、thread id、source location。
7. `BuildPayload()` 构造 payload。
8. `Publish()` 入队。

关键优化：

- 等级过滤在 payload 构造前。
- thread id 用 `thread_local` 缓存。
- 整数参数用 `std::to_chars`。
- 短 payload 用 inline buffer。

## 5. `TryStartOperation()` 和 `OperationGuard`

这是 Stop 安全性的核心。

`operation_state_`：

- 高位 `kOperationGateClosedBit` 表示 Stop 已开始，不允许新操作入场。
- 低位表示当前活跃 producer/flush 操作数量。

流程：

1. 读取 state。
2. 如果 gate bit 已关闭，返回空 guard。
3. CAS 把活跃计数加一。
4. 返回有效 guard。
5. guard 析构时调用 `FinishOperation()`。
6. 如果 gate 已关闭且剩余活跃计数为 0，唤醒 worker。

追问：

- 为什么不能只用一个 `running_` bool？
- Stop 和正在 Publish 的线程有什么竞态？

回答：

- bool 只能拒绝新请求，不能知道已经进入热路径但还没发布完成的线程数量。gate + 计数可以同时表达“停止接纳新操作”和“等待已入场操作完成”。

## 6. `Publish()`

职责：

- 把 `QueueItem` 写入 lock-free queue。
- 执行满队列策略。
- 成功后唤醒 worker。
- 后台失败时停止继续等待。

流程：

1. 普通日志先增加 `enqueued_`。
2. 循环 `queue_.TryEnqueue()`。
3. 成功后 `NotifyWorker()`。
4. 如果队列满且 `DropNewest`，撤销 enqueued 并增加 dropped。
5. 如果 `Block`，先自旋，再 yield，再 sleep。
6. 如果 worker failed，撤销计数并返回 false。

追问：

- 为什么 DropNewest 不破坏队列顺序？
- 为什么 Block 会影响业务线程延迟？

## 7. `Flush()`

Flush 是同步控制消息，不直接在业务线程调用 sink。

流程：

1. `TryStartOperation()`。
2. 检查 worker 是否 joinable、是否 failed。
3. 申请递增 ticket。
4. 构造 `QueueItemType::Flush`。
5. 阻塞发布到队列。
6. 等待 `flush_complete_ticket_ >= ticket`。
7. worker 处理 flush item 时调用 `sink_->Flush()`。
8. worker 更新 complete ticket 并 notify。

追问：

- 为什么 flush 要排队？
- 多个线程同时 Flush 是否安全？

回答：

- 和普通日志同队列才能保证 flush 前面的日志先被处理。
- ticket 单调递增，等待条件是 complete ticket 达到自己的 ticket。

## 8. `Stop()`

Stop 是两阶段关停：

1. `RequestStop()` 关闭 gate，拒绝新 Log/Flush。
2. `NotifyWorker()` 唤醒后台线程。
3. `join()` 等 worker 退出。
4. worker 在队列 drain 且活跃操作数为 0 后退出。
5. 退出前最后 `sink_->Flush()`。

追问：

- Stop 后已经入队的日志会不会丢？
- Stop 时有 producer 正在 Publish 怎么办？

回答：

- 正常路径会 drain 已接受日志。
- 已经进入的 producer 会被 operation count 保护，worker 等它完成。

## 9. `WorkerLoop()`

worker loop 的核心职责：

- drain 队列。
- 处理普通日志和 flush 控制消息。
- 高等级日志触发 flush。
- 处理 FileSink 空闲期自动 flush。
- 等待唤醒或 deadline。
- 捕获 sink 异常并进入 failed 状态。

关键逻辑：

- `HandleItem()` 返回是否需要 flush。
- `sink_->FlushIfDue(Sink::Clock::now())` 支持空闲期刷盘。
- `WaitForWakeup(signal)` 用 condition variable 等新 signal、失败、停止或 deadline。
- try/catch 包住 worker loop，异常进入 `RecordWorkerFailure()`。

追问：

- 为什么 sink 异常不能让线程静默退出？
- 为什么要有 `FlushIfDue()`？

## 10. `RecordWorkerFailure()`

职责：

- 保存异常指针和错误信息。
- 请求停止。
- 设置 `worker_failed_`。
- 推进 flush complete ticket，唤醒等待 Flush 的线程。
- 唤醒 worker。

追问：

- 如果 Flush 正在等待时 sink 抛异常怎么办？

回答：

- `RecordWorkerFailure()` 会把 complete ticket 推到当前 request ticket，并 notify 所有等待者，Flush 能返回 false，而不是永久卡死。

## 11. `include/hlog/detail/lock_free_ring_buffer.h`

这是 HLog 最重要的并发结构。

### 数据结构

- `Cell.sequence`：槽位序号。
- `Cell.value`：队列元素。
- `enqueue_pos_`：producer 抢占位置。
- `dequeue_pos_`：consumer 抢占位置。
- `capacity_`：向上取 2 的幂。
- `mask_`：用于 `position & mask_` 回绕。

### TryEnqueue

1. 读取 `enqueue_pos_`。
2. 找 `buffer_[position & mask_]`。
3. acquire load sequence。
4. `sequence - position == 0` 表示可写。
5. CAS 抢占 position。
6. 写 value。
7. release store sequence 为 `position + 1`。

### TryDequeue

1. 读取 `dequeue_pos_`。
2. 找对应 cell。
3. acquire load sequence。
4. `sequence - (position + 1) == 0` 表示可读。
5. CAS 抢占 position。
6. move 出 value。
7. release store sequence 为 `position + capacity_`。

追问：

- 为什么 position CAS 可以 relaxed？
- 为什么 cell sequence 要 acquire/release？
- 这个队列是不是 wait-free？

## 12. `include/hlog/detail/log_payload.h`

`LogPayload` 用 256B inline buffer 优化短日志。

重点：

- 默认使用 `inline_storage_`。
- 超过容量才分配 heap。
- move 时如果对方是 heap，直接接管内存。
- `view()` 返回 string_view，避免额外拷贝。

追问：

- 为什么不用每次 `std::ostringstream`？
- 256B 是否一定最优？

回答：

- 大量业务日志是短文本加数字，inline buffer 能减少堆分配。
- 256B 是工程折中，需要结合日志长度分布和 benchmark 调整。

## 13. `include/hlog/sink.h`

Sink 是输出抽象。

接口：

- `Write(const LogMessage&)`
- `Flush()`
- `NextAutoFlushTime()`
- `FlushIfDue(Clock::time_point)`

后两个接口是为了让 FileSink 在没有新日志时也能按 flush interval 刷盘。

## 14. `src/sinks/file_sink.cpp`

职责：

- 按 PatternFormatter 格式化日志。
- 写入 staging buffer。
- 根据 `max_batch_size` 或 `flush_interval` flush。
- 支持空闲期 deadline。

追问：

- batching 一定更快吗？
- flush interval 到期但没有新日志怎么办？

回答：

- 不一定，真实文件系统受 OS cache 和磁盘影响，需要 benchmark。
- worker 的 `WaitForWakeup()` 会按 `NextAutoFlushTime()` 设置 wait_until deadline。

## 15. `src/sinks/rotating_file_sink.cpp`

职责：

- 控制单个日志文件大小。
- 达到阈值后轮转。
- 适合避免日志文件无限增长。

生产化追问：

- 是否支持按时间轮转？
- 是否支持压缩和保留数量？

回答：

- 当前重点是大小轮转，生产可继续扩展时间轮转、压缩和保留策略。

## 16. `src/core/pattern_formatter.cpp`

职责：

- 把 `LogMessage` 格式化成文本。
- 处理时间、等级、logger name、thread id、source location、payload。

追问：

- 格式化为什么放后台线程？

回答：

- producer 热路径只构造 payload，完整文本格式化由 worker 执行，可以降低业务线程开销。

## 17. benchmark 文件定位

- `compare_benchmark.cpp`：mutex 队列 vs lock-free 队列。
- `latency_benchmark.cpp`：producer `Log()` 延迟。
- `payload_benchmark.cpp`：payload 构造路径。
- `file_sink_benchmark.cpp`：真实文件 sink。
- `spdlog_compare_benchmark.cpp`：可选外部库对比。

回答 benchmark 时要说明：

- 内存 sink 用于隔离磁盘影响。
- 文件 sink 结果受环境影响更大。
- 引用中位数，不只引用最好值。

## 18. 面试定位表

| 问题 | 先看文件 | 核心函数 |
| --- | --- | --- |
| 日志调用怎么入队 | `include/hlog/async_logger.h` | `Log`、`BuildPayload` |
| 队列满怎么办 | `src/core/async_logger.cpp` | `Publish` |
| Stop 为什么安全 | `src/core/async_logger.cpp` | `TryStartOperation`、`RequestStop`、`FinishOperation` |
| Flush 为什么可靠 | `src/core/async_logger.cpp` | `Flush`、`HandleItem` |
| worker 怎么休眠 | `src/core/async_logger.cpp` | `WaitForWakeup`、`NotifyWorker` |
| sink 异常怎么办 | `src/core/async_logger.cpp` | `RecordWorkerFailure` |
| 无锁队列怎么实现 | `include/hlog/detail/lock_free_ring_buffer.h` | `TryEnqueue`、`TryDequeue` |
| payload 为什么快 | `include/hlog/detail/log_payload.h` | `EnsureCapacity`、`MoveFrom` |
| 文件 batching 怎么做 | `src/sinks/file_sink.cpp` | `Write`、`FlushIfDue` |
| 性能怎么证明 | `examples/*benchmark.cpp` | benchmark main |

## 19. 最后复盘清单

面试前确认自己能回答：

- 异步日志和同步日志的本质差异。
- 队列为什么要有界。
- Block 和 DropNewest 适合什么场景。
- per-cell sequence 怎么避免环形复用混乱。
- acquire/release 分别保证什么。
- Stop 的 gate bit + active count 怎么避免竞态。
- Flush ticket 为什么能保证顺序。
- worker failure 为什么要唤醒 flush 等待者。
- FileSink 空闲期自动 flush 怎么触发。
- benchmark 为什么分队列、延迟、payload、文件 sink 几类。
