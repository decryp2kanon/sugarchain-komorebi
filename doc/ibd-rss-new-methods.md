# Additional peak-RSS experiments

These experiments compare new candidates with `57a01e07e08f054ba5b9cb3505194f95413be18c`, which already contains the earlier memory-management changes. Do not add these percentages to the earlier improvements. The old sampled full-IBD peak of 22.691 GiB has not been remeasured on this baseline.

The investigation covers the cache-related staircase (original events 1–4), resident indexes and header replay (5), and persistence/transition peaks (6–7). Those historical timestamps alone do not identify every live allocation. Component experiments cannot demonstrate that an entire historical event has been eliminated.

## Block-index mmap versus native pread

The experimental patch selected LevelDB's existing POSIX `pread` reader only for the Linux block-index read-reclamation environment. Its existing shared FD limiter, open-on-read fallback, checksum policy and last-reader clean-cache advice were retained. Other DB readers, the record schema, write batching/atomicity, all full-PoW requirements and both startup-mode policies were unchanged. The optional Env factory preserved existing wrappers' virtual read hooks. The complete comparison below did not justify adopting it for the whole-run peak-RSS goal. The experimental patch was retained privately; none of its production or test changes are in this commit.

Removing an SST mapping reduces file-backed process RSS while the kernel may still cache the same file pages. It is not an equal reduction of physical RAM, the OS page cache, cgroup memory or OOM risk. Native pread's copied blocks can also use the existing bounded block cache, while uncompressed mmap-backed blocks normally do not; anonymous memory can consequently grow.

### Isolated DB screening

One million synthetic paired header/evidence inputs (two million records), 32,768-input synchronous batches, baseline/candidate/candidate/baseline order, two processes per policy. The helper uses the same relevant block-tree cache, write-buffer, table-cache-zero and reclamation settings, but it is raw LevelDB rather than a full CDBWrapper/node. Keys and values are fixtures, not genuine PoW evidence. It verifies every record after reopening and makes 2,000 random point reads.

| Median metric | mmap | pread | Change |
|---|---:|---:|---:|
| Write-phase peak RSS | 118.350 MiB | 43.533 MiB | -63.22% |
| Reopen-inclusive peak RSS | 145.352 MiB | 43.533 MiB | -70.05% |
| Ingest and sync time | 3.330 s | 2.869 s | -13.85% |
| Reopen and complete scan | 1.364 s | 1.325 s | -2.85% |
| Last sampled process write_bytes | 1021.414 MiB | 1115.639 MiB | +9.22% |

Compaction timing changed the sampled I/O; these are process counters through the last sample, not device lifetime-write totals. The isolated helper's prototype used a local descriptor limit; the integrated candidate reused the existing production limiter. The large RSS decrease required an actual-node comparison before considering adoption.

### Full-block node comparison

One million deterministic, coinbase-only, real P2P regtest blocks per process, no header announcements, same `-fast-startup=0 -dbcache=512`, fixed mock time, and existing index-check debug scan disabled. Both sides include the final `gettxoutsetinfo("none")` sync and UTXO scan. This uses normal regtest SHA256d, not mainnet Yespower. Four serial runs, baseline/candidate/candidate/baseline, on a shared Linux host while the operating IBD continued. No build or other intentional heavy benchmark overlapped these runs.

| Median metric | Baseline | pread candidate | Change |
|---|---:|---:|---:|
| Peak RSS through acceptance, final sync/scan and restart | 475.010 MiB | 481.150 MiB | **+1.29%** |
| Peak RSS through acceptance only | 471.877 MiB | 464.867 MiB | -1.49% |
| RSS at end of acceptance | 447.449 MiB | 453.400 MiB | +1.33% |
| RSS after final sync/scan | 474.949 MiB | 481.150 MiB | +1.31% |
| Anonymous RSS after final sync/scan | 418.951 MiB | 424.646 MiB | +1.36% |
| Highest sampled file-backed RSS | 92.922 MiB | 55.232 MiB | -40.56% |
| Restarted-process peak RSS | 308.428 MiB | 309.471 MiB | +0.34% |
| Acceptance plus final sync/scan time | 148.479 s | 150.524 s | +2.046 s / +1.38% |
| Startup to RPC readiness | 2.266 s | 2.262 s | Effectively unchanged |
| Completed chainstate writes before shutdown | 31 | 31 | Unchanged |
| Process write_bytes in measured window | 1472.719 MiB | 1472.707 MiB | Effectively unchanged |

