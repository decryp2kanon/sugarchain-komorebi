# Presync dispatch investigation: resumable checkpoint

Baseline: `9a1d3e695e9d1916edcf544d115bd2c27e98bb94`, 2026-10-15.

## Objective and status

Investigate why a hash-only timing improvement did not survive the real verifier
path, then test whether dispatch overhead can be reduced without changing the
first-header admission check or the eight-cold-proof failure-decision boundary.
This checkpoint records the experiment before any new performance result.
No candidate runtime change has been adopted. The operating node and its data
remain untouched.

The goal remains a complete initial PRESYNC within three hours, with whole-node
CPU limited to eight logical CPUs' capacity and RSS below 10,000,000,000 bytes.
Short fixture timings cannot establish that goal. See the
[previous results](presync-three-hour-results.md) for the rejected compute/cache
candidates and the limits of the historical seven-hour comparison.

## Measurement plan

1. Use the same actual thread-CPU clock and elapsed-time definitions for continuous
   hashing, hashing with an eight-item completion barrier, the real verifier with
   2,000-header messages, and the eight-peer PRESYNC driver.
2. Report actual proof calls separately from received peer-header counts. Validate
   unique fixture identities; eight peers do not mean eight times the unique work.
3. Separate time between completed epochs from waiting for an epoch's last job.
   Summed per-thread or per-peer waits overlap and are not elapsed time saved.
4. Time baseline and candidate sequentially, with at least two ABBA groups. Builds
   and other owned tests must finish before performance measurements. Repeat
   useful candidates on larger inputs and P2P fixtures with instrumentation off.

## Candidate and invariants

Only if dispatch costs warrant a prototype, evaluate message-lifetime dispatch
using the existing budget of seven background workers plus the participating
caller. Workers and per-thread scratch are already persistent; this is not a
thread-creation optimization. Keep a maximum of eight cold slots per decision;
do not start the next epoch before every current slot has completed successfully.
Avoid busy spinning and avoid a second pool for startup/Lazy `CheckEach` calls.

Preserve shared-pool serialization, current target/cache/batch-evidence policy,
resource-exception propagation, shutdown draining, and reuse after failure.
Existing invalid-proof computation limits must pass without relaxed assertions.
The previously rejected 512-item supply queue is not a candidate here.

Private artifacts live under `$EVIDENCE_ROOT/presync-root-cause/`. The baseline
manifest records source and binary hashes and the operating process start time.
Measure under the same verified eight-CPU affinity and memory cgroup for every
arm; do not claim an unavailable CPU quota. These experiment controls do not by
themselves prove a complete node meets the requested resource limits.

## Reporting and review scope

Record rejected as well as accepted experiments, timing ranges, actual proof
counts, CPU time, RSS, correctness coverage and remaining limitations. Only
repeatable integrated benefit justifies a runtime change. Preserve small useful
gains without claiming the three-hour goal unless the complete interval is timed.

This work belongs to the presync group of the future **Lazy PoW / RSS management /
presync performance** three-PR split. No PR split or merge is part of this task.

## Measurement implementation checkpoint

The benchmark now appends actual `CLOCK_THREAD_CPUTIME_ID` proof CPU time and
independently checked unique fixture identifiers to its existing CSV columns.
`--stage-timing` reports disjoint verifier/context/other caller intervals;
`--no-pow-timing` disables per-proof clocks for overhead controls while retaining
call counts. Unsupported clocks explicitly report unavailable. Summed thread
elapsed time is still a separate diagnostic, not wall time saved.

The benchmark built successfully. A 6,000-header smoke run covered raw continuous
hashing, an eight-item raw barrier, real one-peer verification, and the eight-peer
driver. Every cold arm performed 6,000 proofs; verifier repeats performed zero.
The private dispatch candidate also passed the existing 31 `header_pow_tests`
cases and all 54,986 assertions, including invalid-proof bounds and exception
recovery. These are preparation checks, not evidence of a speed improvement or
full PRESYNC completion. No runtime candidate is adopted at this checkpoint.
