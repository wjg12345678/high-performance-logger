# HLog 性能实验与调优手册

这份文档回答 HLog 性能相关的核心问题：benchmark 到底测了什么，哪些参数会影响结果，怎么调队列容量、flush、batch、payload，面试时怎么严谨表达性能收益。

## 1. 性能讨论的前提

不要直接说：

```text
HLog 比 spdlog 快十倍。
```

更严谨的说法：

```text
在当前机器、当前 benchmark、内存 sink、相同 producer payload 和测量口径下，HLog 的 CAS 无锁队列热路径比仓库内 mutex 队列基线有明显吞吐和 producer 延迟优势。真实文件落盘场景会受文件系统、flush 策略、payload 大小和机器负载影响，不能把单次数字泛化成所有场景结论。
```

性能结果必须带上下文：

- 机器配置。
- 编译模式。
- 线程数。
- 每线程日志数。
- warm-up 次数。
- measured rounds。
- sink 类型。
- payload 大小。
- 是否 flush。
- 是否写真实文件。

## 2. benchmark 类型

| benchmark | 测什么 | 不测什么 |
| --- | --- | --- |
| `hlog_compare_benchmark` | CAS 队列 vs mutex 队列吞吐 | 磁盘 IO |
| `hlog_latency_benchmark` | producer 侧 `Log()` 调用延迟 | 日志真正落盘延迟 |
| `hlog_payload_benchmark` | payload 构造热路径 | 队列算法差异 |
| `hlog_file_sink_benchmark` | 真实 FileSink 写路径 | 跨机器稳定结论 |
| `hlog_spdlog_compare_benchmark` | 可选外部基线 | 完整生态对比 |

面试要能说明每个 benchmark 的边界，否则性能数据容易被质疑。

## 3. 吞吐调优

吞吐受这些因素影响：

```text
producer 线程数
queue capacity
payload 构造方式
sink 类型
后台 worker drain 策略
文件系统速度
flush interval
batch size
CPU 调度
```

### 3.1 producer 线程数

线程数太低时，锁竞争不明显，无锁队列优势可能不大。

线程数上升后：

- mutex 队列可能出现锁竞争。
- CAS 队列竞争原子游标。
- CPU cache line 和内存序成本开始明显。

调优建议：

```text
按 1, 2, 4, 8, 16, 32 线程递增压测
观察吞吐是否线性增长
观察 p99 是否抖动
观察 dropped 或 blocked 数量
```

### 3.2 queue capacity

容量过小：

- producer 容易遇到队列满。
- Block 策略增加延迟。
- DropNewest 策略丢日志。

容量过大：

- 占更多内存。
- 日志堆积时间变长。
- sink 变慢时故障暴露更晚。

调优方法：

```text
1. 固定线程数和 payload
2. 依次测试 4096 / 16384 / 65536 / 262144
3. 观察吞吐、p99、pending 峰值和 dropped
4. 选择能吸收短时洪峰但不会长期堆积的容量
```

## 4. 延迟调优

Producer 侧延迟不是落盘延迟，它主要包括：

- 日志级别判断。
- payload 构造。
- 入队。
- 必要时唤醒 worker。
- 队列满时等待或失败。

降低 producer 延迟的方向：

| 方向 | 方法 |
| --- | --- |
| 减少无意义日志 | 先做 level filter |
| 减少堆分配 | 命中 inline payload |
| 减少格式化成本 | 整数用 `std::to_chars` |
| 减少线程 ID 成本 | `thread_local` 缓存 |
| 减少队列竞争 | 合理容量、避免过多 producer |
| 避免满队列 | 提升 sink 能力或选择 DropNewest |

## 5. Payload 热路径

HLog 对短日志做了 inline payload 优化：

```text
常见短日志 -> 256B inline buffer
整数 -> std::to_chars
超出 inline -> spill 到堆内存
```

### 5.1 短日志为什么重要

真实服务中大量日志是短字段拼接：

```text
"request_id=", id, " status=", status, " cost=", ms
```

如果每条都走 `std::ostringstream` 和堆分配，producer 热路径成本会很高。

### 5.2 长日志怎么办

长日志会 spill。不要为了追求 inline 命中而截断关键错误信息，但要避免在高频路径打巨大 payload。

建议：

- 大对象不要直接整段打日志。
- JSON 请求体只在 debug 或采样下记录。
- 二进制内容不要写日志。
- 错误日志保留关键字段和摘要。

## 6. FileSink 调优

文件 sink 是最容易让性能结果波动的部分。

影响因素：

- 文件系统缓存。
- 磁盘类型。
- Docker overlay。
- OS writeback。
- flush interval。
- batch size。
- payload 大小。
- 当前机器负载。

### 6.1 batch size

批量写的目标是减少小 write 次数。

但批量不一定总是更快：

- payload 已经较大时收益下降。
- flush 策略可能主导结果。
- 文件系统缓存让单次测试不稳定。
- 拼接 buffer 本身也有成本。

