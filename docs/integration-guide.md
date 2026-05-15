# HLog 接入指南

这份文档回答“这个日志库怎么被别的 C++ 项目使用”。它不是重复讲无锁队列原理，而是面向接入者说明 CMake、logger 配置、sink 选择、队列大小、Flush/Stop、队列满策略、异常处理和最佳实践。

## 1. 接入方式总览

HLog 当前定位是 C++20 异步日志库。推荐接入方式：

```text
业务代码
  -> hlog public API
  -> AsyncLogger
  -> LockFreeRingBuffer
  -> 后台 worker
  -> Sink
```

支持的输出端：

| Sink | 场景 |
| --- | --- |
| `ConsoleSink` | 本地调试、容器 stdout |
| `FileSink` | 单文件落盘 |
| `RotatingFileSink` | 按大小轮转文件 |
| `MultiSink` | 同时写控制台和文件，或多个目标 |

## 2. CMake 接入

项目支持 CMake install/export。外部项目可以通过 `find_package` 使用。

示意：

```cmake
find_package(hlog REQUIRED)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE hlog::hlog)
```

如果作为源码子目录：

```cmake
add_subdirectory(third_party/high-performance-logger)
target_link_libraries(my_service PRIVATE hlog)
```

推荐在生产项目中使用 install/export 或包管理方式，避免业务仓库直接修改日志库源码。

## 3. 最小使用示例

```cpp
#include <hlog/hlog.h>

int main() {
    auto logger = hlog::CreateConsoleLogger("app");
    logger->Info("service started, port=", 8080);
    logger->Flush();
    logger->Stop();
}
```

实际服务中建议：

- main 初始化 logger。
- 全局注入或通过 logger manager 获取。
- 服务退出时显式 `Flush()` / `Stop()`。
- 不要在静态对象析构顺序不确定时依赖日志。

## 4. 选择同步还是异步

HLog 主打异步，但接入时要理解异步日志的取舍。

异步日志优点：

- 业务线程不直接写磁盘。
- 多线程下减少写文件锁竞争。
- 后台线程可以批量 drain。
- 日志洪峰可以通过队列吸收。

异步日志成本：

- 进程崩溃时队列中未落盘日志可能丢失。
- 队列满时必须选择阻塞还是丢弃。
- Flush 需要跨线程同步。
- Stop 生命周期要管理好。

适合场景：

- 高并发服务。
- 日志量较大。
- 能接受明确的异步丢失窗口。

不适合场景：

- 每条日志都必须落盘确认的审计系统。
- 进程崩溃前最后几条日志绝不能丢。
- 极低日志量且追求最简单实现。

## 5. 队列大小设置

队列是有界的。容量太小会频繁满，容量太大占内存且掩盖下游 sink 变慢。

建议估算：

```text
queue_capacity >= 峰值日志速率 * 可接受缓冲时间
```

例如：

```text
峰值 100000 msg/s
希望吸收 100 ms 突发
容量至少约 10000
```

由于环形队列容量通常向 2 的幂对齐，实际可设置为 `16384`。

经验建议：

| 服务类型 | 起步容量 |
| --- | --- |
| 小型服务 | 4096 |
| 普通 Web 服务 | 16384 |
| 高并发服务 | 65536 |
| 日志洪峰明显服务 | 131072 或更高，但要压测 |

不要无限加大队列。队列大只是在延迟暴露下游瓶颈，不会让磁盘变快。

## 6. 队列满策略

HLog 支持两类策略：

| 策略 | 行为 | 适合场景 |
| --- | --- | --- |
| `Block` | 队列满时业务线程等待 | 不希望丢日志，允许业务变慢 |
| `DropNewest` | 队列满时丢弃新日志 | 保护业务延迟，允许丢部分日志 |

### 6.1 Block

优点：

- 尽量不丢已提交日志。
- 日志系统背压能传回业务线程。

缺点：

- sink 慢时业务线程会被拖慢。
- 极端情况下可能放大延迟。

适合：

- 排查型服务。
- 低 QPS 但日志重要。
- 后台任务。

### 6.2 DropNewest

优点：

- 保护业务线程。
- 日志洪峰时不会无限阻塞。

缺点：

- 新日志可能丢。
- 如果没有 dropped counter，排障会误判。

适合：

- 高 QPS 在线服务。
- 业务可用性优先。
- debug/info 级别日志多的场景。

## 7. 日志级别建议

| 级别 | 用法 |
| --- | --- |
| `trace` | 极细调试，生产默认关闭 |
| `debug` | 调试路径，生产谨慎开启 |
| `info` | 服务启动、配置、关键状态变化 |
| `warn` | 可恢复异常、重试、降级 |
| `error` | 请求失败、依赖故障、数据异常 |
| `critical` | 进程级严重错误 |
| `off` | 关闭 |

接入建议：

- 热路径不要打大量 `info`。
- 请求级日志优先采样。
- 错误日志要包含 request_id / user_id / resource_id 等定位字段。
- 不要输出密码、token、手机号明文。

