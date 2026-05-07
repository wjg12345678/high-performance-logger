#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import platform
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SUMMARY_RE = re.compile(r"^(?P<name>\S+)_summary (?P<body>.+)$")


@dataclass
class ThroughputRow:
    threads: int
    mutex_median_throughput: float
    cas_median_throughput: float
    improvement_percent: float


@dataclass
class LatencyRow:
    threads: int
    mutex_mean_ns: float
    cas_mean_ns: float
    mutex_p95_ns: int
    cas_p95_ns: int
    mutex_p99_ns: int
    cas_p99_ns: int


@dataclass
class PayloadLatencyRow:
    threads: int
    prebuilt_short_mean_ns: float
    variadic_short_mean_ns: float
    prebuilt_short_p99_ns: int
    variadic_short_p99_ns: int
    prebuilt_long_mean_ns: float
    variadic_long_mean_ns: float
    prebuilt_long_p99_ns: int
    variadic_long_p99_ns: int


@dataclass
class FileSinkRow:
    threads: int
    unbatched_median_throughput: float
    batched_median_throughput: float
    improvement_percent: float


@dataclass
class EnvironmentInfo:
    system: str
    platform_string: str
    machine: str
    cpu_model: str
    hardware_model: str
    hardware_threads: str
    memory: str
    compiler_path: str
    compiler_version: str
    build_type: str
    generator: str
    build_flags: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate benchmark report assets for hlog.")
    parser.add_argument("--compare-binary", default="build/hlog_compare_benchmark")
    parser.add_argument("--latency-binary", default="build/hlog_latency_benchmark")
    parser.add_argument("--payload-binary", default="build/hlog_payload_benchmark")
    parser.add_argument("--file-sink-binary", default="build/hlog_file_sink_benchmark")
    parser.add_argument("--output-dir", default="docs/perf")
    parser.add_argument("--messages-per-thread", type=int, default=20000)
    parser.add_argument("--file-messages-per-thread", type=int, default=5000)
    parser.add_argument("--measured-rounds", type=int, default=5)
    parser.add_argument("--warmup-rounds", type=int, default=1)
    parser.add_argument("--file-measured-rounds", type=int, default=2)
    parser.add_argument("--file-warmup-rounds", type=int, default=0)
    parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8, 16])
    parser.add_argument("--file-threads", nargs="+", type=int, default=[1, 2, 4, 8])
    return parser.parse_args()


def run_command(command: list[str]) -> str:
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    return completed.stdout


def try_run_command(command: list[str]) -> str:
    try:
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return ""
    return completed.stdout.strip()


def parse_cmake_cache(cache_path: Path) -> dict[str, str]:
    if not cache_path.is_file():
        return {}

    entries: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("#"):
            continue
        if "=" not in stripped or ":" not in stripped:
            continue
        key_with_type, value = stripped.split("=", 1)
        key = key_with_type.split(":", 1)[0]
        entries[key] = value
    return entries


def detect_cmake_cache(args: argparse.Namespace) -> dict[str, str]:
    candidates = [
        Path(args.compare_binary).resolve().parent / "CMakeCache.txt",
        Path("build/CMakeCache.txt"),
    ]
    for candidate in candidates:
        entries = parse_cmake_cache(candidate)
        if entries:
            return entries
    return {}


def detect_cpu_model() -> str:
    system = platform.system()
    if system == "Darwin":
        hardware = detect_macos_hardware_overview()
        processor_name = hardware.get("Processor Name", "")
        processor_speed = hardware.get("Processor Speed", "")
        if processor_name and processor_speed:
            return f"{processor_name} @ {processor_speed}"
        if processor_name:
            return processor_name
        cpu_model = try_run_command(["sysctl", "-n", "machdep.cpu.brand_string"])
        if cpu_model:
            return cpu_model
    elif system == "Linux":
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.is_file():
            for line in cpuinfo.read_text(encoding="utf-8", errors="ignore").splitlines():
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
        lscpu_output = try_run_command(["lscpu"])
        for line in lscpu_output.splitlines():
            if line.startswith("Model name:"):
                return line.split(":", 1)[1].strip()

    machine = try_run_command(["uname", "-m"])
    if machine:
        return machine
    return platform.processor() or platform.machine() or "unknown"