调优方法：

```text
测试 max_batch_size = 4KiB / 16KiB / 64KiB / 256KiB
测试 flush_interval = 50ms / 250ms / 1s
观察吞吐、p99、退出 flush 时间
```

### 6.2 flush interval

flush interval 越短：

- 数据更快推给 OS。
- 后台线程更频繁 flush。
- 性能可能下降。

flush interval 越长：

- 吞吐可能更好。
- 崩溃丢失窗口更大。

要按业务选择，不是越大越好。

### 6.3 fsync

`Flush()` 不等于 `fsync()`。

如果需要强持久化：

- FileSink 要支持 fsync。
- 性能会明显下降。
- benchmark 要单独标注。

普通应用日志通常不每条 fsync，审计日志和交易日志要另当别论。

## 7. Block 和 DropNewest 对性能的影响

### 7.1 Block

队列满时 producer 等待，表现为：

- dropped 少。
- producer p99 升高。
- 业务线程被日志系统反压。

适合日志不能轻易丢的服务。

### 7.2 DropNewest

队列满时丢弃新日志，表现为：

- producer 延迟更稳。
- dropped 计数上升。
- 排障信息可能缺失。

适合业务延迟优先的服务。

benchmark 一定要同时报告 dropped，否则高吞吐可能只是因为日志被丢了。

## 8. 和 spdlog 对比怎么讲

可以讲：

```text
我做了一个可选 spdlog benchmark，在检测到 spdlog 后构建，用相同 producer payload 和内存计数 sink 对比 producer + async queue 开销。
```

不要讲：

```text
HLog 全面超过 spdlog。
```

原因：

- spdlog 是成熟生态，功能范围更广。
- 默认配置可能不同。
- sink、formatter、队列策略会影响结果。
- 单个 benchmark 不能代表所有场景。

正确表达：

```text
这个对比主要说明在我设定的内存 sink 热路径下，HLog 的轻量队列和 payload 构造有优势；不是宣称整体生态或所有 IO 场景都优于 spdlog。
```

## 9. perf 和火焰图

如果要继续调优，建议使用：

```bash
perf record -g ./build/hlog_compare_benchmark 8 20000 5 1
perf report
```

重点看：

- CAS 重试热点。
- payload 构造函数。
- condition_variable 唤醒。
- sink write。
- allocator。
- futex。

如果热点在 `futex`，可能是等待或锁竞争。

如果热点在 allocator，说明仍有堆分配。

如果热点在 write，说明瓶颈已经到 sink 或文件系统。

## 10. 调优步骤

推荐顺序：

```text
1. 先用内存 sink 测队列和 producer 热路径
2. 再用 payload benchmark 测格式化成本
3. 再接 FileSink 测真实写路径
4. 最后接近业务服务压测
```

不要一上来就用真实服务压测，因为变量太多，难以判断问题来自队列、payload、sink 还是业务本身。

## 11. 性能报告应该包含什么

每次报告至少包含：

```text
commit id
机器配置
编译器和编译参数
线程数
每线程日志数
payload 大小
sink 类型
队列容量
队列满策略
warm-up
measured rounds
中位数和 p99
dropped 数量
原始 CSV
```

只贴一张“最佳 QPS”图不够。

## 12. 面试高频回答

### 问：无锁一定更快吗？

答：

```text
不一定。无锁减少 mutex 阻塞和上下文切换，但会引入 CAS 竞争、内存序和 cache coherence 成本。低并发或临界区很短时 mutex 可能更简单甚至更快。我的 benchmark 是在多生产者异步日志场景下验证 CAS 环形队列相对 mutex 队列基线的收益。
```

### 问：为什么 file sink 批量写没有稳定提升？

答：

```text
文件落盘受文件系统缓存、payload 大小、flush 策略和机器负载影响。批量写减少 write 次数，但也有拼接 buffer 和 flush 时机成本。这个结果说明 benchmark 要分层看：内存 sink 更适合看队列，file sink 更适合看端到端趋势，不能简单套用一个结论。
```

### 问：p99 为什么重要？

答：

```text
日志库在业务热路径中，平均延迟低不代表用户体验稳定。队列满、CAS 竞争、worker 调度、flush 都可能造成尾延迟。p99 能暴露高峰期和异常情况下的抖动。
```

## 13. 最终结论

HLog 性能优化的主线是：

```text
减少 producer 锁竞争
减少 payload 分配和格式化成本
减少后台 sink 小写入
明确队列满和 flush 语义
用分层 benchmark 验证，而不是只看单个 QPS
```

如果继续优化，优先做：

- 更多 payload 类型的 `to_chars` 优化。
- structured logging 减少重复格式化。
- 每 sink 独立 worker，避免 MultiSink 慢目标拖累。
- 更细的 dropped/blocked/flush 指标。
- fsync 模式单独 benchmark。
