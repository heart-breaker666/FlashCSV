#!/usr/bin/env python3
"""CLI acceptance, malformed input, bounded concurrency and real I/O failures."""
import csv
import io
import json
import math
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve()
WORK = Path(sys.argv[2]).resolve()
WORK.mkdir(parents=True, exist_ok=True)
checks = 0


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def invoke(directory, source, mode="export", args=(), code=0, kind=None,
           error_line=None, preexec_fn=None):
    global checks
    checks += 1
    output = directory / f"result-{checks}.csv"
    result = subprocess.run([str(BINARY), mode, str(source), "--output", str(output), *args],
                            capture_output=True, text=True, timeout=8, preexec_fn=preexec_fn)
    require(result.returncode == code, f"{args}: exit {result.returncode}, expected {code}: {result.stderr}")
    if code:
        require(not output.exists(), f"failure published output: {args}")
        require("read_rows=" not in result.stderr, "failure printed success statistics")
        if kind:
            require(kind in result.stderr, f"missing error kind {kind}: {result.stderr}")
        if error_line:
            require(re.search(rf"\bline {error_line}\b", result.stderr), result.stderr)
        require(not list(directory.glob(".flash_csv-*")), "failure left temporary file")
        return None
    require(output.is_file(), "missing success output")
    require("read_rows=" in result.stderr and "MiB/s=" in result.stderr, "missing statistics")
    require(f"input_bytes={source.stat().st_size}" in result.stderr, "wrong throughput byte count")
    content = output.read_bytes()
    require(b"\r" not in content, "output must use LF")
    return list(csv.reader(io.StringIO(content.decode("utf-8"))))


def equal_rows(actual, expected):
    require(actual[0] == expected["header"], f"header: {actual}")
    require(len(actual) - 1 == len(expected["rows"]), f"row count: {actual}")
    numeric = set(expected.get("numeric_columns", []))
    for got, wanted in zip(actual[1:], expected["rows"]):
        require(len(got) == len(wanted), f"width: {got}, {wanted}")
        for i, (a, b) in enumerate(zip(got, wanted)):
            if i in numeric and b != "":
                x, y = float(a), float(b)
                require(math.isfinite(x) and abs(x - y) <= 1e-9 * max(1, abs(x), abs(y)), f"number: {a}, {b}")
            else:
                require(a == b, f"field: {got}, {wanted}")


def fixture_cases(directory):
    cases = json.loads((ROOT / "tests/fixtures/cases.json").read_text())["cases"]
    for case in cases:
        expected = case["expected"]
        source = ROOT / "tests/fixtures" / case["input"]
        variants = [case["args"]]
        if case["mode"] == "aggregate":
            variants += [case["args"] + ["--threads", str(n), "--batch-rows", "1", "--queue-batches", "1"]
                         for n in (2, 4)]
        for args in variants:
            actual = invoke(directory, source, case["mode"], args,
                            expected["exit_code"], expected.get("error_kind"), expected.get("error_line"))
            if actual:
                equal_rows(actual, expected)


