#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""A/B/B/A offline block import, including orderly shutdown and disk flush.

This measures a supplied historical block file, NOT full network IBD. Every run
gets a new datadir, disables networking/wallets/assumevalid, and retains evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--blocks", type=Path, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--tip", required=True)
    parser.add_argument("--work-dir", type=Path, required=True, help="must not exist")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.height < 1 or args.timeout < 1 or not re.fullmatch(r"[0-9a-f]{64}", args.tip):
        parser.error("require positive height/timeout and a lowercase 64-digit tip hash")
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ("baseline", "candidate")}
    blocks = args.blocks.resolve(strict=True)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    report = {
        "network": False, "assumevalid": "0", "dbcache_mib": 1024,
        "blocks_sha256": file_hash(blocks),
        "binary_sha256": {name: file_hash(path) for name, path in binaries.items()},
        "runs": [],
    }
    for number, name in enumerate(("baseline", "candidate", "candidate", "baseline")):
        datadir = work / f"{number}-{name}"
        datadir.mkdir()
        command = [str(binaries[name]), f"-datadir={datadir}", "-server=0", "-daemon=0",
                   "-disablewallet=1", "-networkactive=0", "-connect=0", "-dnsseed=0",
                   "-listen=0", "-discover=0", "-listenonion=0", "-assumevalid=0",
                   "-dbcache=1024", f"-stopatheight={args.height}", f"-loadblock={blocks}",
                   "-printtoconsole=0"]
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        begin = time.monotonic()
        with (datadir / "console.log").open("w") as output:
            child = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            active = work / "active-process.json"
            active.write_text(json.dumps({"pid": child.pid, "command": command}) + "\n")
            try:
                status = child.wait(timeout=args.timeout)
            finally:
                if child.poll() is None:
                    child.terminate()  # Only our own child; never discover/kill other nodes.
                    child.wait(timeout=60)
                active.unlink()
        elapsed = time.monotonic() - begin
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        log = (datadir / "debug.log").read_text()
        passed = (status == 0 and f"best={args.tip} height={args.height} " in log
                  and "Shutdown done" in log
                  and not re.search(r"bad-diffbits|high-hash|ERROR:", log))
        row = {"variant": name, "seconds": elapsed, "passed": passed,
               "cpu_seconds": after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
               "exit_status": status, "datadir": str(datadir), "command": command}
        report["runs"].append(row)
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(row), flush=True)
        if not passed:
            raise RuntimeError(f"Import did not validate and stop at the expected tip; inspect {datadir}")


if __name__ == "__main__":
    main()
