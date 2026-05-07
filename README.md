# HLog

HLog 是一个面向高并发场景的 C++20 异步日志库，核心热路径使用基于 `CAS` 的无锁环形队列替代 `mutex` 队列，重点优化多线程日志写入下的锁竞争、payload 构造和单消费者输出路径。

## 产品特性

- 采用“日志器 + 输出端 + 后台线程”分层架构，支持日志分级、异步落盘和自定义输出端。
- 内置 `FileSink / ConsoleSink / RotatingFileSink / MultiSink`，支持文件、控制台、轮转文件以及多目标 fan-out 输出。
- 提供 `PatternFormatter` 与 `LoggerConfig` 配置层，支持格式模板、sink 组合和运行时组装 logger。
- 使用基于 `CAS` 的有界无锁环形队列，结合 `atomic::wait/notify` 和 `thread_local` 线程 ID 缓存降低多线程写日志时的热路径开销。
- 将 producer 热路径的 payload 构造改为 `256B` inline buffer + `std::to_chars` 直接追加，减少常见短日志场景下的额外堆分配和流格式化开销。
- 在 `FileSink` 内部增加 staging buffer 批量写，支持 `max_batch_size / flush_interval`，把后台线程 drain 出来的多条日志拼成连续 buffer 后统一写出。
- 提供阻塞与丢弃两种满队列策略，并通过 install smoke test、sanitizer、内部基线 benchmark 和可选 `spdlog` benchmark 量化与验证效果。

## 设计目标

同步日志实现起来很直接，但在高并发场景下容易暴露几个典型问题：

- 多个业务线程会竞争同一把锁，日志路径容易成为热点。
- 队列节点的频繁申请与释放会放大额外开销。
- 唤醒与等待逻辑如果依赖互斥锁，线程协调成本会持续累积。
- 日志洪峰可能同时带来延迟抖动和内存膨胀。

目标是在高并发写日志场景下提供低开销异步写入能力，并补齐 sink 组合、安装导出、自动化验证和 benchmark 报告等工程闭环。

当前版本已经可以作为可复用的异步日志库集成到其他项目中；同时仍然保持清晰边界，重点覆盖并发队列、producer payload 和文件写路径优化，而不是扩展成大而全的日志生态集合。

## 能力概览

- 异步写入：业务线程只负责构造日志并入队，后台线程统一顺序写出。
- 多线程安全：支持多生产者并发写日志，单消费者独占输出端。
- 日志分级：支持 `trace / debug / info / warn / error / critical / off`。
- 背压策略：支持 `Block` 和 `DropNewest` 两种满队列处理方式。
- 刷盘同步：支持显式 `Flush()`，通过原子票据等待后台线程完成刷盘。
- 内置输出端：提供 `ConsoleSink`、`FileSink`、`RotatingFileSink` 和 `MultiSink`。
- 格式与配置：提供 `PatternFormatter` 和 `LoggerConfig` factory。

## 架构设计

```text
业务线程
   |
   v
异步日志器
  - 日志级别过滤
  - 消息格式化
  - CAS 无锁环形队列
  - 原子唤醒与刷盘协调
   |
   v
后台消费线程
   |
   v
输出端（控制台 / 文件输出 / 轮转文件 / 组合输出）
```

核心组件：

- `hlog::LockFreeRingBuffer<T>`：基于槽位序号的有界无锁队列。
- `hlog::AsyncLogger`：提供日志接口、等级过滤、异步投递、刷盘与统计能力。
- `hlog::Sink`：输出端抽象。
- `hlog::ConsoleSink / FileSink / RotatingFileSink / MultiSink`：内置输出端实现。
- `hlog::PatternFormatter / LoggerConfig`：格式模板与运行时配置装配层。

## 关键设计思考

### 1. 为什么选择有界环形缓冲区

- 环形缓冲区可以预分配内存，避免日志热路径上的频繁动态分配。
- 容量有上界，日志洪峰不会无限制挤占内存。
- 队列容量取 2 的幂后，可以通过位运算完成回绕，减少取模开销。

### 2. 为什么用 `CAS` 替代互斥锁

- 多个生产者只竞争原子游标，而不是进入同一个临界区。
- 在高并发场景下，可显著降低 `mutex + condition_variable` 带来的锁竞争与队头阻塞。
- 每个槽位维护独立序号，能够在无锁条件下判断“可写 / 可读 / 已满”等状态。

### 3. 为什么采用单消费者模型

- 文件写出天然需要顺序性，单消费者更容易保证日志顺序。
- 输出端只由后台线程持有，不需要在写盘路径上重复加锁。
- 可以把并发问题收敛到“入队和出队”这条链路，而不是让队列竞争和输出竞争混在一起。

### 4. 如何保证线程安全

