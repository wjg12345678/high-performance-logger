# HLog 生产化加固路线

HLog 当前已经是一个可复用的 C++ 异步日志库：有异步 logger、无锁环形队列、日志分级、背压策略、Flush/Stop 生命周期、多个 sink、格式化、CMake install/export、示例和 benchmark。

但生产级日志系统不只是“能打日志”和“性能高”。它还要回答 durability、丢失窗口、配置热更新、结构化、采样、日志轮转、磁盘满、敏感信息、监控、远端采集和版本兼容等问题。

## 1. 当前能力

| 方向 | 当前能力 |
| --- | --- |
| 并发模型 | 多生产者单消费者 |
| 队列 | CAS 无锁有界环形队列 |
| 背压 | Block / DropNewest |
| 生命周期 | Flush / Stop 两阶段关停 |
| sink | Console / File / RotatingFile / MultiSink |
| 格式化 | PatternFormatter |
| 配置 | LoggerConfig |
| 工程 | CMake install/export、examples、tests、benchmark |
| 故障 | sink 异常进入可观测失败状态 |

## 2. 必须主动承认的边界

| 边界 | 说明 |
| --- | --- |
| 崩溃丢失窗口 | 异步队列中尚未写出或尚未 fsync 的日志可能丢 |
| DropNewest 丢日志 | 队列满时可能主动丢弃新日志 |
| Flush 不等于 fsync | Flush 到 sink 不代表磁盘介质持久化 |
| MultiSink 隔离不足 | 一个慢 sink 可能拖累后台 worker |
| 结构化能力不足 | 当前更偏文本日志 |
| 动态配置不足 | 日志级别、sink、采样不能完整热更新 |
| 远端采集不足 | 没有 HTTP/Kafka/OTLP sink |
| 生态不完整 | 不等价 spdlog/log4j 全生态 |

这些边界必须主动讲清楚，反而会显得专业。

## 3. P0：Durability 语义文档化

生产日志首先要明确“什么时候算写成功”。

### 3.1 当前语义

```text
Log() 返回成功
  -> 表示日志已经被 logger 接受
  -> 不等于已经写到文件
  -> 不等于已经 fsync 到磁盘
```

`Flush()`：

```text
等待后台 worker 处理此前已接受日志，并调用 sink Flush
```

仍然不等于 fsync，除非 sink 明确实现 fsync。

### 3.2 建议新增模式

| 模式 | 语义 | 成本 |
| --- | --- | --- |
| `AsyncBuffered` | 当前常规异步写 | 性能最高，崩溃可能丢 |
| `AsyncFlushOnError` | error/critical 自动 flush | 错误路径更可靠 |
| `AsyncFsyncInterval` | 周期 fsync | 成本中等 |
| `SyncCritical` | critical 同步写或 fsync | 成本高 |

不要让所有日志都 fsync。应该按级别和业务重要性选择。

## 4. P0：队列满和丢弃可观测

如果使用 DropNewest，必须让业务知道丢了多少。

建议指标：

```text
hlog_messages_total
hlog_messages_dropped_total
hlog_messages_blocked_total
hlog_queue_pending
hlog_queue_capacity
hlog_worker_flush_total
hlog_sink_errors_total
hlog_stop_drain_ms
```

日志库本身也可以周期性输出内部状态：

```text
hlog stats: pending=1024 dropped=12 failed=false
```

但要避免内部状态日志递归打进同一个故障 sink。

## 5. P0：敏感信息和结构化字段

生产日志最常见风险之一是泄露敏感信息。

禁止直接输出：

- 密码。
- token。
- session id。
- 手机号明文。
- 身份证。
- 银行卡。
- 私钥。
- 完整请求体。

建议支持字段过滤：

```cpp
logger->Info("login failed",
             hlog::Field("user", user_id),
             hlog::Field("ip", client_ip),
             hlog::Field("reason", "bad_password"));
```

结构化日志可以进一步输出 JSON：

```json
{"level":"warn","event":"login_failed","user_id":42,"reason":"bad_password"}
```

当前 PatternFormatter 更偏文本格式，后续可以新增 `JsonFormatter`。

## 6. P1：动态配置

生产服务经常需要临时打开 debug，但不能重启。

建议支持：

- 动态修改 logger level。
- 动态调整采样率。
- 动态切换 sink。
- 动态修改 flush interval。
- 配置版本号和回滚。

注意：

- 配置更新要线程安全。
- sink 切换不能丢日志。
- 错误配置不能破坏已有 logger。

## 7. P1：采样

高 QPS 服务不能每个请求都打完整日志。

采样策略：

| 策略 | 用途 |
| --- | --- |
| 固定比例采样 | 普通请求日志 |
| 错误全量 | error/critical 不采样 |
| 慢请求全量 | 超过阈值记录 |
| key-based 采样 | 同一用户/请求稳定采样 |
| 限速日志 | 同类错误每秒最多 N 条 |

示例：

```text
info 请求日志采样 1%
warn/error 全量
同一错误每 10 秒聚合输出一次
```

## 8. P1：sink 隔离

当前 MultiSink 简单直观，但一个 sink 慢可能拖累全部输出。

生产化方案：

```text
logger queue
  -> dispatcher
       -> sink queue 1 -> file worker
       -> sink queue 2 -> console worker
       -> sink queue 3 -> remote worker
```

优点：

- 慢远端 sink 不影响本地 file sink。
- 每个 sink 可以有独立背压。
- sink 失败可以局部隔离。