def detect_macos_hardware_overview() -> dict[str, str]:
    output = try_run_command(["system_profiler", "SPHardwareDataType"])
    fields: dict[str, str] = {}
    for line in output.splitlines():
        stripped = line.strip()
        if ":" not in stripped:
            continue
        key, value = stripped.split(":", 1)
        fields[key.strip()] = value.strip()
    return fields


def detect_compiler_version(compiler_path: str, compiler_id: str, compiler_version: str) -> str:
    if compiler_id and compiler_version:
        return f"{compiler_id} {compiler_version}"
    if compiler_version:
        return compiler_version
    if compiler_path:
        version_line = try_run_command([compiler_path, "--version"]).splitlines()
        if version_line:
            return version_line[0]
    return "unknown"


def collect_environment_info(args: argparse.Namespace) -> EnvironmentInfo:
    cache_entries = detect_cmake_cache(args)
    macos_hardware = detect_macos_hardware_overview() if platform.system() == "Darwin" else {}
    compiler_path = cache_entries.get("CMAKE_CXX_COMPILER", "unknown")
    compiler_version = detect_compiler_version(
        compiler_path,
        cache_entries.get("CMAKE_CXX_COMPILER_ID", ""),
        cache_entries.get("CMAKE_CXX_COMPILER_VERSION", ""),
    )
    build_type = cache_entries.get("CMAKE_BUILD_TYPE", "unknown") or "unknown"
    build_type_key = build_type.upper()
    build_flags = " ".join(
        value
        for value in (
            cache_entries.get("CMAKE_CXX_FLAGS", "").strip(),
            cache_entries.get(f"CMAKE_CXX_FLAGS_{build_type_key}", "").strip(),
        )
        if value
    ).strip() or "unknown"
    hardware_threads = try_run_command(["sysctl", "-n", "hw.ncpu"])
    if not hardware_threads:
        hardware_threads = str(os.cpu_count() or "unknown")

    return EnvironmentInfo(
        system=platform.system() or "unknown",
        platform_string=platform.platform(),
        machine=platform.machine() or "unknown",
        cpu_model=detect_cpu_model(),
        hardware_model=(
            macos_hardware.get("Model Identifier")
            or macos_hardware.get("Model Name")
            or "unknown"
        ),
        hardware_threads=hardware_threads,
        memory=macos_hardware.get("Memory", "unknown"),
        compiler_path=compiler_path,
        compiler_version=compiler_version,
        build_type=build_type,
        generator=cache_entries.get("CMAKE_GENERATOR", "unknown") or "unknown",
        build_flags=build_flags,
    )


def parse_summary(output: str) -> dict[str, dict[str, float]]:
    summaries: dict[str, dict[str, float]] = {}
    for line in output.splitlines():
        line = line.strip()
        match = SUMMARY_RE.match(line)
        if not match:
            continue

        fields: dict[str, float] = {}
        for token in match.group("body").split():
            key, value = token.split("=", 1)
            fields[key] = float(value)
        summaries[match.group("name")] = fields
    return summaries


def collect_throughput_rows(args: argparse.Namespace) -> list[ThroughputRow]:
    rows: list[ThroughputRow] = []
    for thread_count in args.threads:
        output = run_command(
            [
                args.compare_binary,
                str(thread_count),
                str(args.messages_per_thread),
                str(args.measured_rounds),
                str(args.warmup_rounds),
            ]
        )
        summaries = parse_summary(output)
        mutex = summaries["mutex_async_logger"]
        cas = summaries["cas_async_logger"]
        rows.append(
            ThroughputRow(
                threads=thread_count,
                mutex_median_throughput=mutex["median_throughput_msgs_per_sec"],
                cas_median_throughput=cas["median_throughput_msgs_per_sec"],
                improvement_percent=(
                    (cas["median_throughput_msgs_per_sec"] - mutex["median_throughput_msgs_per_sec"])
                    / mutex["median_throughput_msgs_per_sec"]
                    * 100.0
                ),
            )
        )
    return rows


