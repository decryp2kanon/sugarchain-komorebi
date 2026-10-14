# Three-hour presync experiments: resumable checkpoint

Date: 2026-10-15. Production baseline: `49b9f7ed78cd37e5eae1f28e4ed24e8cc89d76a4`.

## Status and objective

**The three-hour target has not been achieved. No runtime candidate from this checkpoint is installed or adopted.** The completed experiments below use private executables; node runtime source, operating binaries, operating node and its data remain unchanged.

The objective is complete initial PRESYNC within 10,800 seconds while keeping node-wide CPU at or below eight logical CPUs' capacity and RSS below **10,000,000,000 bytes (9.313 GiB)**. Initial PoW, contextual validation and existing invalid-input work bounds must remain intact. REDOWNLOAD and block synchronization are separate phases, but their memory and recomputation costs must not be hidden by a presync-only improvement.

The historical observation was 26,012 seconds, requiring a 58.48% reduction for the same work boundary. It is context, not a same-binary baseline for the new microbenchmarks. See [the preceding worker-supply experiment](presync-worker-supply-results.md). Its larger-queue candidate remains rejected because it exceeded the existing invalid-PoW computation bound.

## Resource controls and excluded runs

All completed measurements ran sequentially while the operating node continued running. No build or separate benchmark overlapped the accepted compute comparison. Host contention and scheduling still affect wall time.

- The requested transient-unit CPU quota was **not available**: the delegated cgroup had no `cpu.max`. The initial `cache-screen-0-baseline` attempt failed the control check before launching the workload. It contributes no result; requesting `CPUQuota=800%` must not be described as an enforced quota.
- The replacement runner confines itself and its child to eight logical CPUs. The accepted compute series uses mask `0-3,8-11`, four distinct physical cores per L3 domain. This is an identical experimental resource condition, not a proposed production affinity setting.
- The initial compute screen used `0-7`, concentrating workers in one L3 domain. Its completed baseline/fixed timings, **32.365/31.500 seconds**, and the interrupted next run are excluded from performance conclusions. The correctness checks under that mask remain valid; their timings are not performance evidence.
- Each completed unit verified `memory.max=9,999,998,976` bytes, the page-rounded limit. All reported `max`, `oom` and `oom_kill` events were zero. This cgroup limit also counts file cache and is not identical to process RSS.
- `/proc` CPU and RSS/HWM were sampled about every 250 ms. CPU recomputed over intervals of at least one second peaked at **800.49–801.09%** in the accepted compute runs. Small overshoots reflect CPU tick/read-time sampling resolution; do not round these into a claim that every observed sample was below 800%. The inherited eight-CPU affinity is the execution ceiling.

These controls apply to private experiment processes, not the running production node. They do not establish whole-node resource compliance for a full presync.

## A. Proof-cache retention screen — synthetic inputs

Three private policies retain complete 256-bit entries: the existing paged growing tiers, a final remainder tier, and fixed hash partitions of at most 64 MiB. No shortened fingerprint or probabilistic positive result authorizes verification. Private variants replace only the growing backend; the original verification and batch-evidence paths remain available.

One run per policy inserted **44,900,000 synthetic SHA256(index) keys** with a nominal 2,048 MiB entry budget, reserving 2 MiB for the production-style batch allowance. It then made 462,890 read-only chronological probes, without reinserting misses. Each run also rejected all 100,000 disjoint negative probes. The preparation self-test passed 30,000 inserts and 30,000 positive/negative probes respectively across the three policies.

| Policy | Logical individual-entry allocation, MiB | Insert interval, s | Delayed lookup interval, s | Sampled miss rate | Sampled/process peak RSS, MiB |
|---|---:|---:|---:|---:|---:|
| Existing growth | 1,294.734 | 51.365 | 0.270 | 24.0485% | 1,318.75 |
| Final remainder tier | 2,046.000 | 55.319 | 0.249 | 6.6787% | 2,080.00 |
| Fixed segments | 2,046.000 | 30.160 | 0.208 | 0/462,890 observed | 2,080.50 |

These are **not PoW or presync speed measurements**. The timed insert interval includes SHA256 key generation and excludes store construction. The synthetic helper does not allocate the reserved batch cache. Flags, page directories and allocator overhead are additional to the entry budget. The runner's child high-water values were 0.25 MiB above the table values; neither value is a whole-node measurement. Average child CPU was approximately 99.5–99.6%; sampled cgroup memory remained below 2.188 billion bytes.

Segmentation merits real-proof follow-up, but its larger and earlier physical allocation is a cost. The zero observed miss count is not proof of lossless retention for all workloads. The final remainder tier can also change which age groups lose entries. No 4/5 GiB experiment or production cache-limit increase has been performed. Such an experiment must count metadata in its total budget and preserve the integer limits of every backend.

## B. Fixed-parameter Yespower — genuine hash computation