Individual original-process peaks: baseline 473.094/476.926 MiB, candidate 482.637/479.664 MiB. Total times: 149.705/147.252 versus 150.432/150.617 seconds. The small timing difference is not a prediction of mainnet IBD slowdown. In this fixture, smaller intermediate file-backed RSS did not lower the overall peak; anonymous memory was higher. The component-only result therefore cannot justify a claim of lower whole-node memory.

All four runs accepted the complete chain, preserved block/header height, active tip and chainwork after restart, matched UTXO count and tip across runs, and passed `verifychain(3,100)`. This benchmark does not compare UTXO MuHash. Sampled private-node swap stayed at zero. Process memory was sampled every 100 ms, with Linux VmHWM retained; separate maxima of file/anonymous components do not establish the simultaneous composition of an unsampled lifetime peak. The reported original-process peak covers final sync/scan, not a separately captured shutdown-only transient. Restart has a separate process HWM. Timings include the Python generator/P2P fixture; RPC latency samples taken after barriers do not bound stalls during a synchronous write.

### Header-replay node comparison

To cover the original header-replay stage independently, the existing private header fixture was run with one active block plus one million valid strict-regtest headers, `-dbcache=16`, again in baseline/candidate/candidate/baseline order. The generated chains differ between runs; the same generation algorithm, count and options are used. This is a separate workload from the full-block fixture, so neither its memory deltas nor its timings can be combined with that table.

| Median metric | Baseline | pread candidate | Change |
|---|---:|---:|---:|
| First daemon lifetime peak, including shutdown | 301.160 MiB | 272.229 MiB | -28.932 MiB / **-9.61%** |
| Higher peak including restarted process | 310.002 MiB | 272.502 MiB | -37.500 MiB / **-12.10%** |
| RSS after header acceptance | 270.504 MiB | 269.412 MiB | -0.40% |
| Restarted-process peak | 310.002 MiB | 271.598 MiB | -12.39% |
| Header acceptance plus shutdown | 36.351 s | 36.756 s | +0.405 s / +1.11% |
| Startup to RPC readiness | 1.886 s | 1.886 s | Effectively unchanged |
| Process write_bytes through acceptance | 672.133 MiB | 672.133 MiB | Effectively unchanged |

Restart-inclusive individual peaks were 309.754/310.250 MiB versus 273.688/271.316 MiB. The first daemon's complete lifetime high-water mark comes from a new `RUSAGE_CHILDREN` maximum immediately after its exit; no other child exited in that interval. Proc sampling continues through shutdown but can omit final I/O between its last sample and process exit. Component peaks are sampled separately and need not coincide. Restart times varied from 1.760 to 2.011 seconds in the baseline and 1.759 to 2.012 in the candidate, so the identical medians do not imply deterministic timing.

All four runs preserved the final header's hash, height, previous hash, target and chainwork after reopening, along with the active block hash/work, and passed verifychain. Sampled swap stayed at zero. The real benefit is lower transient header/reload RSS, not a demonstrated reduction of steady accepted RSS or full mainnet memory.


## Final decision and validation

**No new production optimization is adopted from this investigation.** Keep the earlier optimizations unchanged. The native-pread candidate is a useful, small, structure-preserving header-stage experiment with no identified correctness blocker. However, its header saving and the higher full-block-fixture maximum are different measurements; subtracting 6.141 MiB from 37.500 MiB would manufacture a nonexistent whole-run saving. The user's primary target was the maximum across the entire IBD, which has not been shown to improve. Hold the candidate rather than treating the DB-only 70% reduction as a node result.

The experimental daemon, GUI and unit target built. Existing DB-wrapper coverage was strengthened in the private patch and passed **12 cases / 129,631 assertions** in each normal and ASan/UBSan+leak run. A private SST corruption check required the actual checksum-mismatch error and restored the bytes before reuse. Existing LevelDB POSIX environment tests passed **7 cases** in each normal and ASan/UBSan run, covering both readers, descriptor-limit fallback, EOF, NotFound, close-on-exec and wrapper interception. These test changes were removed with the unadopted production patch; the underlying existing tests remain. The eight actual-node runs above all passed their specified restart checks. Cache prototypes passed their separate normal/sanitizer checks described below.

