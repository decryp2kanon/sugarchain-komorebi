#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain Komorebi developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Linux-only, genuine Yespower subprocess recovery test; no live node or peers.

The child stops at an observed calculation/WAL-sync/batch boundary, then the
parent sends SIGKILL. Process death is not a simulation of physical power loss.
"""

import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import tempfile
import threading
import time


def run_child(binary, root, mode, point="", workers=8, expect_error=False):
    counter = root / ".invocation-count"
    invocation = int(counter.read_text()) + 1 if counter.exists() else 1
    counter.write_text(str(invocation))
    prefix = f"{invocation:03d}-{mode}"
    control = root / f"{prefix}.control.json"
    control.write_text(json.dumps(dict(root=str(root), mode=mode, point=point,
                                      workers=workers, expect_error=expect_error)))
    environment = dict(os.environ, SUGAR_LAZY_CRASH_CONTROL=str(control))
    command = [str(binary), "--run_test=historical_pow_crash_tests/child",
               "--log_level=nothing", "--report_level=no", "--color_output=no"]
    events = []
    lines = queue.Queue()
    with (root / f"{prefix}.stderr").open("w") as errors:
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                   stderr=errors, text=True, start_new_session=True)

        def reader():
            try:
                for line in process.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        deadline = time.monotonic() + 120
        killed = False
        try:
            with (root / f"{prefix}.events.jsonl").open("w") as transcript:
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f"child timeout: {mode} {point}")
                    line = lines.get(timeout=remaining)
                    if line is None:
                        break
                    if not line.startswith("CRASH_EVENT "):
                        continue
                    event = json.loads(line.removeprefix("CRASH_EVENT "))
                    events.append(event)
                    transcript.write(json.dumps(event) + "\n")
                    transcript.flush()
                    if event["event"] == "kill_ready":
                        assert mode == "crash" and event["point"] == point
                        stop_deadline = time.monotonic() + 5
                        while "\nState:\tT" not in Path(f"/proc/{process.pid}/status").read_text():
                            if time.monotonic() >= stop_deadline:
                                raise TimeoutError("child did not enter SIGSTOP")
                            time.sleep(0.005)
                        process.kill()  # Only this child PID, never a name/pattern.
                        killed = True
            returncode = process.wait(timeout=10)
            if mode == "crash":
                assert killed and returncode == -signal.SIGKILL, (point, returncode)
                assert not any(e["event"] == "scan_result" for e in events)
            else:
                assert returncode == 0, (mode, returncode, errors.name)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            process.stdout.close()
            thread.join(timeout=5)
    return events


def only(events, kind):
    values = [e for e in events if e["event"] == kind]
    assert len(values) == 1, (kind, len(values))
    return values[0]


def check_resume(binary, root, workers, crash):
    # The first DB opener after SIGKILL must also run the scan. An earlier
    # inspect-only opener would consume WAL recovery before the actual restart.
    resume = run_child(binary, root, "resume", workers=workers)
    inventory = only(resume, "inventory")
    expected, present = set(inventory["expected"]), set(inventory["present"])
    computed, committed = set(), set()
    for event in crash:
        if event["event"] == "proof":
            assert event["result"] == 0
            computed.add(event["hash"])
        elif event["event"] in ("write_committed", "wal_sync_after"):
            committed = computed.copy()
    assert committed <= present <= computed <= expected
    # A crash during the next WAL write may preserve the whole batch or none
    # of it, but must not recover a partial batch of evidence.
    assert len(present) in (0, 512, 1024, len(expected)), len(present)
    partial = [e for e in crash if e["event"] == "partial_wal_write"]
    if partial:
        assert len(partial) == 1
        assert 0 < partial[0]["written"] < partial[0]["requested"]
        assert len(present) == 512, "incomplete second batch must not be admitted"
    result = only(resume, "scan_result")
    proof_events = [e for e in resume if e["event"] == "proof"]
    assert all(e["result"] == 0 for e in proof_events)
    actual = [e["hash"] for e in proof_events]
    assert set(actual) == expected - present
    assert len(actual) == len(set(actual)), "duplicate actual proof work"
    assert result["complete"] and not result["error"]
    assert result["processed"] == len(expected)
    assert result["verified"] == len(expected - present)
    assert result["reused"] == len(present)
    after = only(run_child(binary, root, "inspect", workers=workers), "inventory")
    assert set(after["present"]) == expected
    return dict(workers=workers, reused=len(present), verified=len(actual), complete=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    assert 1 <= args.repetitions <= 10
    binary = args.test_binary.resolve(strict=True)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        root, temporary = args.output.resolve(), None
    else:
        temporary = tempfile.TemporaryDirectory(prefix="lazy-pow-crash-")
        root = Path(temporary.name)
    try:
        pristine = root / "pristine"
        pristine.mkdir()
        (pristine / ".lazy-pow-crash-test").touch()
        expected = only(run_child(binary, pristine, "seed"), "inventory")
        assert len(expected["expected"]) == 6000 and not expected["present"]
        points = ["proof_return:1", "proof_return:513", "write_begin:2", "wal_partial:2",
                  "wal_sync_before:2", "wal_sync_after:2", "write_committed:2",
                  "write_committed:12"]
        results = []
        for workers in (1, 8):
            for repeat in range(args.repetitions):
                for point in points:
                    case = root / f"w{workers}-r{repeat}-{point.replace(':', '-')}"
                    shutil.copytree(pristine, case)
                    crash = run_child(binary, case, "crash", point, workers)
                    result = check_resume(binary, case, workers, crash)
                    result.update(point=point, repetition=repeat)
                    results.append(result)
                    print(json.dumps(result), flush=True)
                    (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        # Successive crashes reuse the same DB, rather than resetting it.
        case = root / "repeated-crashes"
        shutil.copytree(pristine, case)
        durable = set()
        for point in ["write_committed:1"] * 3 + ["write_begin:2"]:
            crash = run_child(binary, case, "crash", point, 8)
            assert durable <= set(only(crash, "inventory")["present"])
            computed = set()
            for event in crash:
                if event["event"] == "proof":
                    computed.add(event["hash"])
                elif event["event"] == "write_committed":
                    durable.update(computed)
            if point != "write_begin:2":
                recovered = only(run_child(binary, case, "inspect"), "inventory")
                assert durable <= set(recovered["present"])
        # Earlier crashes may have committed additional prefixes. Inspect the
        # final restart independently instead of applying the two-batch oracle.
        resume = run_child(binary, case, "resume")
        prior = only(resume, "inventory")
        assert durable <= set(prior["present"])
        result = only(resume, "scan_result")
        missing = set(prior["expected"]) - set(prior["present"])
        actual = [e["hash"] for e in resume if e["event"] == "proof"]
        assert set(actual) == missing and len(actual) == len(missing)
        assert result["complete"] and not result["error"]
        assert result["reused"] == len(prior["present"])
        assert result["processed"] == 6000
        # Exercise malformed/truncated/version/rules evidence after recovery.
        damaged = root / "damaged-evidence"
        shutil.copytree(case, damaged)
        inventory = only(run_child(binary, damaged, "damage"), "inventory")
        assert len(inventory["present"]) == 5996
        resumed = run_child(binary, damaged, "resume")
        result = only(resumed, "scan_result")
        proofs = [e["hash"] for e in resumed if e["event"] == "proof"]
        assert set(proofs) == set(inventory["expected"]) - set(inventory["present"])
        assert len(proofs) == result["verified"] == 4
        assert result["reused"] == 5996 and result["complete"]
        for mode, message in (("bad_pow", "Lazy PoW failed at height"),
                              ("bad_identity", "block-index identity mismatch")):
            invalid = root / mode
            shutil.copytree(case, invalid)
            bad = only(run_child(binary, invalid, mode), "invalid_header")
            failed = only(run_child(binary, invalid, "resume", expect_error=True), "scan_result")
            assert not failed["complete"]
            assert message in failed["error"] and bad["hash"] in failed["error"]
        print(f"PASS: {len(results)} boundary recoveries, repeated-crash recovery, "
              "damaged evidence repair and invalid-header rejection", flush=True)
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