缺点：

- 内存更多。
- 顺序语义更复杂。
- Stop/Flush 协调更复杂。

## 9. P1：轮转和保留策略

RotatingFileSink 当前按大小轮转。生产还需要：

- 按时间轮转。
- 按大小 + 时间组合轮转。
- 保留最近 N 个文件。
- 保留最近 N 天。
- 压缩历史日志。
- 删除失败告警。

文件命名建议：

```text
app.log
app.2026-05-15.0.log
app.2026-05-15.1.log.gz
```

轮转要注意：

- rename 原子性。
- Windows/Linux 差异。
- 多进程同时写同一个文件风险。

## 10. P1：远端 sink

生产服务通常会把日志送到集中系统：

- Kafka。
- HTTP collector。
- OpenTelemetry OTLP。
- syslog。
- journald。

远端 sink 要考虑：

- 网络超时。
- 重试。
- 批量发送。
- 压缩。
- 失败落本地。
- 限速。
- 队列积压。

不要让远端日志系统故障拖垮业务。远端 sink 更需要独立队列和熔断。

## 11. P1：崩溃场景

异步日志在崩溃时可能丢：

```text
已经进入业务线程但未入队
已经入队但 worker 未处理
worker 已写到 stdio buffer 但未 flush
flush 到 OS page cache 但未 fsync
```

生产建议：

- fatal signal 时尽量写最小同步日志。
- critical 级别支持同步降级。
- core dump 配置。
- 崩溃前 ring buffer snapshot 不一定安全，谨慎实现。

signal handler 中只能调用 async-signal-safe 函数，不要在 handler 中走复杂 logger。

## 12. P2：API 稳定性

日志库作为基础组件，要注意兼容：

- public header 不随意改名。
- enum 值稳定。
- CMake target 稳定。
- 配置项新增默认值。
- deprecated 机制。
- 版本号。
- changelog。

建议使用语义化版本：

```text
MAJOR.MINOR.PATCH
```

## 13. P2：测试升级

当前已有测试和 benchmark。生产化还应补：

| 测试 | 目标 |
| --- | --- |
| 磁盘满 | sink 失败可观测 |
| 权限不足 | 初始化失败和运行失败 |
| Stop 并发 | 多线程 Log/Flush/Stop 交错 |
| 长稳 | 连续运行数小时无内存增长 |
| 大 payload | spill 路径稳定 |
| 多 sink 异常 | 一个 sink 失败不影响其他 |
| 轮转边界 | 文件刚好达到大小限制 |
| sanitizer | TSAN/ASAN/UBSAN |

## 14. P2：可观测性接入

日志库也需要 metrics：

```text
messages_enqueued
messages_written
messages_dropped
queue_pending
queue_capacity
flush_count
flush_latency
sink_write_latency
worker_wakeup_count
worker_failure
```

可以提供 callback：

```cpp
logger->SetStatsCallback([](const hlog::Stats& stats) {
    export_to_prometheus(stats);
});
```

不要强绑定 Prometheus 库，保持核心库轻量。

## 15. P2：和成熟日志生态的差距

成熟日志库通常有：

- 结构化日志。
- 异步和同步多模式。
- 多语言生态。
- 丰富 sink。
- 动态配置。
- pattern 完整语法。
- MDC / context。
- source location。
- 采样。
- 远端传输。
- 大量生产验证。

HLog 当前更适合展示：

- C++ 并发。
- 无锁队列。
- 异步日志生命周期。
- 热路径优化。
- benchmark 方法。
- 可复用库工程化。

## 16. 优先级路线

### 16.1 一周内

```text
1. 增加 dropped/blocked/failed 更完整指标
2. 增加 AsyncFlushOnError 策略
3. 增加敏感字段文档和脱敏 helper
4. 增加磁盘满/权限失败测试
5. 增加接入文档和配置建议
```

### 16.2 两到三周

```text
1. JsonFormatter
2. 动态日志级别
3. 采样和限速日志
4. 按时间轮转
5. 每 sink 独立队列实验
```

### 16.3 长期

```text
1. OTLP/Kafka/HTTP sink
2. 配置中心接入
3. fsync 策略
4. 多进程安全写
5. OpenTelemetry trace id 集成
6. 完整版本兼容策略
```

## 17. 面试表达

被问“能不能生产用”，建议答：

```text
它已经具备可复用日志库的核心能力，可以接入普通 C++ 项目做异步日志。但我不会说它完整替代成熟日志生态。生产化还要补结构化日志、动态配置、远端 sink、采样、sink 隔离、fsync 策略、敏感信息脱敏和更完整的故障测试。
```

被问“异步日志会不会丢”，建议答：

```text
会有明确丢失窗口。Log 返回只代表日志被接受，不代表落盘。队列满时 DropNewest 会丢，进程崩溃时队列和 OS 缓存中的日志也可能丢。可以通过 Block、Flush、error 自动 flush、fsync 策略降低风险，但性能和可靠性需要取舍。
```

## 18. 最终结论

HLog 后续生产化不要只追更高 QPS。优先级应该是：

```text
语义清楚 > 故障可观测 > 接入稳定 > 性能优化 > 更多 sink
```

日志库是基础设施，最怕“平时快，出事时不知道丢了什么”。所以要把队列满、sink 失败、Flush/Stop、崩溃丢失窗口和敏感信息处理讲清楚。