No additional functional suite was run after the non-adoption decision; it would not validate a new final production behavior. The final production/test source matches the baseline. Normal daemon, GUI and unit targets were rebuilt from that source; the daemon and unit executable SHA-256 hashes exactly match their preserved baseline binaries. The running operating node, its configuration and data were not restarted or changed. No full mainnet IBD, Windows/macOS build, TSan, slow-storage test or physical/cgroup memory comparison was performed. Two observations per node policy provide screening evidence, not confidence intervals or universal regressions.

This is an investigation record within the temporary combined PR #11. Before merge, Lazy PoW/persistent evidence and memory/CPU management must still be split into separate PRs. There is no new implementation saving to add to the retained changes and no new forecast of the old 22.691 GiB peak.


## Additional memory candidates evaluated but not adopted

These standalone measurements use the current pooled block-map layout and proof-cache algorithms. They are component results, not whole-node IBD measurements. Neither candidate changes production behavior in this patch.

### Higher block-map load factors

The actual `FastBlockMap` and pool allocator were tested with `max_load_factor` values 1, 1.25, 1.5 and 2. Natural growth and startup's existing `reserve(count + count / 2)` were measured separately. The reservation policy was unchanged. Eight 10,000-entry correctness cases preceded sixteen two-million-entry runs, with each combination measured twice in forward/reverse order.

| Factor 1.25 versus current factor 1 | Natural growth | Startup reservation |
|---|---:|---:|
| Helper peak RSS | 391.984 → 385.939 MiB | 393.750 → 388.625 MiB |
| Peak reduction | 6.045 MiB / 1.54% | 5.125 MiB / 1.30% |
| Insertion time | +5.09% | +8.49% |
| Existing-key lookup time | +7.20% | +10.30% |
| Absent-key lookup time | +11.91% | +12.24% |
| Iteration time | +9.13% | +28.78% |

Larger factors increased the insertion/miss/traversal penalties. Exact key/index/parent pointers, duplicate behavior, traversal and deletion/reinsertion checks passed. All variants retained the same node-pool allocation.

Natural-growth savings also depend on the current rehash phase: at 1,320,404–1,447,153 entries, factor 1.25 held 697,824 **more** buckets than the baseline, approximately 5.324 MiB of extra bucket-array storage. This is a bucket-count calculation, not a sampled RSS difference. The small endpoint savings, continuing operation costs and count-dependent regression did not justify changing the production policy.

### Packing immutable proof-cache tiers

Three private prototypes packed only retired tiers while preserving every logical slot, full 256-bit value, hash mapping, tier budget, zero behavior and terminal-tier eviction. Normal and ASan/UBSan checks covered slot equivalence, allocation failures, partial packing, retry, growth failure and locked reader lifetime.

The final prototype used one typed allocation per packed page and byte-prefix ranks. In a bounded twelve-million-entry comparison at a 2,048 MiB configured cache budget:

- RSS and lifetime high-water mark fell **1,319.00 → 1,101.91 MiB**, saving **217.09 MiB / 16.46%**.
- Insertion rose **11.4186 → 14.8655 seconds**, adding **3.4469 seconds / 30.19%**.
- Sampled prior-key lookups were **14.67% slower**; 100,000 absent probes were **40.43% slower**.
- The longest measured freeze took **0.166 seconds**, within a **0.176-second** tier-installation-plus-freeze interval. Production integration would perform this operation under the exclusive cache lock.

The final variant has one observation per side. Earlier instrumented timings were affected by a 2 ms heap-stat sampler, particularly on fragmented candidate heaps; adoption was assessed using separate controls without that sampler or per-growth allocation-stat traversal. Reduced requested allocation was about 234 MiB; allocator retention explains part of the smaller observed RSS reduction.

The real memory saving did not outweigh the sustained cache-path cost, synchronous growth pause and additional representation/failure-state machinery for this change. No cache prototype was integrated. These synthetic-digest results do not establish whole-node throughput or an additional percentage reduction of the historical IBD peak.
