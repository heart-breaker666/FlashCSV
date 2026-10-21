#!/usr/bin/env python3
"""Release benchmark matrix with three repeats, output validation and peak RSS."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import statistics
import subprocess
import tempfile
from zoneinfo import ZoneInfo

from generate_data import DEFAULT_SEED, ROOT, generate


def verify(path, expected):
    seen = 0
    previous = None
    with path.open(newline="") as source:
        reader = csv.reader(source)
        if next(reader) != ["key", "count", "sum(amount)", "min(amount)", "max(amount)", "avg(amount)"]:
            raise ValueError("unexpected benchmark output header")
        for row in reader:
            key, count, *values = row
            if previous is not None and previous >= key:
                raise ValueError("unsorted or duplicate result key")
            previous = key
            wanted = expected["states"][key]
            if int(count) != wanted[0]:
                raise ValueError(f"count mismatch: {key}")
            reference = [wanted[1] / 100, wanted[2] / 100, wanted[3] / 100, wanted[1] / (100 * wanted[0])]
            for value, target in zip(values, reference):
                actual = float(value)
                if not math.isfinite(actual) or abs(actual - target) > 1e-9 * max(1, abs(actual), abs(target)):
                    raise ValueError(f"numeric mismatch: {key}: {actual}, {target}")
            seen += 1
    if seen != expected["groups"]:
        raise ValueError("group count mismatch")


def command_output(command):
    return subprocess.check_output(command, text=True).strip()


def memory_bytes():
    match = re.search(r"^MemTotal:\s+(\d+) kB", Path("/proc/meminfo").read_text(), re.MULTILINE)
    return int(match[1]) * 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/release/flash_csv")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--large", action="store_true", help="also test input larger than machine RAM, if disk allows")
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error("at least three repeats required")
    binary = args.binary.resolve()
    cache = binary.parent / "CMakeCache.txt"
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache.read_text():
        parser.error("a Release build is required")
    reports = ROOT / "reports"
    reports.mkdir(exist_ok=True)
    stamp = datetime.now(ZoneInfo("Asia/Shanghai")).strftime("%Y%m%d-%H%M%S")
    directory = Path(tempfile.mkdtemp(prefix=f"benchmark-{stamp}-", dir=reports))
    machine = {"timestamp": stamp, "platform": platform.platform(), "cpu": command_output(["lscpu"]),
               "memory_bytes": memory_bytes(), "filesystem": command_output(["df", "-T", str(ROOT)]),
               "compiler": command_output(["c++", "--version"]).splitlines()[0],
               "cmake": command_output(["cmake", "--version"]).splitlines()[0],
               "binary": str(binary), "seed": DEFAULT_SEED, "repeats": args.repeats,
               "cache": "No cache eviction; generated inputs may be cached; large input exceeds total RAM.",
               "limits": {"record_bytes": 65536, "batch_bytes": 1048576, "batch_rows": 4096,
                          "queue_batches": 4, "max_groups": 100000}}
    machine["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    machine["release_flags"] = (binary.parent / "CMakeFiles/flash_csv_core.dir/flags.make").read_text()
    machine["core_source_sha256"] = {
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for folder in (ROOT / "src", ROOT / "include/flash_csv") for path in folder.iterdir() if path.is_file()}
    (directory / "environment.json").write_text(json.dumps(machine, indent=2) + "\n")
    datasets = [(100000, 1000, 0), (1000000, 1000, 0), (10000000, 1000, 0),
                (1000000, 10, 0), (1000000, 100000, 0)]
    large_note = "未请求大于机器内存的规模。"
    if args.large:
        padding = max(512, math.ceil(machine["memory_bytes"] * 1.15 / 10000000))
        estimate = 10000000 * (padding + 32)
        if shutil.disk_usage(ROOT).free > estimate * 1.1 + 512 * 1024**2:
            datasets.append((10000000, 1000, padding))
            large_note = "大于机器内存的输入已实测（见表格），引擎地址空间限制为 256 MiB。"
        else:
            large_note = f"磁盘余量不足，未实测大于机器内存的输入（估计需 {estimate / 1024**3:.2f} GiB）。"
    summaries = []
    all_runs = []
    for rows, groups, padding in datasets:
        source, expected = generate(rows, groups, padding=padding)
        for threads in ((1,) if padding else (1, 2, 4)):
            runs = []
            for repeat in range(args.repeats):
                identity = f"r{rows}-g{groups}-p{padding}-t{threads}-{repeat}"
                output = directory / (identity + ".csv")
                rss = directory / (identity + ".rss")
                command = ["/usr/bin/time", "-f", "%M", "-o", str(rss), str(binary), "aggregate", str(source),
                           "--output", str(output), "--group-by", "key", "--agg", "count", "--agg", "sum", "amount",
                           "--agg", "min", "amount", "--agg", "max", "amount", "--agg", "avg", "amount",
                           "--threads", str(threads)]

                def constrain():
                    resource.setrlimit(resource.RLIMIT_AS, (256 * 1024**2, 256 * 1024**2))

                result = subprocess.run(command, capture_output=True, text=True, timeout=180,
                                        preexec_fn=constrain if padding else None)
                (directory / (identity + ".stderr")).write_text(result.stderr)
                if result.returncode:
                    raise RuntimeError(f"benchmark failed: {result.stderr}")
                verify(output, expected)
                metrics = dict(re.findall(r"(\S+)=(\S+)", result.stderr))
                if int(metrics["read_rows"]) != rows or int(metrics["matched_rows"]) != rows:
                    raise ValueError("statistics row count mismatch")
                if int(metrics["input_bytes"]) != expected["bytes"] or int(metrics["output_rows"]) != groups:
                    raise ValueError("statistics byte/output count mismatch")
                run = {"rows": rows, "groups": groups, "padding": padding, "threads": threads,
                       "repeat": repeat, "seconds": float(metrics["seconds"]), "MiB/s": float(metrics["MiB/s"]),
                       "rss_kib": int(rss.read_text()), "input_bytes": expected["bytes"],
                       "source_sha256": expected["sha256"], "verified": True}
                runs.append(run)
                all_runs.append(run)
                (directory / "runs.json").write_text(json.dumps(all_runs, indent=2) + "\n")
                print(f"{identity}: {run['seconds']:.3f}s, {run['MiB/s']:.2f} MiB/s, "
                      f"RSS {run['rss_kib'] / 1024:.2f} MiB, verified", flush=True)
            summaries.append({"rows": rows, "groups": groups, "padding": padding, "threads": threads,
                              "input_mib": expected["bytes"] / 1024**2,
                              "median_seconds": statistics.median(run["seconds"] for run in runs),
                              "median_mib_s": statistics.median(run["MiB/s"] for run in runs),
                              "median_rss_mib": statistics.median(run["rss_kib"] for run in runs) / 1024,
                              "peak_rss_mib": max(run["rss_kib"] for run in runs) / 1024})
    (directory / "environment.json").write_text(json.dumps(machine, indent=2) + "\n")
    (directory / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    cpu = re.search(r"^(?:Model name|型号名称)：?\s*:?\s*(.+)$", machine["cpu"], re.MULTILINE)
    lines = ["# FlashCSV Release 基准", "", f"日期：{stamp[:8]}（Asia/Shanghai）。固定种子 {DEFAULT_SEED}，每配置 {args.repeats} 次。",
             "", f"CPU：{cpu[1] if cpu else '见 environment.json'}；可见逻辑 CPU：{os.cpu_count()}；"
             f"总内存：{machine['memory_bytes'] / 1024**3:.2f} GiB。", "",
             f"编译器：{machine['compiler']}；Release 默认优化；存储与虚拟化详情见 environment.json。",
             "输入生成后直接测量，未清理系统缓存，未隔离其他进程；结果不代表冷盘性能。",
             "耗时为引擎打开输入至输出发布；RSS 为 GNU time 子进程峰值；吞吐包含输入表头。", "",
             "所有输出均与独立整数分单位的预期比较：COUNT 精确；浮点按设计容差验证。", "",
             "| 行数 | 分组数 | 填充字节/行 | 输入 MiB | 线程 | 中位秒 | 中位 MiB/s | 中位 RSS MiB | 最大 RSS MiB |",
             "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in summaries:
        lines.append(f"| {item['rows']:,} | {item['groups']:,} | {item['padding']} | {item['input_mib']:.2f} | "
                     f"{item['threads']} | {item['median_seconds']:.3f} | {item['median_mib_s']:.2f} | "
                     f"{item['median_rss_mib']:.2f} | {item['peak_rss_mib']:.2f} |")
    lines += ["", large_note, "", "原始测量、stderr、结果文件、环境与汇总均保留在本目录；大数据仅在 data/。",
              "重复运行：`python3 tools/benchmark.py --large`（使用已构建的 build/release/flash_csv）。", ""]
    (directory / "summary.md").write_text("\n".join(lines))
    print(f"Report: {directory / 'summary.md'}", flush=True)


if __name__ == "__main__":
    main()