def collect_latency_rows(args: argparse.Namespace) -> list[LatencyRow]:
    rows: list[LatencyRow] = []
    for thread_count in args.threads:
        output = run_command(
            [
                args.latency_binary,
                str(thread_count),
                str(args.messages_per_thread),
                str(args.measured_rounds),
                str(args.warmup_rounds),
            ]
        )
        summaries = parse_summary(output)
        mutex = summaries["mutex_async_logger"]
        cas = summaries["cas_async_logger"]
        rows.append(
            LatencyRow(
                threads=thread_count,
                mutex_mean_ns=mutex["median_mean_call_ns"],
                cas_mean_ns=cas["median_mean_call_ns"],
                mutex_p95_ns=int(mutex["median_p95_call_ns"]),
                cas_p95_ns=int(cas["median_p95_call_ns"]),
                mutex_p99_ns=int(mutex["median_p99_call_ns"]),
                cas_p99_ns=int(cas["median_p99_call_ns"]),
            )
        )
    return rows


def collect_payload_latency_rows(args: argparse.Namespace) -> list[PayloadLatencyRow]:
    rows: list[PayloadLatencyRow] = []
    for thread_count in args.threads:
        output = run_command(
            [
                args.payload_binary,
                str(thread_count),
                str(args.messages_per_thread),
                str(args.measured_rounds),
                str(args.warmup_rounds),
            ]
        )
        summaries = parse_summary(output)
        prebuilt_short = summaries["prebuilt_short"]
        variadic_short = summaries["variadic_short"]
        prebuilt_long = summaries["prebuilt_long"]
        variadic_long = summaries["variadic_long"]
        rows.append(
            PayloadLatencyRow(
                threads=thread_count,
                prebuilt_short_mean_ns=prebuilt_short["median_mean_call_ns"],
                variadic_short_mean_ns=variadic_short["median_mean_call_ns"],
                prebuilt_short_p99_ns=int(prebuilt_short["median_p99_call_ns"]),
                variadic_short_p99_ns=int(variadic_short["median_p99_call_ns"]),
                prebuilt_long_mean_ns=prebuilt_long["median_mean_call_ns"],
                variadic_long_mean_ns=variadic_long["median_mean_call_ns"],
                prebuilt_long_p99_ns=int(prebuilt_long["median_p99_call_ns"]),
                variadic_long_p99_ns=int(variadic_long["median_p99_call_ns"]),
            )
        )
    return rows


def collect_file_sink_rows(args: argparse.Namespace) -> list[FileSinkRow]:
    rows: list[FileSinkRow] = []
    for thread_count in args.file_threads:
        output = run_command(
            [
                args.file_sink_binary,
                str(thread_count),
                str(args.file_messages_per_thread),
                str(args.file_measured_rounds),
                str(args.file_warmup_rounds),
            ]
        )
        summaries = parse_summary(output)
        unbatched = summaries["unbatched_file_sink"]
        batched = summaries["batched_file_sink"]
        rows.append(
            FileSinkRow(
                threads=thread_count,
                unbatched_median_throughput=unbatched["median_throughput_msgs_per_sec"],
                batched_median_throughput=batched["median_throughput_msgs_per_sec"],
                improvement_percent=(
                    (batched["median_throughput_msgs_per_sec"] - unbatched["median_throughput_msgs_per_sec"])
                    / unbatched["median_throughput_msgs_per_sec"]
                    * 100.0
                ),
            )
        )
    return rows


def format_scientific(value: float) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}e6"
    if value >= 1_000:
        return f"{value / 1_000:.2f}e3"
    return f"{value:.0f}"


def ns_to_us(value: float) -> float:
    return value / 1_000.0


