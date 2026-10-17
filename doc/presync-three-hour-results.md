# Three-hour presync experiments: results

Date: 2026-10-15. Runtime baseline: `49b9f7ed78cd37e5eae1f28e4ed24e8cc89d76a4`.

## Conclusion and scope

**The three-hour target has not been achieved. All runtime candidates are rejected
for this task.** The promising hash-only improvement did not survive the integrated
presync comparison. The operating node, its binary and data have not been
replaced or restarted for these experiments.

The target is complete initial PRESYNC in 10,800 seconds, with node-wide CPU no
greater than eight logical CPUs' capacity and RSS below 10,000,000,000 bytes
(9.313 GiB). All initial PoW and contextual checks, and invalid-input computation
bounds, must remain. Historical PRESYNC took 26,012 seconds; it is context, not a
matching-run baseline for these short experiments. REDOWNLOAD and block sync are
separate work and must not be counted as completed by a presync microbenchmark.

## Completed genuine-hash comparisons

Each row uses its own two ABBA groups: four original and four candidate runs,
40,000 real headers per run, eight persistent worker scratch regions, no proof
cache. Original and candidate are separately compiled objects. All timed runs
completed exactly 40,000 hashes and matched the full aggregate output digest.
Percentages compare within-series medians; positive values are regressions.

| Private variant | Baseline → candidate wall seconds | Wall change | Worker CPU-time change | Decision |
|---|---:|---:|---:|---|
| Initial copied fixed helpers | 18.886652 → 17.559161 | −7.03% | −2.16% | Superseded; source duplication not adopted |
| Shared bodies, original prefetch | 18.897068 → 19.361716 | +2.46% | +0.48% | Not adopted |
| Exact outer API dispatch | 18.264803 → 18.639525 | +2.05% | −2.12% | Not adopted |
| Rolling prefetch alone | 18.427169 → 18.071483 | −1.93% | +0.27% | Not adopted |
| Shared bodies plus rolling prefetch | 19.079410 → 17.671742 | −7.38% | −1.49% | Rejected after integrated comparison below |

The combined row changes both specialization and prefetch and cannot isolate
their contributions. Worker CPU time improves much less than elapsed time; in
that series average utilization rose from 713.61–759.53% to 762.08–781.81%.
Scheduling and host contention therefore matter. These percentages must not be
added, multiplied, or presented as full-PRESYNC improvements. Hash-process peaks
were approximately 72 MiB, with no material RSS improvement established.

## Proof-cache results and cost

At 44.9 million synthetic entries and a nominal 2,048 MiB budget, sampled misses
were 24.0485% for existing growth, 6.6787% for a final remainder tier, and zero of
462,890 probes for fixed segments. This was one run per policy, not a PoW timing
comparison. Existing growth allocated 1,294.734 MiB of entries and peaked at
1,318.75 MiB RSS; segments allocated 2,046 MiB and peaked at 2,080.50 MiB RSS.
Metadata is additional to the nominal entry allowance. Segments allocate more
memory earlier; no 4/5 GiB policy is adopted or justified by this screen.

A real-header ABBA comparison used 100,000 headers and a scaled 4 MiB cache,
changing repeat chunks from 2,000 to 1,999 headers. Delayed recomputation fell
from 85,179–85,204 genuine hashes to zero, and repeat time from 43.241–45.738
seconds to 0.112560–0.112937 seconds. Cold passes still computed every proof.
This demonstrates changed-boundary delayed reuse, **not faster initial PRESYNC**.
Normal identical-message repeats can reuse existing batch evidence. The cache
replacement remains unadopted because its earlier memory cost is established
while the intended initial-presync benefit is not.

## Correctness evidence and remaining limits

Each shared, combined rolling, outer-dispatch and prefetch-only candidate passed
the 8,691-input full-digest comparison with genesis known-answer validation, the
279-case API comparison including nine invalid cases, and fresh-scratch mmap
failure injection. These are comparisons against separately compiled unchanged
original code, not an independently authored implementation of the algorithm.