- 生产者通过 `compare_exchange_weak` 抢占槽位并发布日志消息。
- 消费线程独占出队和输出端写入，避免多线程同时写文件。
- `Flush()` 会投递控制消息，并等待原子完成票据更新。
- 线程唤醒使用 `atomic::wait/notify`，减少额外的互斥锁协调路径。

## 队列热路径对比

仓库中的 `hlog_compare_benchmark` 在同样的异步模型和同样的内存输出端下，对比两种方案：

- 互斥锁方案：`std::mutex + std::condition_variable + std::deque`
- 无锁方案：`CAS` 无锁环形队列 + `atomic::wait/notify`

示例命令：

```bash
./build/hlog_compare_benchmark 8 20000 5 1
```

当前机器上一组样例结果如下（`2026-05-07`，`1` 次 warm-up + `5` 次 measured rounds，取中位吞吐；对照组是仓库内 `mutex + condition_variable + deque` 基线，而不是外部日志库）：

| 实现方案 | 吞吐 |
| --- | ---: |
| 互斥锁异步队列 | `1.38e6 msg/s` |
| `CAS` 无锁异步队列 | `4.35e6 msg/s` |
| 吞吐提升 | `+215%` |

说明：

- 这个压测刻意使用内存输出端，目的是隔离磁盘 I/O 干扰，专门观察队列与线程同步开销。
- benchmark 会输出 warm-up、每轮测量结果以及 summary，建议优先引用中位吞吐而不是单次最好成绩。
- 绝对数值会受机器配置、编译器和优化选项影响，但趋势可以反映锁竞争差异和扩展性差异。
- 更完整的多线程扩展数据、调用延迟表格和图表见 [docs/perf.md](docs/perf.md)。

## 可选外部库对标

仓库中的 `hlog_spdlog_compare_benchmark` 会在检测到 `spdlog` 后自动构建，使用同样的 producer payload、同样的单消费者异步模型和同样的内存计数 sink，对比：

- `spdlog` 异步 logger
- `hlog` 的 `CAS` 异步 logger

示例命令：

```bash
./build/hlog_spdlog_compare_benchmark 8 20000 5 1
```

当前机器上一组样例结果如下（`2026-05-07`，`1` 次 warm-up + `5` 次 measured rounds，取中位吞吐）：

| 实现方案 | 吞吐 |
| --- | ---: |
| `spdlog` 异步 logger | `3.36e5 msg/s` |
| `hlog` `CAS` 异步 logger | `4.27e6 msg/s` |
| 吞吐提升 | `+1170%` |

说明：

- 这组对比仍然刻意使用内存 sink，回答的是“现成异步日志库在相似写路径上的 producer + queue 开销对比”，不是完整文件落盘场景结论。
- `spdlog` benchmark 是可选 target；本仓库不会强制拉第三方依赖，但 `.github/workflows/ci.yml` 中的 `external-benchmark-smoke` 会在 Ubuntu 上安装 `libspdlog-dev` 并跑一轮 smoke。
- 如果本机没有 `spdlog`，CMake 会跳过 `hlog_spdlog_compare_benchmark`，其余构建、测试和 install/export 不受影响。

## 调用延迟观测

仓库中的 `hlog_latency_benchmark` 使用同样的异步模型和同样的内存输出端，测量 producer 侧单次 `Log()` 调用延迟：

```bash
./build/hlog_latency_benchmark 8 20000 5 1
```

当前机器上一组样例结果如下（`2026-05-07`，`1` 次 warm-up + `5` 次 measured rounds，取中位；表格单位为 `us`）：

| 实现方案 | 平均调用延迟 | p95 | p99 |
| --- | ---: | ---: | ---: |
| 互斥锁异步队列 | `9.40` | `47.58` | `85.27` |
| `CAS` 无锁异步队列 | `1.52` | `3.17` | `6.78` |

说明：

- 这个 benchmark 只包围 producer 侧单次日志调用，适合观察业务线程写日志时的直接开销。
- 结果包含 `steady_clock` 取时开销，因此更适合做相对比较，而不是解读成绝对“函数体净耗时”。
- 为了隔离磁盘 I/O 干扰，latency benchmark 同样使用内存输出端。
- 正式报告可通过 `scripts/generate_benchmark_report.py` 重新生成。

## Producer 热路径基准

仓库中的 `hlog_payload_benchmark` 保持同一个 `hlog::AsyncLogger` 和同一个内存输出端不变，只比较 producer 如何构造 payload：

- `prebuilt_*`：先用 `std::ostringstream` 拼成 `std::string`，再调用 `Info()`，近似优化前热路径。
- `variadic_short`：直接调用 `Info("thread=", tid, " seq=", index)`，命中 inline payload。
- `variadic_long`：直接调用 `Info(..., " payload=", <512B blob>)`，触发 spillover 路径。

示例命令：

```bash
./build/hlog_payload_benchmark 8 20000 5 1
```