## 8. Sink 选择

### 8.1 ConsoleSink

适合容器环境：

```text
应用 -> stdout -> Docker / systemd / 日志采集器
```

优点是运维简单，缺点是本地文件轮转不由 HLog 控制。

### 8.2 FileSink

适合本地文件落盘。要注意：

- 文件目录权限。
- 磁盘容量。
- flush interval。
- 是否需要 fsync。

### 8.3 RotatingFileSink

适合长期运行服务，避免单个日志文件无限变大。

需要配置：

- 单文件最大大小。
- 保留文件数量。
- 文件命名规则。

生产还可以扩展：

- 按时间轮转。
- 压缩旧日志。
- 删除过期日志。

### 8.4 MultiSink

适合同一条日志写多个目标：

```text
ConsoleSink + RotatingFileSink
```

注意：MultiSink 中某个 sink 慢或失败，会影响后台 worker。生产上可考虑每个 sink 独立队列，避免互相拖累。

## 9. Flush 语义

`Flush()` 的意义：

```text
调用线程希望此前已经被接受的日志尽快被后台 worker 写到 sink，并调用 sink Flush。
```

它不是：

- 操作系统落盘保证。
- `fsync()` 保证。
- 进程崩溃后不丢保证。

如果业务要求强落盘，需要 sink 支持 `fsync` 或外部存储确认，但这会显著影响性能。

使用建议：

- 服务启动后不必每条日志 flush。
- 关键操作前后可以按需 flush。
- 程序退出前显式 flush。
- benchmark 时明确是否 flush。

## 10. Stop 语义

`Stop()` 应该在服务退出时调用。

设计目标：

```text
1. 拒绝新的 Log/Flush 调用
2. 等待已经进入 Log/Flush 的操作完成发布或失败
3. 后台 worker drain 队列
4. flush sink
5. join worker 线程
```

接入禁忌：

- 不要在多个静态对象析构过程中随意写日志。
- 不要 Stop 后继续使用 logger。
- 不要在 signal handler 中直接调用复杂日志逻辑。

## 11. 异常处理

HLog 后台 sink 抛异常时不会直接 `terminate`，而是进入失败状态。

业务应该：

- 定期检查 `failed()`。
- 记录或上报 `failure_message()`。
- 对关键服务触发告警。
- 必要时切换备用 sink。

常见异常：

- 文件路径不存在。
- 权限不足。
- 磁盘满。
- 文件系统只读。
- 轮转 rename 失败。

## 12. 多线程使用建议

推荐：

- 多线程共享同一个 logger。
- 每条日志带上线程 id 或 request id。
- 避免在日志参数构造中做重 CPU 操作。
- 热路径尽量使用简单类型，让 `to_chars` 和 inline payload 命中。

不推荐：

- 每个请求创建一个 logger。
- 每条日志 new 一个 sink。
- 在日志语句里拼大型 JSON。
- 在高频路径开启 trace。

## 13. 和 WebServer 项目接入

如果接入 Atlas 这类 C++ WebServer：

建议日志维度：

```text
request_id
method
path
status
latency_ms
user_id
client_ip
error_code
```

建议级别：

- `/healthz` 成功不打 info，或采样。
- 4xx 按场景打 warn/debug。
- 5xx 打 error。
- 上传失败要带 file_id/upload_id。
- DB 连接失败、Redis 限流失败打 error/warn。

不要在文件上传每个 chunk 都打 info，否则日志系统会成为瓶颈。

## 14. 生产接入检查清单

```text
1. 明确日志级别策略
2. 明确队列容量
3. 明确队列满策略
4. 明确 sink 类型和文件路径
5. 明确轮转和保留策略
6. 明确 flush interval
7. 明确退出时 Stop 顺序
8. 接入 dropped/failed 指标
9. 压测日志洪峰
10. 模拟磁盘满和权限错误
11. 检查敏感信息脱敏
12. 文档说明崩溃丢失窗口
```

## 15. 接入时的常见错误

| 错误 | 后果 |
| --- | --- |
| 队列容量过小 | 高峰期频繁阻塞或丢日志 |
| 每条日志 Flush | 性能退化成同步日志 |
| 不调用 Stop | 退出时队列中日志可能没 drain |
| 日志中输出 token | 安全风险 |
| 所有日志都 info | 生产日志噪声大，性能受影响 |
| 忽略 failed 状态 | sink 已失败但业务不知道 |
| 只看吞吐 benchmark | 忽略 p99、丢弃数和磁盘影响 |

## 16. 最终建议

HLog 接入其他项目时，不要先宣传“最快”，而要先讲清楚：

```text
日志级别怎么定
队列满了怎么办
退出时怎么 drain
sink 失败怎么发现
崩溃时哪些日志可能丢
benchmark 口径是什么
```

这些问题回答清楚，日志库才像一个工程组件，而不是只会跑性能数字的 demo。