def extra_cases(directory):
    source = directory / "extra.csv"

    def write(data):
        source.write_bytes(data if isinstance(data, bytes) else data.encode())

    write("key,n,other\nA,1,10\nA,2,20\n,3,30\nB,bad,40\n")
    for threads in (1, 2, 4):
        actual = invoke(directory, source, "aggregate", ["--agg", "sum", "n", "--agg", "sum", "other",
                        "--where", "key", "str", "ne", "B", "--threads", str(threads), "--batch-rows", "1"])
        require(actual == [["sum(n)", "sum(other)"], ["6", "60"]], "different numeric columns / aggregate short circuit")
    valid = ["0", "-0", "1.", ".5", "-.5", "1e+2", "-2E-3", "1e-300", "1e300"]
    invalid = ["", "+1", " 1", "1 ", "0x10", "nan", "NaN", "inf", "Infinity", "-inf", ".", "-", "1e", "1e+",
               "1e9999", "1e-9999", "1x", "1,2"]
    for value in valid + invalid:
        encoded = io.StringIO()
        writer = csv.writer(encoded, lineterminator="\n")
        writer.writerow(["n"])
        writer.writerow([value])
        write(encoded.getvalue())
        invoke(directory, source, "export", ["--where", "n", "num", "eq", value],
               2 if value in invalid else 0, "invalid_number" if value in invalid else None)
        if value in invalid:
            invoke(directory, source, "aggregate", ["--agg", "sum", "n"], 1, "invalid_number", 2)
    for op, expected in [("eq", ["2"]), ("ne", ["1", "3"]), ("gt", ["3"]),
                         ("ge", ["2", "3"]), ("lt", ["1"]), ("le", ["1", "2"])]:
        write("n\n1\n2\n3\n")
        actual = invoke(directory, source, args=["--where", "n", "num", op, "2"])
        require(actual[1:] == [[n] for n in expected], "comparison " + op)
    write("--column\n--help\n--output\n")
    actual = invoke(directory, source, args=["--select", "--column", "--where", "--column", "str", "eq", "--help"])
    require(actual == [["--column"], ["--help"]], "option-like column/value must retain string semantics")
    write("n\n1e308\n1e308\n")
    invoke(directory, source, "aggregate", ["--agg", "sum", "n"], 1, "sum_overflow")
    require(invoke(directory, source, "aggregate", ["--agg", "min", "n"])[1] == ["1e+308"], "min must not sum")

    for data, kind, line in [("a\nx\"\n", "csv_syntax", 2), ("a\n\"x", "csv_syntax", 2),
                             ("a\r\n1\r", "csv_syntax", 2), ("a\n\"x\r\ny\"", "unsupported_multiline", 2),
                             ("a,b\n\n", "field_count", 2), ("a\n\"x\" \n", "csv_syntax", 2)]:
        write(data)
        invoke(directory, source, code=1, kind=kind, error_line=line)
    write("a\n\n")
    require(invoke(directory, source) == [["a"], [""]], "blank physical row")
    for ending in ("", "\n", "\r\n"):
        write("a\n1234" + ending)
        invoke(directory, source, args=["--max-record-bytes", "4", "--batch-bytes", "4"])
        write("a\n12345" + ending)
        invoke(directory, source, args=["--max-record-bytes", "4", "--batch-bytes", "4"], code=1, kind="record_limit")
    write(b"\xef\xbb\xbf1234\n")
    invoke(directory, source, args=["--max-record-bytes", "4", "--batch-bytes", "4"])
    write("12345\n")
    invoke(directory, source, args=["--max-record-bytes", "4", "--batch-bytes", "4"], code=1, kind="record_limit", error_line=1)
    write("a\n" + "x" * 65536 + "\n")
    invoke(directory, source)
    write("a\n" + "x" * 65537 + "\n")
    invoke(directory, source, code=1, kind="record_limit", error_line=2)
    # Producer and workers both fail with a tiny queue; each subprocess is timed.
    write("n\nbad\n" + "1\n" * 10000)
    invoke(directory, source, "aggregate", ["--agg", "sum", "n", "--threads", "4",
           "--batch-rows", "1", "--queue-batches", "1"], 1, "invalid_number")
    write("n\n" + "1\n" * 10000 + '"broken\n')
    invoke(directory, source, "aggregate", ["--agg", "count", "--threads", "4",
           "--batch-rows", "1", "--queue-batches", "1"], 1, "unsupported_multiline")
    # Many batches and unequal worker populations, with exact count and tolerant floats.
    write("key,n\n" + "".join(f"g{i % 13},{(i % 19) / 10}\n" for i in range(12003)))
    baseline = None
    for threads in (1, 2, 4):
        actual = invoke(directory, source, "aggregate", ["--group-by", "key", "--agg", "count",
                        "--agg", "sum", "n", "--agg", "avg", "n", "--threads", str(threads),
                        "--batch-rows", "17", "--queue-batches", "1"])
        if baseline is None:
            baseline = {"header": actual[0], "rows": actual[1:], "numeric_columns": [2, 3]}
        else:
            equal_rows(actual, baseline)