当前机器上一组样例结果如下（`2026-05-07`，`1` 次 warm-up + `5` 次 measured rounds，取中位；表格单位为 `us`）：

| 场景 | 旧热路径均值 | 新热路径均值 | 均值改善 | 旧热路径 p99 | 新热路径 p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 短日志（inline） | `3.57` | `1.24` | `65.2%` | `7.71` | `2.03` |
| 长日志（spill） | `5.05` | `1.61` | `68.1%` | `14.48` | `5.11` |

说明：

- 这个 benchmark 不是 `mutex` 对照，而是同一个 logger 内部对比“旧 payload 构造方式”和“新 payload 构造方式”。
- 它更适合回答“lock-free queue 之外，producer 侧格式化和分配还剩多少成本”这个问题。
- 更完整的多线程表格和图表见 [docs/perf.md](docs/perf.md) 中的 `Payload Hot Path` 章节。

## 文件落盘路径基准

仓库中的 `hlog_file_sink_benchmark` 使用真实 `FileSink` 路径，对比：

- `unbatched_file_sink`：`max_batch_size=1`，近似逐条写文件。
- `batched_file_sink`：`max_batch_size=64 KiB`、`flush_interval=250 ms`，把多条格式化日志拼成连续 buffer 后统一写出。

示例命令：

```bash
./build/hlog_file_sink_benchmark 8 1000 2 0
```

当前机器上一组样例结果如下（`2026-05-07`，`2` 次 measured rounds，取中位）：

| 场景 | 吞吐 |
| --- | ---: |
| 非批量 file sink | `1.19e3 msg/s` |
| 批量 file sink | `1.01e3 msg/s` |
| 吞吐变化 | `-15.5%` |

说明：

- 这组 benchmark 使用 `512B` 级别 payload，比内存 sink benchmark 更接近真实日志体积。
- 真实文件系统路径会引入缓存、元数据和存储设备噪声，所以它回答的是“端到端写路径局部优化是否有价值”，不是跨机器可复现的绝对指标。
- 这部分结果的符号和幅度都可能随本机负载、缓存状态和存储介质变化而波动，因此更适合展示方法和测试口径，而不是在 README 里承诺一个稳定的正向百分比。

## 仓库结构

```text
LICENSE
Dockerfile
docker-compose.yml
include/hlog/
  async_logger.h
  console_sink.h
  file_sink.h
  logger_config.h
  lock_free_ring_buffer.h
  log_level.h
  log_message.h
  log_payload.h
  multi_sink.h
  pattern_formatter.h
  rotating_file_sink.h
  sink.h
cmake/
  hlogConfig.cmake.in
src/
  async_logger.cpp
  console_sink.cpp
  file_sink.cpp
  logger_config.cpp
  pattern_formatter.cpp
  rotating_file_sink.cpp
examples/
  basic_example.cpp
  benchmark_support.h
  benchmark_main.cpp
  compare_benchmark.cpp
  find_package_consumer/
  file_sink_benchmark.cpp
  latency_benchmark.cpp
  payload_benchmark.cpp
  service_example.cpp
  spdlog_compare_benchmark.cpp
tests/
  async_logger_test.cpp
docs/
  perf.md
scripts/
  generate_benchmark_report.py
  install_smoke_test.sh
```

## 构建与运行

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

示例程序：

```bash
./build/hlog_example
./build/hlog_benchmark
./build/hlog_compare_benchmark
./build/hlog_file_sink_benchmark
./build/hlog_latency_benchmark
./build/hlog_payload_benchmark
./build/hlog_service_example
```

如果本机已安装 `spdlog`，还会额外生成：

```bash
./build/hlog_spdlog_compare_benchmark
```

生成的日志默认写入工程目录下的 `example.log`、`benchmark.log` 和 `service.log`。

服务示例支持通过环境变量调整端口、级别、pattern 和 rotation 参数，例如：

```bash
HLOG_PORT=8080 \
HLOG_LEVEL=info \
HLOG_PATTERN="%Y-%m-%d %H:%M:%S.%e [%l] [%n] [tid=%t] %v" \
./build/hlog_service_example
```

也可以直接在容器里构建并运行服务示例：

```bash
docker compose build hlog-service
docker compose run --service-ports -e HLOG_MAX_REQUESTS=1 hlog-service
curl -sS http://127.0.0.1:18080/healthz
```

## 安装与复用