The private specialization fixes Yespower 1.0, the existing personalization, 80 input bytes, `N=2048`, `r=32` and the derived 684 second-loop iterations. S-box initialization keeps its original `r=1`, `N=768` path, and the initial `r=1` expansion remains generic. Mixing order, rounds, prefetch policy and compiler optimization flags are unchanged.

The original optimized implementation is compiled into a separate baseline object with renamed exports; it does not share modified mixing helpers. Both objects use the same unchanged SHA256 implementation. This is an independent baseline build, not an independently authored Yespower implementation. A generic candidate entry remains available as a layout control, but no completed layout-control timing series is included here.

Completed correctness checks:

- All 32 output bytes matched for **8,691 inputs**: 6,000 real headers, 640 single-bit mutations, 2,048 deterministic random inputs, two edge patterns and mainnet genesis.
- The genesis result additionally matched the existing known-answer value.
- Injected fresh-scratch mmap failures matched return code `-1`, `ENOMEM`, all-`0xff` output and empty-region cleanup in both implementations; exactly two allocations were denied. This does not cover every allocator or retry/free failure case.

Two sequential ABBA groups used 40,000 genuine headers, eight independent persistent worker scratch regions, and no proof cache. Every run completed exactly 40,000 hashes and produced the same aggregate result digest. Input loading and output-digest calculation were outside the timer; first-use scratch allocation was inside it.

| Order | Implementation | Wall seconds | Summed worker CPU seconds |
|---:|---|---:|---:|
| 1 | Baseline | 18.455222 | 137.322182 |
| 2 | Fixed | 17.923036 | 134.806863 |
| 3 | Fixed | 17.090400 | 133.371767 |
| 4 | Baseline | 19.318081 | 136.929562 |
| 5 | Baseline | 18.261740 | 136.932779 |
| 6 | Fixed | 18.296099 | 134.040543 |
| 7 | Fixed | 17.195286 | 134.292100 |
| 8 | Baseline | 19.524204 | 137.422778 |

Wall medians were **18.886652 → 17.559161 seconds, 7.03% lower**. Summed worker CPU medians were **137.127481 → 134.166321 seconds, 2.16% lower**. Wall ranges overlap, and the different improvements show that scheduling/utilization contribute to the elapsed result. Do not describe the 7.03% as a demonstrated reduction in intrinsic hash CPU cost or whole-presync time.

Per-run average worker CPU ranged from 703.86–749.83% for baseline and 732.62–780.98% for fixed. Process peaks were 71.97–72.50 MiB and 71.70–72.30 MiB respectively; sampled cgroup memory peaked below 83.2 million bytes. These tiny memory differences do not establish an RSS optimization. No sanitizer, alternate-platform or integrated-node validation of the specialization is recorded at this checkpoint.

## Resume artifacts and commands

`$EVIDENCE_ROOT` denotes the private experiment directory; `$HEADERS_RAW` denotes the archived contiguous one-million-header fixture. The private sources and binaries are intentionally not part of this documentation-only commit. These commands require those retained artifacts, not merely a fresh repository checkout. Review their source/build hashes before reuse, and record the exact fixture hash for each expanded comparison.

Retained evidence includes:

- `baseline.json`, `cache/baseline-source-sha256.json`, `cache/prepared-artifacts.json`, `compute/build-metadata.json`, `compute/driver-metadata.json`: source/build identity and commands.
- `cache-screen-r1-{0-baseline,1-remainder,2-segmented}/`: stdout, stderr, measurements and raw samples. This single-thread screen used the earlier eight-CPU mask; it is not pooled with compute timing.
- `compute-check-results.json`, `compute-balanced-results.json` and their per-run directories: completed checks and accepted ABBA results.
- `measurement-setup-note.txt`, `cache-screen-0-baseline.service.log`, `compute-screen-results.json`: excluded setup/timing records.
- `delayed-header-pow.cpp`, `delayed-baseline`, `delayed-segmented`: prepared real-proof follow-up, measured in the follow-up below. It changes repeat message boundaries from 2,000 to 1,999 headers to avoid reusing the original batch keys.

Run only one measurement at a time and use fresh output tags. The runner refuses to overwrite an existing result directory. Its affinity is topology-specific and must be reviewed before using another host.

```sh
# EVIDENCE_ROOT and HEADERS_RAW must identify the retained artifacts.
run_private() {
    tag=$1
    shift
    systemd-run --user --wait --pipe --collect --unit="presync-${tag}" \
        -p MemoryMax=10000000000 \
        python3 "$EVIDENCE_ROOT/run_limited.py" \
        --out "$EVIDENCE_ROOT/$tag" --timeout 600 -- "$@"
}

run_private retry-check "$EVIDENCE_ROOT/compute/driver-fixed" \
    check fixed "$HEADERS_RAW" 6000 8
run_private retry-failure "$EVIDENCE_ROOT/compute/failure"

# Use baseline/fixed/fixed/baseline twice, with a distinct tag per run.
run_private retry-compute-a1 "$EVIDENCE_ROOT/compute/driver-fixed" \
    bench baseline "$HEADERS_RAW" 40000 8
run_private retry-compute-b1 "$EVIDENCE_ROOT/compute/driver-fixed" \
    bench fixed "$HEADERS_RAW" 40000 8

# Repeat the cache screen independently before treating its timing as stable.
run_private retry-cache-a "$EVIDENCE_ROOT/cache/retention" \
    baseline 2048 44900000 97
run_private retry-cache-b "$EVIDENCE_ROOT/cache/retention" \
    segmented 2048 44900000 97
```

