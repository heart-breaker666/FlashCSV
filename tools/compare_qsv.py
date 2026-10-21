#!/usr/bin/env python3
"""Compare FlashCSV and an official qsv binary on identical, verified CSV tasks."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import tempfile
import time
from zoneinfo import ZoneInfo

from benchmark import verify as verify_aggregate
from generate_data import DEFAULT_SEED, ROOT, generate


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def output(command):
    return subprocess.check_output(command, text=True).strip()


def cents(number):
    sign = -1 if number.startswith("-") else 1
    whole, fraction = number.lstrip("-").split(".")
    return sign * (int(whole) * 100 + int(fraction))


def export_reference(source, directory, filtered):
    """Independent CSV oracle, including selected field contents and input order."""
    target = directory / (source.stem + ("-filter" if filtered else "-select") + ".expected.csv")
    rows = 0
    with source.open(newline="") as stream, target.open("w", newline="") as result:
        reader = csv.DictReader(stream)
        writer = csv.writer(result, lineterminator="\n")
        writer.writerow(["key", "amount"])
        for row in reader:
            if not filtered or cents(row["amount"]) > 50000:
                writer.writerow([row["key"], row["amount"]])
                rows += 1
    return target, rows


def verify_export(actual, reference):
    # select preserves strings; SQL parses amount as Float64 and may reformat it.
    rows = 0
    with actual.open(newline="") as a, reference.open(newline="") as b:
        left, right = csv.reader(a), csv.reader(b)
        if next(left) != next(right):
            raise ValueError("export header mismatch")
        for expected in right:
            row = next(left, None)
            if row is None or len(row) != 2 or row[0] != expected[0]:
                raise ValueError(f"export row/key mismatch at row {rows + 1}")
            value = float(row[1])
            wanted = cents(expected[1]) / 100
            if not math.isfinite(value) or abs(value - wanted) > 1e-9 * max(1, abs(value), abs(wanted)):
                raise ValueError(f"export amount mismatch at row {rows + 1}")
            rows += 1
        if next(left, None) is not None:
            raise ValueError("extra export rows")
    return rows


def command_for(engine, workload, source, target, threads, flash, qsv, streaming):
    if engine == "flash_csv":
        if workload.startswith("export"):
            command = [str(flash), "export", str(source), "--output", str(target),
                       "--select", "key", "--select", "amount"]
            if workload == "export-filter":
                command += ["--where", "amount", "num", "gt", "500"]
            return command
        return [str(flash), "aggregate", str(source), "--output", str(target),
                "--group-by", "key", "--agg", "count", "--agg", "sum", "amount",
                "--agg", "min", "amount", "--agg", "max", "amount", "--agg", "avg", "amount",
                "--threads", str(threads)]
    if workload == "export-select":
        return [str(qsv), "select", "key,amount", str(source), "--output", str(target)]
    if workload == "export-filter":
        query = "SELECT key, amount FROM _t_1 WHERE amount > 500"
    else:
        query = ('SELECT key, COUNT(*) AS count, SUM(amount) AS "sum(amount)", '
                 'MIN(amount) AS "min(amount)", MAX(amount) AS "max(amount)", '
                 'AVG(amount) AS "avg(amount)" FROM _t_1 GROUP BY key ORDER BY key')
    command = [str(qsv), "sqlp", str(source), query, "--output", str(target)]
    if streaming:
        command.append("--streaming")
    return command


def run(command, env, directory, identity, timeout):
    stderr = directory / (identity + ".stderr")
    metrics = directory / (identity + ".time.json")
    wrapper = ["/usr/bin/time", "-f", '{"wall_seconds":%e,"user_seconds":%U,"system_seconds":%S,"rss_kib":%M}',
               "-o", str(metrics)] + command
    started = time.perf_counter()
    with stderr.open("w") as errors:
        process = subprocess.Popen(wrapper, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                   stderr=errors, start_new_session=True)
        try:
            process.communicate(timeout=timeout)
            returncode = process.returncode
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(f"timeout after {timeout}s: {identity}")
    seconds = time.perf_counter() - started
    if returncode:
        raise RuntimeError(f"exit {returncode}: {identity}: {stderr.read_text()[-3000:]}")
    measured = json.loads(metrics.read_text())
    measured.update(seconds=seconds, command=command, returncode=returncode)
    return measured


def write_summary(directory, metadata, runs):
    keys = sorted({(r["workload"], r["rows"], r["groups"], r["padding"], r["threads"], r["engine"], r["streaming"])
                   for r in runs if not r["warmup"]})
    summaries = []
    for key in keys:
        workload, rows, groups, padding, threads, engine, streaming = key
        samples = [r for r in runs if not r["warmup"] and
                   (r["workload"], r["rows"], r["groups"], r["padding"], r["threads"], r["engine"], r["streaming"]) == key]
        seconds = statistics.median(r["seconds"] for r in samples)
        summaries.append(dict(workload=workload, rows=rows, groups=groups, padding=padding,
                              threads=threads, engine=engine, streaming=streaming, repeats=len(samples),
                              median_seconds=seconds, input_mib=samples[0]["input_bytes"] / 1024**2,
                              min_seconds=min(r["seconds"] for r in samples),
                              max_seconds=max(r["seconds"] for r in samples),
                              median_mib_s=samples[0]["input_bytes"] / 1024**2 / seconds,
                              median_rss_mib=statistics.median(r["rss_kib"] for r in samples) / 1024,
                              peak_rss_mib=max(r["rss_kib"] for r in samples) / 1024,
                              output_rows=samples[0]["output_rows"]))
    save(directory / "summary.json", summaries)
    lines = ["# FlashCSV 与 qsv 对比实测", "", f"日期：{metadata['timestamp']}（Asia/Shanghai）。",
             f"共 {len(summaries)} 个配置、{sum(s['repeats'] for s in summaries)} 次正式测量，全部通过结果校验。",
             f"qsv {metadata['release_tag']}：官方 Linux GNU 发行包（PGO）；完整版本见 environment.json。",
             "FlashCSV：当前源码重新构建的 Release（编译选项和 SHA-256 见 environment.json）。", "",
             "## 源码与运行环境", "",
             "qsv 源码位于 /data/fangdashuai/mycode/qsv，来自官方 24.0.0 源码归档；常规 Git 传输失败，未取得 Git 历史。",
             "源码归档原始提交为 e5f5ba94d2f478a0061c7ab17c449f16a4490cd2；归档及二进制 SHA-256 已记录。",
             "本机没有 Rust 编译环境，未从源码构建 qsv；实际测试使用同版本官方发行包。",
             "系统运行库版本不足，qsv 通过 build/qsv/qsv-run 使用隔离的 Ubuntu 官方兼容库启动，未替换系统库。",
             "运行库版本、包来源及校验值见环境文件；此结果包含该启动方式的开销。", "",
             "## 方法与适用范围", "",
             f"固定种子 {DEFAULT_SEED}；每配置先预热一次，再测 {metadata['repeats']} 次，取中位数。",
             "同一机器、同一输入、相同筛选/选列/五种聚合及按键排序，输出均为磁盘 CSV。",
             "输入、导出预期的生成和校验不计时；每次运行包含进程启动、读取、计算和输出关闭。",
             "高精度总耗时由 Python perf_counter 测量，GNU time 另记墙钟、CPU 时间和子进程峰值 RSS。",
             "RSS 包含共享库和文件映射，不等于私有堆内存；FlashCSV 进程之外的系统文件缓存不计入其 RSS。",
             "各轮交替执行两种工具；未清系统缓存、未固定 CPU、未隔离其他进程，结果属于预热后测量。",
             "选列使用 qsv select；数值筛选和聚合使用 qsv sqlp（其内部执行引擎为 Polars）。",
             "qsv 的普通 SQL 任务保留默认解析设置；仅大于内存输入添加 --streaming。",
             "未测 qsv --low-memory 等其它调优组合；结果不代表 qsv 或 Polars 的最佳配置或所有数据形状。",
             "不创建 qsv 索引、统计缓存或模式缓存；类型推断的时间计入 qsv 耗时。",
             "FlashCSV 的线程数是聚合计算线程数，另有主线程读取；qsv 设置 POLARS_MAX_THREADS 和 QSV_MAX_JOBS。",
             "这两个线程参数并非相同总线程数或相同 CPU 限额；export-select 两者均为串行命令。",
             "COUNT 和分组精确校验，聚合浮点按设计容差核对独立整数分预期；导出逐行核对键、顺序及金额数值。",
             "qsv SQL 会重写数值文本，所以筛选导出比较的是字段数值，不要求 CSV 字节完全相同。",
             "每次输出校验后记录 SHA-256 并删除，保留命令、测量、日志和独立预期清单，避免重复占用磁盘。",
             "语法错误、空值、溢出等边界行为不在此次性能排名范围内；未运行 qsv 全量测试。", "",
             "## 全部测量中位值", "",
             "| 任务 | 行数 | 分组数 | 填充/行 | 工具 | 线程参数 | 中位秒 | 最小–最大秒 | MiB/s | 中位 RSS MiB | 最大 RSS MiB |",
             "|---|---:|---:|---:|---|---:|---:|---|---:|---:|---:|"]
    for s in summaries:
        name = s["engine"] + (" streaming" if s["streaming"] else "")
        lines.append(f"| {s['workload']} | {s['rows']:,} | {s['groups']:,} | {s['padding']} | {name} | "
                     f"{s['threads']} | {s['median_seconds']:.4f} | {s['min_seconds']:.4f}–{s['max_seconds']:.4f} | {s['median_mib_s']:.2f} | "
                     f"{s['median_rss_mib']:.2f} | {s['peak_rss_mib']:.2f} |")
    lines += ["", "## 相同配置速度比", "",
              "速度比 = FlashCSV 耗时 / qsv 耗时；大于 1 表示 qsv 更快，小于 1 表示 FlashCSV 更快。", "",
              "| 任务 | 行数 | 分组数 | 填充/行 | 线程参数 | FlashCSV 秒 | qsv 秒 | qsv 相对速度 |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for a in summaries:
        if a["engine"] != "flash_csv":
            continue
        b = next(s for s in summaries if s["engine"] == "qsv" and
                 all(s[k] == a[k] for k in ("workload", "rows", "groups", "padding", "threads")))
        lines.append(f"| {a['workload']} | {a['rows']:,} | {a['groups']:,} | {a['padding']} | {a['threads']} | "
                     f"{a['median_seconds']:.4f} | {b['median_seconds']:.4f} | "
                     f"{a['median_seconds']/b['median_seconds']:.2f}x |")
    lines += ["", "## 复现", "", "```bash", metadata["reproduce"], "```", "",
              "environment.json 保存机器、版本、二进制/源码和数据 SHA-256；runs.json 保存所有预热与测量。",
              "所有输出均经过校验。输入文件保存在项目 data/，构建和官方发行包保存在 build/qsv/。", ""]
    (directory / "summary.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, default=ROOT / "build/release/flash_csv")
    parser.add_argument("--qsv", type=Path, default=ROOT / "build/qsv/qsv-run")
    parser.add_argument("--qsv-source", type=Path, default=ROOT.parent / "qsv")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--large", action="store_true", help="include existing >RAM padded CSV")
    parser.add_argument("--smoke", action="store_true", help="only run 100k-row task checks")
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error("at least three timed repeats required")
    flash, qsv = args.flash.resolve(), args.qsv.resolve()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in (flash.parent / "CMakeCache.txt").read_text():
        parser.error("FlashCSV must use Release")
    stamp = datetime.now(ZoneInfo("Asia/Shanghai")).strftime("%Y%m%d-%H%M%S")
    directory = Path(tempfile.mkdtemp(prefix=f"comparison-qsv-{stamp}-", dir=ROOT / "reports"))
    env = os.environ.copy()
    # Prevent inherited tuning from silently changing the compared commands.
    for name in list(env):
        if name.startswith(("QSV_", "POLARS_")):
            del env[name]
    release = json.loads((ROOT / "build/qsv/release.json").read_text())
    source = args.qsv_source.resolve()
    source_origin_path = ROOT / "build/qsv/source-origin.json"
    source_origin = json.loads(source_origin_path.read_text()) if source_origin_path.is_file() else {}
    commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                            capture_output=True, text=True) if (source / ".git").exists() else None
    remote = subprocess.run(["git", "-C", str(source), "remote", "get-url", "origin"],
                            capture_output=True, text=True) if (source / ".git").exists() else None
    qsv_version = output([str(qsv), "--version"])
    if not qsv_version.startswith("qsv " + release["tag_name"] + "-"):
        parser.error("this comparison requires qsv " + release["tag_name"])
    metadata = dict(timestamp=stamp, repeats=args.repeats, platform=platform.platform(),
                    cpu=output(["lscpu"]), logical_cpus=os.cpu_count(), memory=Path("/proc/meminfo").read_text(),
                    filesystem=output(["df", "-T", str(ROOT)]),
                    flash_binary=str(flash), flash_sha256=digest(flash), qsv_binary=str(qsv), qsv_sha256=digest(qsv),
                    qsv_version=qsv_version, release_tag=release["tag_name"],
                    qsv_source=str(source), qsv_source_commit=commit.stdout.strip() if commit and commit.returncode == 0 else source_origin.get("commit"),
                    qsv_source_remote=remote.stdout.strip() if remote and remote.returncode == 0 else source_origin.get("repository"),
                    release_assets=[a for a in release["assets"] if "x86_64-unknown-linux-gnu.zip" in a["name"]],
                    flash_flags=(flash.parent / "CMakeFiles/flash_csv_core.dir/flags.make").read_text(),
                    flash_core_sha256={str(p.relative_to(ROOT)): digest(p) for folder in (ROOT/"src", ROOT/"include/flash_csv")
                                       for p in folder.iterdir() if p.is_file()},
                    harness_sha256=digest(Path(__file__).resolve()), generator_sha256=digest(ROOT/"tools/generate_data.py"),
                    cpu_affinity=sorted(os.sched_getaffinity(0)), datasets=[],
                    reproduce="python3 tools/compare_qsv.py" + (" --large" if args.large else "") +
                              (" --smoke" if args.smoke else ""))
    runtime_packages = ROOT / "build/qsv/runtime-packages.json"
    if runtime_packages.is_file():
        metadata["isolated_runtime_packages"] = json.loads(runtime_packages.read_text())
    if qsv.name == "qsv-run":
        metadata["qsv_launcher"] = qsv.read_text()
        metadata["qsv_actual_binary_sha256"] = digest(qsv.parent / "qsv")
    source_archive = ROOT / "build/qsv/qsv-24.0.0-source.tar.gz"
    if source_archive.is_file():
        metadata["qsv_source_archive_sha256"] = digest(source_archive)
    if source_origin:
        metadata["qsv_source_origin"] = source_origin
    for name in ("cpu.max", "memory.max"):
        path = Path("/sys/fs/cgroup") / name
        if path.is_file():
            metadata[name] = path.read_text().strip()
    save(directory / "environment.json", metadata)
    specifications = [(100000, 1000, 0)] if args.smoke else [
        (1000000, 1000, 0), (10000000, 1000, 0), (1000000, 10, 0), (1000000, 100000, 0)]
    if args.large and not args.smoke:
        padded = ROOT / "data" / f"rows-10000000-groups-1000-seed-{DEFAULT_SEED}-padding-512.csv"
        if not padded.is_file() or not padded.with_suffix(".json").is_file():
            parser.error("--large requires the existing 10M-row, padding-512 CSV and its manifest")
        specifications.append((10000000, 1000, 512))
    runs = []
    save(directory / "runs.json", runs)
    try:
        for rows, groups, padding in specifications:
            source, expected = generate(rows, groups, padding=padding)
            metadata["datasets"].append({k: expected[k] for k in ("rows", "groups", "padding", "seed", "bytes", "sha256")})
            save(directory / "environment.json", metadata)
            configs = [("aggregate", t) for t in ((1,) if padding or args.smoke else (1, 2, 4))]
            if groups == 1000 and not padding:
                configs = [("export-select", 1), ("export-filter", 1)] + configs
            for workload, threads in configs:
                reference, expected_rows = (None, groups)
                if workload.startswith("export"):
                    reference, expected_rows = export_reference(source, directory, workload == "export-filter")
                for repeat in range(-1, args.repeats):
                    engines = ["flash_csv", "qsv"] if repeat % 2 == 0 else ["qsv", "flash_csv"]
                    for engine in engines:
                        identity = f"{workload}-r{rows}-g{groups}-p{padding}-t{threads}-{engine}-" + ("warmup" if repeat < 0 else str(repeat))
                        target = directory / (identity + ".csv")
                        streaming = engine == "qsv" and bool(padding)
                        command = command_for(engine, workload, source, target, threads, flash, qsv, streaming)
                        run_env = dict(env, POLARS_MAX_THREADS=str(threads), QSV_MAX_JOBS=str(threads))
                        measured = run(command, run_env, directory, identity, args.timeout)
                        if workload == "aggregate":
                            verify_aggregate(target, expected)
                        elif verify_export(target, reference) != expected_rows:
                            raise ValueError("export row count mismatch")
                        record = dict(measured, workload=workload, rows=rows, groups=groups, padding=padding,
                                      threads=threads, engine=engine, streaming=streaming, repeat=repeat,
                                      warmup=repeat < 0, verified=True, output_rows=expected_rows,
                                      input_bytes=expected["bytes"], source_sha256=expected["sha256"],
                                      output_bytes=target.stat().st_size, output_sha256=digest(target),
                                      environment_overrides={k:run_env[k] for k in ("POLARS_MAX_THREADS", "QSV_MAX_JOBS")})
                        runs.append(record)
                        save(directory / "runs.json", runs)
                        target.unlink()
                        print(f"{identity}: {record['seconds']:.4f}s, RSS {record['rss_kib']/1024:.2f} MiB, verified", flush=True)
                if reference:
                    reference.unlink()
        write_summary(directory, metadata, runs)
    except Exception as error:
        save(directory / "failure.json", {"error":str(error), "completed_runs":len(runs)})
        print(f"Incomplete report: {directory}", flush=True)
        raise
    print(f"Report: {directory / 'summary.md'}", flush=True)


if __name__ == "__main__":
    main()