如果只想把它当作库来构建，可以关闭 examples 和 tests：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DHLOG_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build build-release -j
cmake --install build-release --prefix /tmp/hlog-install
```

安装后可以在其他 CMake 项目中通过 `find_package` 使用：

```cmake
find_package(hlog CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE HLog::hlog)
```

仓库内提供了可复现的安装烟测脚本和最小 consumer：

```bash
bash scripts/install_smoke_test.sh
```

- 外部 consumer 工程位于 `examples/find_package_consumer/`
- 脚本会执行 `cmake --install`，再在临时目录里用 `find_package(hlog CONFIG REQUIRED)` 配置、链接并运行 consumer
- GitHub Actions 中的 `package-smoke` job 会在 `ubuntu-latest` 和 `macos-latest` 上自动执行这套流程

## 测试

当前测试覆盖：

- 日志等级过滤正确性
- 并发异步写入正确性
- `DropNewest` 策略下的溢出行为
- `Flush()` 对未消费日志的等待语义
- `Stop()` 后拒绝新写入，以及 `pending` 统计归零
- `Stop()` 不显式 `Flush()` 时仍能排空已接受的日志
- `PatternFormatter` token 展开与 `LogLevel` 解析
- `MultiSink` fan-out 行为
- `ConsoleSink` 格式化输出
- `RotatingFileSink` 按大小轮转
- `LoggerConfig` factory 构建 file sink logger

执行命令：

```bash
ctest --test-dir build --output-on-failure
```

## CI 与 Sanitizer

仓库包含 GitHub Actions workflow：`.github/workflows/ci.yml`。

- 常规 CI：在 `ubuntu-latest` 和 `macos-latest` 上执行 `Release` 构建与测试
- 安装复用验证：在 `ubuntu-latest` 和 `macos-latest` 上执行 install smoke test
- 外部基线验证：在 `ubuntu-latest` 上安装 `libspdlog-dev` 并运行 `hlog_spdlog_compare_benchmark` smoke
- 并发/内存检查：在 `ubuntu-latest + clang` 上执行 `ASAN + UBSAN` 与 `TSAN`

本地也可以直接复用同一套开关：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHLOG_ENABLE_ASAN=ON -DHLOG_ENABLE_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHLOG_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

## 生成正式性能报告

仓库提供了性能报告生成脚本，会批量跑吞吐和延迟 benchmark，并生成 CSV、SVG 图表和 Markdown 报告：

```bash
python3 scripts/generate_benchmark_report.py \
  --compare-binary ./build/hlog_compare_benchmark \
  --latency-binary ./build/hlog_latency_benchmark \
  --payload-binary ./build/hlog_payload_benchmark \
  --file-sink-binary ./build/hlog_file_sink_benchmark \
  --output-dir docs/perf \
  --messages-per-thread 20000 \
  --file-messages-per-thread 1000 \
  --measured-rounds 5 \
  --warmup-rounds 1 \
  --file-measured-rounds 2 \
  --file-warmup-rounds 0 \
  --threads 1 2 4 8 16 \
  --file-threads 1 2 4 8
```

输出产物默认写入：

- `docs/perf.md`
- `docs/perf/throughput.csv`
- `docs/perf/latency.csv`
- `docs/perf/payload-latency.csv`
- `docs/perf/file-sink-throughput.csv`
- `docs/perf/throughput.svg`
- `docs/perf/mean-latency.svg`
- `docs/perf/p99-latency.svg`
- `docs/perf/payload-short-mean-latency.svg`
- `docs/perf/payload-short-p99-latency.svg`
- `docs/perf/payload-long-mean-latency.svg`
- `docs/perf/payload-long-p99-latency.svg`
- `docs/perf/file-sink-throughput.svg`

## 项目概览

HLog 采用“日志器 + 输出端 + 后台线程”的分层架构，使用基于 `CAS` 的有界无锁环形队列承接多线程日志写入，通过后台线程顺序写出，并提供日志分级、阻塞/丢弃两种背压策略以及显式 `Flush()` 同步机制。仓库当前内置 `ConsoleSink / FileSink / RotatingFileSink / MultiSink`，并通过 `PatternFormatter` 和 `LoggerConfig` 提供格式模板、sink 组合和运行时装配能力；同时支持 `CMake install/export`、最小 consumer、install smoke test 和一个多线程 HTTP 服务示例。为了验证核心优化路径，仓库还补充了与 `mutex + condition_variable` 异步队列的同模型对照压测、可选 `spdlog` 外部基线、producer 侧调用延迟、payload hot-path 和真实 file-sink benchmark。当前机器上的样例结果显示，在 `8` 线程、`160000` 条日志、`1` 次 warm-up + `5` 次测量的中位口径下，队列热路径吞吐从约 `1.38e6 msg/s` 提升到约 `4.35e6 msg/s`，单次 `Log()` 平均延迟从约 `9.40 us` 降到约 `1.52 us`，短日志 payload 构造均值从约 `3.57 us` 降到约 `1.24 us`。真实 file-sink 路径的结果单独保留在 `docs/perf.md` 中，因为它比内存 sink benchmark 更容易受文件系统状态和本机负载影响。

## License

本项目采用 [MIT License](LICENSE)。
