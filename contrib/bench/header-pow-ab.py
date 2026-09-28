#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Compare production header PoW executables in fresh processes, in A/B/B/A order.

Requires bench_header_pow binaries. Neither networking nor node processes are
used. Cold first-proof time and repeated-cache time are reported separately;
neither is an estimate of full IBD duration.
"""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import resource
import statistics
import subprocess
import time


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def parse_measurements(output, count, workers):
    rows = list(csv.DictReader(io.StringIO(output)))
    if len(rows) != 2 or [row["pass"] for row in rows] != ["cold", "repeat"]:
        raise ValueError("Expected one cold and one repeat result")
    result = {}
    for row in rows:
        seconds = float(row["seconds"])
        rate = float(row["headers_per_second"])
        if (int(row["workers"]) != workers or int(row["headers"]) != count
                or not math.isfinite(seconds) or seconds <= 0
                or not math.isfinite(rate) or rate <= 0
                or not math.isclose(rate * seconds, count, rel_tol=0.0001)):
            raise ValueError("Invalid or inconsistent benchmark measurement")
        result[row["pass"]] = {"seconds": seconds, "headers_per_second": rate}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--count", type=int, default=6000)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--baseline-cache-mib", type=int)
    parser.add_argument("--candidate-cache-mib", type=int)
    parser.add_argument("--work-dir", type=Path, required=True, help="must not exist")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if not 1 <= args.count <= 1_000_000 or not 1 <= args.workers <= 8 or args.timeout < 1:
        parser.error("count must be 1..1000000, workers 1..8, timeout positive")
    cache_sizes = {name: getattr(args, f"{name}_cache_mib") for name in ("baseline", "candidate")}
    if any(size is not None and not 1 <= size <= 2048 for size in cache_sizes.values()):
        parser.error("cache budgets must be 1..2048 MiB")
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ("baseline", "candidate")}
    headers = args.headers.resolve(strict=True)
    if headers.stat().st_size < args.count * 80:
        parser.error("header file is too short")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    fingerprints = {str(path): digest(path) for path in (*binaries.values(), headers)}
    report = {"network": False, "count": args.count, "workers": args.workers, "cache_mib": cache_sizes,
              "sha256": fingerprints, "runs": []}
    for number, name in enumerate(("baseline", "candidate", "candidate", "baseline")):
        # Never silently compare different input/build versions within one run.
        if any(digest(Path(path)) != value for path, value in fingerprints.items()):
            raise RuntimeError("Input or executable changed during comparison")
        command = [str(binaries[name]), str(headers), str(args.count), str(args.workers)]
        if cache_sizes[name] is not None:
            command.append(str(cache_sizes[name]))
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.monotonic()
        process = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        elapsed = time.monotonic() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        (work / f"{number}-{name}.csv").write_text(process.stdout)
        (work / f"{number}-{name}.stderr").write_text(process.stderr)
        row = {"variant": name, "command": command, "exit_status": process.returncode,
               "wall_seconds": elapsed,
               "cpu_seconds": after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime}
        report["runs"].append(row)
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        process.check_returncode()
        if any(digest(Path(path)) != value for path, value in fingerprints.items()):
            raise RuntimeError("Input or executable changed during comparison")
        row["measurements"] = parse_measurements(process.stdout, args.count, args.workers)
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(row), flush=True)
    report["median_seconds"] = {
        name: {phase: statistics.median(row["measurements"][phase]["seconds"]
                                        for row in report["runs"] if row["variant"] == name)
               for phase in ("cold", "repeat")}
        for name in binaries
    }
    report["cold_speedup"] = (report["median_seconds"]["baseline"]["cold"]
                              / report["median_seconds"]["candidate"]["cold"])
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("median_seconds", "cold_speedup")}), flush=True)


if __name__ == "__main__":
    main()
