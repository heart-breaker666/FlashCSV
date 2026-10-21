#!/usr/bin/env python3
"""Generate fixed-seed CSV and an independent integer-cent aggregation oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SEED = 20261012


def generate(rows, groups, seed=DEFAULT_SEED, padding=0):
    if not (rows > 0 and 0 < groups <= min(rows, 100000) and padding >= 0):
        raise ValueError("rows > 0, groups in 1..min(rows,100000), padding >= 0 required")
    directory = ROOT / "data"
    directory.mkdir(exist_ok=True)
    name = f"rows-{rows}-groups-{groups}-seed-{seed}-padding-{padding}"
    source = directory / (name + ".csv")
    manifest = directory / (name + ".json")
    if source.exists() and manifest.exists():
        expected = json.loads(manifest.read_text())
        if expected["bytes"] != source.stat().st_size:
            raise ValueError(f"existing dataset size mismatch: {source}")
        digest = hashlib.sha256()
        with source.open("rb") as existing:
            for block in iter(lambda: existing.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != expected["sha256"]:
            raise ValueError(f"existing dataset checksum mismatch: {source}")
        return source, expected
    if source.exists():
        raise ValueError(f"dataset without manifest already exists: {source}")
    rng = random.Random(seed)
    states = [[0, 0, None, None] for _ in range(groups)]
    digest = hashlib.sha256()
    payload = "x" * padding
    fd, temporary = tempfile.mkstemp(prefix=".generate-", dir=directory)
    try:
        with os.fdopen(fd, "wb") as output:
            header = b"id,key,amount,payload\n"
            output.write(header)
            digest.update(header)
            chunk = []
            for i in range(rows):
                group = i % groups
                cents = rng.randint(-10000, 100000)
                state = states[group]
                state[0] += 1
                state[1] += cents
                state[2] = cents if state[2] is None else min(state[2], cents)
                state[3] = cents if state[3] is None else max(state[3], cents)
                magnitude = abs(cents)
                number = f"{'-' if cents < 0 else ''}{magnitude // 100}.{magnitude % 100:02}"
                chunk.append(f"{i},g{group:06},{number},{payload}\n")
                if len(chunk) == 10000 or i + 1 == rows:
                    data = "".join(chunk).encode("ascii")
                    output.write(data)
                    digest.update(data)
                    chunk.clear()
                if (i + 1) % 1000000 == 0:
                    print(f"generated {i + 1:,}/{rows:,} rows (padding={padding})", flush=True)
        os.link(temporary, source)
        expected = {"rows": rows, "groups": groups, "seed": seed, "padding": padding,
                    "bytes": source.stat().st_size, "sha256": digest.hexdigest(),
                    "states": {f"g{i:06}": state for i, state in enumerate(states)}}
        manifest.write_text(json.dumps(expected, separators=(",", ":")) + "\n")
        return source, expected
    finally:
        Path(temporary).unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=1000000)
    parser.add_argument("--groups", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--padding", type=int, default=0)
    args = parser.parse_args()
    path, expected = generate(args.rows, args.groups, args.seed, args.padding)
    print(f"{path}: {expected['bytes']:,} bytes; sha256={expected['sha256']}")