def argument_cases(directory):
    source = ROOT / "tests/fixtures/sales.csv"
    bad_export = [["--wat"], ["--output", "extra"], ["--select"], ["--where", "city", "str", "gt", "x"],
                  ["--where", "city", "wrong", "eq", "x"], ["--where", "city", "str", "eq"],
                  ["--select", "city", "--select", "city"], ["--select", "missing"], ["--agg", "count"],
                  ["--group-by", "city"], ["--threads", "1"], ["--max-groups", "1"],
                  ["--max-record-bytes", "2000000"], ["--batch-rows", "0"], ["--queue-batches", "-1"],
                  ["--batch-bytes", "1x"], ["--batch-rows", "+1"], ["--batch-rows", "18446744073709551616"],
                  ["--batch-rows", "1", "--batch-rows", "2"], ["extra.csv"]]
    for args in bad_export:
        invoke(directory, source, args=args, code=2)
    bad_aggregate = [[], ["--agg", "sum"], ["--agg", "median", "amount"],
                     ["--agg", "count", "--agg", "count"], ["--agg", "sum", "amount", "--agg", "sum", "amount"],
                     ["--agg", "count", "--threads", "33"], ["--agg", "count", "--threads", "0"],
                     ["--agg", "count", "--select", "city"], ["--agg", "count", "--group-by", "city", "--group-by", "city"],
                     ["--agg", "sum", "missing"]]
    for args in bad_aggregate:
        invoke(directory, source, "aggregate", args, 2)
    collision = directory / "collision.csv"
    collision.write_text("count,n\nx,1\n")
    invoke(directory, collision, "aggregate", ["--group-by", "count", "--agg", "count"], 2, "duplicate")
    help_result = subprocess.run([str(BINARY), "--help"], capture_output=True, text=True, timeout=8)
    require(help_result.returncode == 0 and "65536" in help_result.stdout and "100000" in help_result.stdout, "help defaults")
    for args in ([], ["bad", str(source)], ["export", str(source)]):
        result = subprocess.run([str(BINARY), *args], capture_output=True, timeout=8)
        require(result.returncode == 2, "missing/invalid command arguments")


def io_cases(directory):
    source = directory / "io.csv"
    source.write_text("n\n" + "abcdef\n" * 15000)
    output = directory / "preserve.csv"
    output.write_text("sentinel\n")
    for target, code in ((output, 1), (source, 2)):
        before = target.read_bytes()
        result = subprocess.run([str(BINARY), "export", str(source), "--output", str(target)], capture_output=True, timeout=8)
        require(result.returncode == code and target.read_bytes() == before, "existing or identical output protection")
    alias = directory / "alias.csv"
    os.link(source, alias)
    result = subprocess.run([str(BINARY), "export", str(source), "--output", str(alias)], capture_output=True, timeout=8)
    require(result.returncode == 2, "hardlink same input")
    broken = directory / "broken.csv"
    broken.symlink_to(directory / "nonexistent.csv")
    result = subprocess.run([str(BINARY), "export", str(source), "--output", str(broken)], capture_output=True, timeout=8)
    require(result.returncode == 1 and broken.is_symlink(), "dangling symlink protection")
    invoke(directory, directory / "absent.csv", code=1, kind="io")
    invoke(directory, directory, code=1, kind="io")
    missing_dir = directory / "missing"
    result = subprocess.run([str(BINARY), "export", str(source), "--output", str(missing_dir / "out.csv")],
                            capture_output=True, timeout=8)
    require(result.returncode == 1 and not missing_dir.exists(), "output directory must already exist")

    def limited_file():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

    invoke(directory, source, code=1, kind="io", preexec_fn=limited_file)
    # Small output reaches the same actual write error during final flush.
    def zero_file():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

    invoke(directory, ROOT / "tests/fixtures/sales.csv", code=1, kind="io", preexec_fn=zero_file)


with tempfile.TemporaryDirectory(prefix="acceptance-", dir=WORK) as temporary:
    directory = Path(temporary)
    fixture_cases(directory)
    extra_cases(directory)
    argument_cases(directory)
    io_cases(directory)
print(f"All 28 fixtures and extended acceptance passed ({checks} engine invocations plus CLI/I/O checks)")