The independent API suite's latest **combined rolling** ASan/UBSan run passed
**167 valid cases, 46 invalid cases and 52 local initialization/free cycles**.
It includes one-condition dispatch-boundary changes, both algorithm versions,
generic parameter alternatives, scratch reuse/recovery, canaries and four threads
with independent locals. Leak detection and sanitizer halt-on-error were enabled.
Its recorded `tls_cases=0` means legacy TLS coverage must not be claimed.
Earlier 159-case suite results are separate; they do not establish the added
cases passed for other variants. The permanent Boost suite built and passed
**3 cases / 96 assertions against unchanged baseline crypto**, including
14 golden answers with independent near-match parameter conditions, invalid
parameters and scratch lifecycle checks. These finite checks do not establish
equivalence for every input, compiler or platform.

## Completed integrated presync comparison

Two completed ABBA groups used the real verification path through
`bench_header_pow --presync`: 40,000 unique headers, eight workers and eight peer
states, producing 320,000 peer-header validations and 40,000 cold Yespower calls
per run. Every repeated pass performed zero new Yespower calls. This includes
contextual validation but **does not include TCP/P2P traffic or a complete node
PRESYNC**. Process CPU time includes setup, cold and repeat work; it is distinct
from the hash-only worker CPU time above.

| Order | Implementation | Cold seconds | Repeat seconds | Process CPU seconds | Process peak RSS, MiB |
|---:|---|---:|---:|---:|---:|
| 1 | Baseline | 21.1619 | 0.385217 | 143.620274 | 96.723 |
| 2 | Combined rolling | 21.6379 | 0.413835 | 143.521325 | 96.730 |
| 3 | Combined rolling | 22.8709 | 0.487692 | 149.896897 | 97.031 |
| 4 | Baseline | 21.9829 | 0.389456 | 145.564309 | 96.441 |
| 5 | Baseline | 21.1324 | 0.423622 | 142.726779 | 96.625 |
| 6 | Combined rolling | 20.9077 | 0.398888 | 143.372720 | 96.047 |
| 7 | Combined rolling | 20.9594 | 0.406598 | 144.576569 | 96.980 |
| 8 | Baseline | 21.5064 | 0.380317 | 142.692140 | 96.566 |

Cold medians were **21.33415 → 21.29865 seconds (−0.1664%)**, with substantially
overlapping ranges. Whole-process CPU medians were **143.173527 → 144.048947
seconds (+0.6114%)**. The maximum process RSS across each variant's four runs rose
from **96.723 to 97.031 MiB**. All runs exited successfully with zero memory-limit,
OOM or OOM-kill events. These results show no demonstrated integrated presync
benefit or memory reduction, so the combined candidate is rejected as well.

The proposal's feasibility gate failed: there is no measured candidate benefit
that justifies proceeding to a full multi-hour PRESYNC trial. No such trial was
run, and no three-hour completion or whole-node resource compliance is claimed.
The retained changes are measurement support, documentation and separately
validated regression coverage, not these experimental runtime optimizations.

## Controls, provenance and resuming

Accepted timings ran sequentially with affinity `0-3,8-11` and page-rounded
`memory.max=9,999,998,976` bytes. CPU quota was unavailable; affinity, not a claimed
`CPUQuota=800%`, limited execution capacity. Process RSS/HWM was sampled roughly
every 250 ms. Small sampled CPU overshoots occurred from tick/timestamp resolution;
none of this establishes a whole-node 800%/10 GB guarantee. The original `0-7`
affinity timing screen and the failed CPU-quota setup are excluded. The live node
continued running, so host contention remains a limitation.

Repository checkpoints are `71c84b0796` (initial evidence), `cc06618d89` (optional
`--delayed-reuse` benchmark), `6bbcb9c3a3` (shared/prefetch evidence), and
`481cdf9e3a` (permanent Yespower regression tests). None changes node runtime
behavior. See [the historical checkpoint](presync-three-hour-experiments.md)
for detailed measurements and reproducible commands.

`$EVIDENCE_ROOT` denotes retained private artifacts. Completed evidence includes
`compute-balanced-results.json`, the four `*-time-results.json` series, their
`*-check-results.json` files, and `cache-balanced-delayed-results.json`.
The new sanitizer result is `api-correctness/result-rolling-asan-v2.log`, with
`revision2-status.json` and `build-rolling-asan-v2.json` recording its provenance.
`integrated-results.json` and its eight run directories contain the completed
presync comparison. Build hashes and original logs preserve rejected candidates
for review without installing them in the operating node.