def improvement_percent(baseline: float, optimized: float) -> float:
    if baseline == 0:
        return 0.0
    return (baseline - optimized) / baseline * 100.0


def write_csv(path: Path, header: list[str], rows: Iterable[list[object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(rows)


def render_line_chart(
    *,
    path: Path,
    title: str,
    y_label: str,
    x_values: list[int],
    series: list[tuple[str, str, list[float]]],
) -> None:
    width = 900
    height = 420
    margin_left = 80
    margin_right = 24
    margin_top = 48
    margin_bottom = 56
    plot_width = width - margin_left - margin_right
    plot_height = height - margin_top - margin_bottom

    max_y = max(max(values) for _, _, values in series)
    max_y *= 1.1
    if max_y == 0:
        max_y = 1.0

    def x_pos(index: int) -> float:
        if len(x_values) == 1:
            return margin_left + plot_width / 2.0
        return margin_left + plot_width * index / (len(x_values) - 1)

    def y_pos(value: float) -> float:
        return margin_top + plot_height - (value / max_y) * plot_height

    y_ticks = 5
    parts: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<style>',
        'text { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; fill: #16324f; }',
        '.title { font-size: 20px; font-weight: 700; }',
        '.axis-label { font-size: 12px; font-weight: 600; }',
        '.tick { font-size: 11px; fill: #49617b; }',
        '.grid { stroke: #d7e1ec; stroke-width: 1; }',
        '.axis { stroke: #35506b; stroke-width: 1.5; }',
        '.legend { font-size: 12px; font-weight: 600; }',
        '</style>',
        '<rect width="100%" height="100%" fill="#f7fbff" rx="12" ry="12"/>',
        f'<text x="{margin_left}" y="28" class="title">{escape_xml(title)}</text>',
        f'<text x="{width / 2:.1f}" y="{height - 14}" text-anchor="middle" class="axis-label">Threads</text>',
        (
            f'<text x="20" y="{margin_top + plot_height / 2:.1f}" transform="rotate(-90 20 {margin_top + plot_height / 2:.1f})" '
            f'class="axis-label">{escape_xml(y_label)}</text>'
        ),
    ]

    for tick in range(y_ticks + 1):
        value = max_y * tick / y_ticks
        y = y_pos(value)
        parts.append(f'<line x1="{margin_left}" y1="{y:.1f}" x2="{width - margin_right}" y2="{y:.1f}" class="grid"/>')
        parts.append(
            f'<text x="{margin_left - 10}" y="{y + 4:.1f}" text-anchor="end" class="tick">{escape_xml(format_chart_value(value))}</text>'
        )

    parts.append(
        f'<line x1="{margin_left}" y1="{margin_top}" x2="{margin_left}" y2="{margin_top + plot_height}" class="axis"/>'
    )
    parts.append(
        f'<line x1="{margin_left}" y1="{margin_top + plot_height}" x2="{width - margin_right}" y2="{margin_top + plot_height}" class="axis"/>'
    )

    for index, x_value in enumerate(x_values):
        x = x_pos(index)
        parts.append(
            f'<line x1="{x:.1f}" y1="{margin_top + plot_height}" x2="{x:.1f}" y2="{margin_top + plot_height + 6}" class="axis"/>'
        )
        parts.append(
            f'<text x="{x:.1f}" y="{margin_top + plot_height + 22}" text-anchor="middle" class="tick">{x_value}</text>'
        )

    legend_x = width - margin_right - 240
    legend_y = 24
    for offset, (label, color, _) in enumerate(series):
        current_y = legend_y + offset * 22
        parts.append(f'<line x1="{legend_x}" y1="{current_y}" x2="{legend_x + 18}" y2="{current_y}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<circle cx="{legend_x + 9}" cy="{current_y}" r="3.5" fill="{color}"/>')
        parts.append(
            f'<text x="{legend_x + 28}" y="{current_y + 4}" class="legend">{escape_xml(label)}</text>'
        )

    for label, color, values in series:
        points = " ".join(f"{x_pos(index):.1f},{y_pos(value):.1f}" for index, value in enumerate(values))
        parts.append(
            f'<polyline fill="none" stroke="{color}" stroke-width="3" stroke-linejoin="round" stroke-linecap="round" points="{points}"/>'
        )
        for index, value in enumerate(values):
            x = x_pos(index)
            y = y_pos(value)
            parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{color}"/>')
            parts.append(
                f'<text x="{x:.1f}" y="{y - 10:.1f}" text-anchor="middle" class="tick">{escape_xml(format_chart_value(value))}</text>'
            )

    parts.append("</svg>")
    path.write_text("\n".join(parts), encoding="utf-8")


def escape_xml(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def format_chart_value(value: float) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}M"
    if value >= 1_000:
        return f"{value / 1_000:.1f}K"
    return f"{value:.0f}"


def write_markdown_report(
    *,
    output_path: Path,
    throughput_rows: list[ThroughputRow],
    latency_rows: list[LatencyRow],
    payload_latency_rows: list[PayloadLatencyRow],
    file_sink_rows: list[FileSinkRow],
    environment_info: EnvironmentInfo,
    args: argparse.Namespace,
) -> None:
    generated_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    throughput_table = "\n".join(
        f"| {row.threads} | `{format_scientific(row.mutex_median_throughput)}` | `{format_scientific(row.cas_median_throughput)}` | `{row.improvement_percent:.1f}%` |"
        for row in throughput_rows
    )
    latency_table = "\n".join(
        (
            f"| {row.threads} | `{ns_to_us(row.mutex_mean_ns):.2f}` | `{ns_to_us(row.cas_mean_ns):.2f}` | "
            f"`{ns_to_us(row.mutex_p95_ns):.2f}` | `{ns_to_us(row.cas_p95_ns):.2f}` | "
            f"`{ns_to_us(row.mutex_p99_ns):.2f}` | `{ns_to_us(row.cas_p99_ns):.2f}` |"
        )
        for row in latency_rows
    )
    payload_short_table = "\n".join(
        (
            f"| {row.threads} | `{ns_to_us(row.prebuilt_short_mean_ns):.2f}` | "
            f"`{ns_to_us(row.variadic_short_mean_ns):.2f}` | "
            f"`{improvement_percent(row.prebuilt_short_mean_ns, row.variadic_short_mean_ns):.1f}%` | "
            f"`{ns_to_us(row.prebuilt_short_p99_ns):.2f}` | "
            f"`{ns_to_us(row.variadic_short_p99_ns):.2f}` | "
            f"`{improvement_percent(float(row.prebuilt_short_p99_ns), float(row.variadic_short_p99_ns)):.1f}%` |"
        )
        for row in payload_latency_rows
    )
    payload_long_table = "\n".join(
        (
            f"| {row.threads} | `{ns_to_us(row.prebuilt_long_mean_ns):.2f}` | "
            f"`{ns_to_us(row.variadic_long_mean_ns):.2f}` | "
            f"`{improvement_percent(row.prebuilt_long_mean_ns, row.variadic_long_mean_ns):.1f}%` | "
            f"`{ns_to_us(row.prebuilt_long_p99_ns):.2f}` | "
            f"`{ns_to_us(row.variadic_long_p99_ns):.2f}` | "
            f"`{improvement_percent(float(row.prebuilt_long_p99_ns), float(row.variadic_long_p99_ns)):.1f}%` |"
        )
        for row in payload_latency_rows
    )
    file_sink_table = "\n".join(
        (
            f"| {row.threads} | `{format_scientific(row.unbatched_median_throughput)}` | "
            f"`{format_scientific(row.batched_median_throughput)}` | "
            f"`{row.improvement_percent:.1f}%` |"
        )
        for row in file_sink_rows
    )

    content = f"""# Benchmark Report

Generated at `{generated_at}` by [`scripts/generate_benchmark_report.py`](../scripts/generate_benchmark_report.py).

## Methodology

- Compare target: `./build/hlog_compare_benchmark`
- Latency target: `./build/hlog_latency_benchmark`
- Payload target: `./build/hlog_payload_benchmark`
- File sink target: `./build/hlog_file_sink_benchmark`
- Thread counts: `{", ".join(str(value) for value in args.threads)}`
- File sink thread counts: `{", ".join(str(value) for value in args.file_threads)}`
- Messages per thread: `{args.messages_per_thread}`
- File sink messages per thread: `{args.file_messages_per_thread}`
- Warm-up rounds: `{args.warmup_rounds}`
- Measured rounds: `{args.measured_rounds}`
- File sink warm-up rounds: `{args.file_warmup_rounds}`
- File sink measured rounds: `{args.file_measured_rounds}`
- Compare / latency / payload targets use an in-memory counting sink to isolate queueing and synchronization costs from disk I/O
- File sink target uses the real `hlog::FileSink` path and includes filesystem effects

## Environment

- System: `{environment_info.system}`
- Platform: `{environment_info.platform_string}`
- Machine: `{environment_info.machine}`
- Hardware model: `{environment_info.hardware_model}`
- CPU: `{environment_info.cpu_model}`
- Hardware threads: `{environment_info.hardware_threads}`
- Memory: `{environment_info.memory}`
- Compiler: `{environment_info.compiler_version}`
- Compiler path: `{environment_info.compiler_path}`
- Build type: `{environment_info.build_type}`
- CMake generator: `{environment_info.generator}`
- Build flags: `{environment_info.build_flags}`

## Throughput

![Throughput by Threads](perf/throughput.svg)

| Threads | `mutex + condition_variable` | `CAS` ring buffer | Improvement |
| --- | ---: | ---: | ---: |
{throughput_table}

Raw CSV: [docs/perf/throughput.csv](perf/throughput.csv)

## Producer-Side Mean Latency

![Mean Call Latency by Threads](perf/mean-latency.svg)

## Producer-Side P99 Latency

![P99 Call Latency by Threads](perf/p99-latency.svg)

| Threads | Mutex mean (`us`) | CAS mean (`us`) | Mutex p95 (`us`) | CAS p95 (`us`) | Mutex p99 (`us`) | CAS p99 (`us`) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
{latency_table}

Raw CSV: [docs/perf/latency.csv](perf/latency.csv)

## Payload Hot Path

This section keeps the same `hlog::AsyncLogger` and the same in-memory sink, and changes only how the producer builds the payload:

- `prebuilt_*`: build a `std::string` with `std::ostringstream` before calling `Info()`, approximating the previous hot path.
- `variadic_short`: call `Info("thread=", tid, " seq=", index)` directly; the common-case payload stays within `LogPayload` inline storage.
- `variadic_long`: call `Info(..., " payload=", <512B blob>)` directly; the payload exceeds inline storage and exercises spillover.

### Short Payload Mean Latency

![Short Payload Mean Log() Latency](perf/payload-short-mean-latency.svg)

### Short Payload P99 Latency

![Short Payload P99 Log() Latency](perf/payload-short-p99-latency.svg)

| Threads | Prebuilt mean (`us`) | Variadic mean (`us`) | Mean improvement | Prebuilt p99 (`us`) | Variadic p99 (`us`) | P99 improvement |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
{payload_short_table}

### Long Payload Mean Latency

![Long Payload Mean Log() Latency](perf/payload-long-mean-latency.svg)

### Long Payload P99 Latency

![Long Payload P99 Log() Latency](perf/payload-long-p99-latency.svg)

| Threads | Prebuilt mean (`us`) | Variadic mean (`us`) | Mean improvement | Prebuilt p99 (`us`) | Variadic p99 (`us`) | P99 improvement |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
{payload_long_table}

Raw CSV: [docs/perf/payload-latency.csv](perf/payload-latency.csv)

## File Sink Throughput

This section uses the real `FileSink` path rather than the in-memory counting sink, and compares:

- `unbatched_file_sink`: `max_batch_size=1`, approximating per-message file writes.
- `batched_file_sink`: `max_batch_size=64 KiB` and `flush_interval=250 ms`, which batches multiple formatted log lines into one contiguous write.

Because this benchmark includes actual filesystem behavior, it is noisier than the in-memory queue benchmarks and should be interpreted as a local-machine systems optimization signal rather than a portable absolute number.

![File Sink Throughput by Threads](perf/file-sink-throughput.svg)

| Threads | Unbatched file sink | Batched file sink | Improvement |
| --- | ---: | ---: | ---: |
{file_sink_table}

Raw CSV: [docs/perf/file-sink-throughput.csv](perf/file-sink-throughput.csv)

## Notes

- The latency benchmark wraps only the producer-side `Log()` call, so it reflects direct caller overhead rather than end-to-end flush latency.
- The measurements include `steady_clock` sampling cost; treat them as relative comparisons rather than absolute instruction-level timings.
- Because the report uses medians over multiple rounds, it is more stable than quoting a single best run from the README.
- The single-thread case may still favor the mutex baseline, because there is no contention yet and the lock-free bookkeeping has a fixed overhead. The lock-free advantage appears once producer contention becomes the bottleneck.
- The payload benchmark is not a mutex-vs-lock-free comparison; it is a same-logger A/B test for producer-side payload construction overhead.
- The file sink benchmark is intentionally a separate section, because it mixes queueing, formatting, filesystem buffering, and storage behavior; it answers a different question from the in-memory sink benchmarks.
"""
    output_path.write_text(content, encoding="utf-8")


def main() -> None:
    args = parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    environment_info = collect_environment_info(args)
    throughput_rows = collect_throughput_rows(args)
    latency_rows = collect_latency_rows(args)
    payload_latency_rows = collect_payload_latency_rows(args)
    file_sink_rows = collect_file_sink_rows(args)

    write_csv(
        output_dir / "throughput.csv",
        [
            "threads",
            "mutex_median_throughput_msgs_per_sec",
            "cas_median_throughput_msgs_per_sec",
            "improvement_percent",
        ],
        [
            [
                row.threads,
                f"{row.mutex_median_throughput:.6f}",
                f"{row.cas_median_throughput:.6f}",
                f"{row.improvement_percent:.6f}",
            ]
            for row in throughput_rows
        ],
    )

    write_csv(
        output_dir / "latency.csv",
        [
            "threads",
            "mutex_median_mean_call_ns",
            "cas_median_mean_call_ns",
            "mutex_median_p95_call_ns",
            "cas_median_p95_call_ns",
            "mutex_median_p99_call_ns",
            "cas_median_p99_call_ns",
        ],
        [
            [
                row.threads,
                f"{row.mutex_mean_ns:.6f}",
                f"{row.cas_mean_ns:.6f}",
                row.mutex_p95_ns,
                row.cas_p95_ns,
                row.mutex_p99_ns,
                row.cas_p99_ns,
            ]
            for row in latency_rows
        ],
    )

    write_csv(
        output_dir / "payload-latency.csv",
        [
            "threads",
            "prebuilt_short_median_mean_call_ns",
            "variadic_short_median_mean_call_ns",
            "prebuilt_short_median_p99_call_ns",
            "variadic_short_median_p99_call_ns",
            "prebuilt_long_median_mean_call_ns",
            "variadic_long_median_mean_call_ns",
            "prebuilt_long_median_p99_call_ns",
            "variadic_long_median_p99_call_ns",
        ],
        [
            [
                row.threads,
                f"{row.prebuilt_short_mean_ns:.6f}",
                f"{row.variadic_short_mean_ns:.6f}",
                row.prebuilt_short_p99_ns,
                row.variadic_short_p99_ns,
                f"{row.prebuilt_long_mean_ns:.6f}",
                f"{row.variadic_long_mean_ns:.6f}",
                row.prebuilt_long_p99_ns,
                row.variadic_long_p99_ns,
            ]
            for row in payload_latency_rows
        ],
    )

    write_csv(
        output_dir / "file-sink-throughput.csv",
        [
            "threads",
            "unbatched_median_throughput_msgs_per_sec",
            "batched_median_throughput_msgs_per_sec",
            "improvement_percent",
        ],
        [
            [
                row.threads,
                f"{row.unbatched_median_throughput:.6f}",
                f"{row.batched_median_throughput:.6f}",
                f"{row.improvement_percent:.6f}",
            ]
            for row in file_sink_rows
        ],
    )

    threads = [row.threads for row in throughput_rows]
    render_line_chart(
        path=output_dir / "throughput.svg",
        title="Median Throughput by Thread Count",
        y_label="Messages per second",
        x_values=threads,
        series=[
            (
                "mutex + condition_variable",
                "#c84c09",
                [row.mutex_median_throughput for row in throughput_rows],
            ),
            (
                "CAS ring buffer",
                "#007f5f",
                [row.cas_median_throughput for row in throughput_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "mean-latency.svg",
        title="Median Producer-Side Mean Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "mutex + condition_variable",
                "#c84c09",
                [ns_to_us(row.mutex_mean_ns) for row in latency_rows],
            ),
            (
                "CAS ring buffer",
                "#007f5f",
                [ns_to_us(row.cas_mean_ns) for row in latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "p99-latency.svg",
        title="Median Producer-Side P99 Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "mutex + condition_variable",
                "#c84c09",
                [ns_to_us(row.mutex_p99_ns) for row in latency_rows],
            ),
            (
                "CAS ring buffer",
                "#007f5f",
                [ns_to_us(row.cas_p99_ns) for row in latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "payload-short-mean-latency.svg",
        title="Short Payload Mean Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "prebuilt string via ostringstream",
                "#c84c09",
                [ns_to_us(row.prebuilt_short_mean_ns) for row in payload_latency_rows],
            ),
            (
                "direct variadic inline payload",
                "#007f5f",
                [ns_to_us(row.variadic_short_mean_ns) for row in payload_latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "payload-short-p99-latency.svg",
        title="Short Payload P99 Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "prebuilt string via ostringstream",
                "#c84c09",
                [ns_to_us(row.prebuilt_short_p99_ns) for row in payload_latency_rows],
            ),
            (
                "direct variadic inline payload",
                "#007f5f",
                [ns_to_us(row.variadic_short_p99_ns) for row in payload_latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "payload-long-mean-latency.svg",
        title="Long Payload Mean Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "prebuilt string via ostringstream",
                "#c84c09",
                [ns_to_us(row.prebuilt_long_mean_ns) for row in payload_latency_rows],
            ),
            (
                "direct variadic spillover payload",
                "#007f5f",
                [ns_to_us(row.variadic_long_mean_ns) for row in payload_latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "payload-long-p99-latency.svg",
        title="Long Payload P99 Log() Latency",
        y_label="Microseconds",
        x_values=threads,
        series=[
            (
                "prebuilt string via ostringstream",
                "#c84c09",
                [ns_to_us(row.prebuilt_long_p99_ns) for row in payload_latency_rows],
            ),
            (
                "direct variadic spillover payload",
                "#007f5f",
                [ns_to_us(row.variadic_long_p99_ns) for row in payload_latency_rows],
            ),
        ],
    )
    render_line_chart(
        path=output_dir / "file-sink-throughput.svg",
        title="Median File Sink Throughput by Thread Count",
        y_label="Messages per second",
        x_values=[row.threads for row in file_sink_rows],
        series=[
            (
                "unbatched file sink",
                "#c84c09",
                [row.unbatched_median_throughput for row in file_sink_rows],
            ),
            (
                "batched file sink",
                "#007f5f",
                [row.batched_median_throughput for row in file_sink_rows],
            ),
        ],
    )

    write_markdown_report(
        output_path=Path("docs/perf.md"),
        throughput_rows=throughput_rows,
        latency_rows=latency_rows,
        payload_latency_rows=payload_latency_rows,
        file_sink_rows=file_sink_rows,
        environment_info=environment_info,
        args=args,
    )


if __name__ == "__main__":
    main()