## Pending decisions and validation

1. Measure real cold and delayed verification with constrained cache capacity, actual Yespower call counts and reinsertion churn. Keep changed message boundaries or use individual `CheckEach`/`CheckBlockProofOfWork` passes: a 48 MiB scaled cache still has the same 2 MiB batch cache as the full-size setting, so unchanged-message repeats can hide individual-proof eviction. Existing `header_pow_tests` already provides a smaller batch-independent cold/delayed fixture. The opt-in `bench_header_pow --delayed-reuse` mode reproduces this experiment; its 2,000/1,999 message sizes are incompatible with `--presync` and `--block-stage`.
2. Repeat useful cache comparisons, record early RSS and segment balance, and measure PRESYNC→REDOWNLOAD cost. Do not expand to 4/5 GiB unless real recomputation justifies it; total-memory accounting and the legacy backend's integer bounds need a separate check.
3. For the fixed hash candidate, confirm the benefit in a private node/verification path and against the generic layout control. Preserve a portable fallback and replace experimental source duplication with maintainable integration only if benefit survives.
4. Before adopting either candidate, run relevant mutation/target/context, invalid-first/middle/last proof, reset/eviction/concurrency, failure, P2P and sanitizer checks. Preserve the existing maximum speculative work on rejected input.
5. Only a complete matching-work PRESYNC within 10,800 seconds with measured node-wide resource compliance can satisfy the target. Do not multiply or add improvements from these unrelated screens. Commit and push accepted logical steps and checkpoint evidence separately.

PR #11 remains staging work intended for separation into **Lazy PoW**, **RSS/memory management**, and **presync performance/diagnostics** PRs. This checkpoint belongs to the third scope and performs no split or merge.


## C. Real delayed reuse follow-up and reproducible benchmark

An ABBA screen used 100,000 contiguous real headers, eight workers and a scaled
4 MiB cache. The cold pass uses 2,000-header chunks and the delayed repeat uses
1,999-header chunks. Each process loads and checks fixture continuity, difficulty
and median time before the timer. Timed passes include genuine PoW when proof
cache evidence is unavailable; every cold pass performed exactly 100,000 hashes.

| Order | Private policy | Cold seconds | Delayed seconds | Delayed real hashes | Process peak RSS, MiB |
|---:|---|---:|---:|---:|---:|
| 1 | Existing growth | 52.4198 | 45.7378 | 85,204 | 94.418 |
| 2 | Fixed segments | 52.3810 | 0.112937 | 0 | 95.266 |
| 3 | Fixed segments | 51.7938 | 0.112560 | 0 | 94.840 |
| 4 | Existing growth | 52.5772 | 43.2414 | 85,179 | 94.559 |

This establishes a real-proof benefit for **changed boundaries after delayed
reuse**. It does not establish the same miss rate in a running initial PRESYNC.
Misses are reinserted during real verification, so this table is not comparable
to the read-only synthetic probe miss rate. The two cold timings per variant
are insufficient to assert a small cold-pass speedup. The larger cache policy
remains unadopted pending evidence that its earlier allocation is worthwhile in
the intended workload.

Unchanged 2,000-header messages can restore individual evidence from the existing
batch cache: 37.5 million headers need 18,750 such batch entries and 44.9 million
need 22,450, compared with 65,536 slots in the 2 MiB batch cache. These counts do
not guarantee retention under other branches, boundaries or competing traffic,
but explain why the deliberately misaligned repeat is not a normal-network speed
measurement. Neither cache policy removes the first genuine hash of each unique
header or demonstrates the three-hour target.

The repository benchmark now exposes the same pattern explicitly:

```sh
build/bin/bench_header_pow "$HEADERS_RAW" 100000 8 4 --grow-cache --delayed-reuse
# Ordinary identical-message control:
build/bin/bench_header_pow "$HEADERS_RAW" 100000 8 4 --grow-cache
```

No node runtime path changes in this benchmark commit. The benchmark built and
completed a 6,000-real-header smoke run, with 6,000 cold hashes and zero repeated
hashes at 4 MiB. Both incompatible mode combinations were rejected. Private raw
results are retained in `cache-balanced-delayed-results.json` and its four run
directories. Repeated trial evidence and broader retention checks would still be
needed before adopting a production cache replacement.
