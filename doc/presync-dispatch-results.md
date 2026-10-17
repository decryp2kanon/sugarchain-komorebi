# Presync root-cause reanalysis and bounded dispatch results

2026-10-15. Runtime baseline: `9a1d3e695e9d1916edcf544d115bd2c27e98bb94`.

## Decision

**Do not adopt the persistent-dispatch runtime candidate. The three-hour target
was not achieved.** Two uninstrumented-clock ABBA groups changed direction:
3.26% shorter, then 4.38% longer. The overall cold median was 1.98% longer.
The operating node, its settings/data, and normal daemon/GUI/test executables
were not replaced or restarted. Only benchmark instrumentation and documentation
are retained. The candidate remains a private reproducible experiment.

The useful new finding is that the measured gap between fixed-eight epochs
within the same message was small: 130.32 ms across a roughly 19-second driver, with only 16.32 ms less in
the candidate trace. This does not support removing hours through dispatch gaps
alone. Most time lay inside the envelope from the first job's start to the last
job's finish, which includes both proof computation and unequal completion times.
It is **not** a measurement of pure hash CPU time or pure scheduler idle time.

## Measurement definitions and controls

Each driver uses the same real mainnet fixture prefix: 40,000 distinct headers.
SHA-256: `f68ef2e58dc8bac7ed58ed027c7a86ec0725673ba9f7039867050e7e105c3ed7`.
Raw ordered hash-result SHA-256 matched in every run:
`154067701b1d1a32eb064502bc878d2b51c24f63acc5faff414ef60fb5526a4c`.

The benchmark independently checks fixture-identifier uniqueness and reports
actual Yespower calls. These are distinct checks, not per-call identity tracing.
Every cold driver run made 40,000 proof calls and every repeat made zero.
The eight-peer driver processes 320,000 peer headers; its printed peer-header
rate must not be mistaken for unique-proof throughput.

Both raw and integrated drivers use `CLOCK_THREAD_CPUTIME_ID` around Yespower.
Summed call wall times are reported separately and cannot be added to caller
wall time. `--stage-timing` adds disjoint verifier/context/other caller intervals.
`--no-pow-timing` disables per-proof clocks while retaining the call counter;
the acceptance comparison also omits caller-stage instrumentation.

All measurements were sequential. Builds and other owned heavy tests were
finished before timing. Each owned process tree used the same eight distinct
physical cores, logical CPU IDs `0,1,2,3,8,9,10,11`, inherited by all children.
The user cgroup did not expose CPU quota, so this is an affinity execution
ceiling, **not** an enforced `CPUQuota`. MemoryMax was 10,000,000,000 bytes
(kernel page-rounded `memory.max=9999998976`). No OOM event occurred.
The existing operating node continued running and could contribute contention.

Process RSS and cgroup memory are different metrics. The small fixtures below
do not fill the long-run proof cache or prove a complete node's RSS stays below
10 GB during full PRESYNC/REDOWNLOAD. The original seven-hour run cannot be
retrospectively decomposed by these new clocks.

## Same-clock diagnostic ladder

Four runs per mode, mirrored order, 16 runs total. Values are medians; ranges
are observed run ranges. These are diagnosis results, not four interchangeable
implementations of the same complete network path.

| Mode | Wall seconds (range) | Proof CPU seconds | Unique proofs/s |
|---|---:|---:|---:|
| Continuous raw hashing, eight workers | 16.7091 (16.4475–17.5204) | 129.3539 | 2,393.9 |
| Raw hashing with an eight-item barrier | 18.7801 (17.3132–20.7098) | 132.6064 | 2,129.9 |
| Actual verifier, one peer, 2,000-header messages | 18.0537 (17.6041–20.1114) | 129.9875 | 2,215.6 |
| Actual verifier plus eight PRESYNC states | 19.2182 (18.6646–19.7295) | 131.2795 | 2,081.4 |

The eight-peer context stage took a median 0.05678 s, approximately 0.297% of
cold elapsed time. This supports concentrating on the proof-verifier path;
it does not justify multiplying a context-only speedup by the historical seven
hours. Raw timings include final scratch destruction and thread joins; the
integrated cold timer excludes pool/fixture/state preparation. The raw barrier
also uses fixed worker lanes rather than production dynamic claims. Therefore
raw-versus-integrated differences are not a direct dispatch-cost subtraction.

## What the new candidate changed

One pool still serves both `Check` and `CheckEach`, with seven background workers
and the caller. A fixed slot array and one mutex/condition variable replace the
repeated generic queue submission/recovery path. The last finishing worker
prepares the next epoch only after every current task has returned. No busy spin,
second pool, extra speculative epoch, cache-policy change or algorithm change
is introduced. The first-header admission and maximum-eight-cold-proof failure
boundary remain intact. `CheckEach` retains its existing bounded supply policy.

This source was built and tested privately, never installed as runtime code.
Candidate SHA-256:
`a06102a9d1c9dc6f6f4652b22d313f43447f48d91faba128420dfcc3c0588116`.

## Epoch trace: diagnosis, not acceptance timing

Both traces were complete and consistent: 5,026 epochs, 56 traced messages,
39,991 wrapper jobs, no dropped records or invalid results. First serial
admission and warm paths are outside this trace; the driver counters still
recorded 40,000 cold proofs and zero repeat proofs.

| Nonoverlapping epoch partition | Baseline seconds | Candidate seconds |
|---|---:|---:|
| Prepare to dispatch | 0.120439 | 0.132737 |
| Dispatch to first job start | 0.025075 | 0.036973 |
| First job start to last job finish | 18.572366 | 18.304389 |
| Last job finish to completion observation | 0.038034 | 0.000754 |

The job envelope occupies 99.02% and 99.08% of the respective epoch spans.
Completion observation differs: the baseline caller has returned from Complete;
the candidate last finisher has decided completion. The apparent 98% decrease
in that final partition is not an independently comparable speedup.

The common last-finish-to-next-first-start interval, summed over 4,970 transitions
within the same message, decreased from **130.323 to 114.000 ms**, or 12.53% of
that small interval. Cross-message gaps, first serial admission and warm paths
are excluded; this is not the complete supply gap. It overlaps the table partitions
and must not be added to them. Its absolute difference is **16.323 ms**.
The baseline also had a 111.06 ms epoch outlier (candidate maximum 17.27 ms);
the cause was not established. Trace overhead and this variability require the
separate clock-disabled acceptance comparison.

## Clock-disabled candidate comparison

Actual proof/PRESYNC-state driver, two ABBA groups, four runs per arm, 40,000
unique inputs and 320,000 received peer headers per run. A means baseline.

| Comparison | Baseline seconds | Candidate seconds | Candidate time change |
|---|---:|---:|---:|
| Group 1 mean | 19.82920 | 19.18295 | 3.26% shorter |
| Group 2 mean | 19.17390 | 20.01335 | 4.38% longer |
| Overall median | 19.23200 | 19.61330 | 1.98% longer |
| Observed range | 19.0763–20.4659 | 18.6393–20.5267 | Overlapping |

Median benchmark-process peak RSS was **96.449 → 96.463 MiB**, effectively
unchanged. Median whole-process CPU time was **134.782 → 135.387 s** (+0.45%);
this includes untimed fixture preparation and the repeat pass, not just cold
verification. The run-to-run direction reversal fails the proposed adoption
gate. Per the plan, the candidate was not expanded to 100,000–1,000,000 inputs,
sanitizer qualification or a multi-hour acceptance run.

## Actual P2P diagnostic intervals

Fresh private mainnet datadirs, unchanged minimum chainwork, no external peers,
no whitelisting, eight proof workers. Responders supplied real 2,000-header
messages using ordinary getheaders requests. The staggered scenario had eight
responders with configured response/start delays. Each responder reached 40,000
`presynced_headers`; the active/index chain remained genesis-only and IBD true.
All three runs exited normally and strict trace parsing reported no warnings.

| Diagnostic arm | All-peer interval | Proof-verifier share of message handling | Node peak RSS |
|---|---:|---:|---:|
| Baseline, one responder | 19.7404 s | 99.8826% | 143.80 MiB |
| Baseline, eight staggered responders | 19.8421 s | 99.1430% | 150.59 MiB |
| Candidate, eight staggered responders | 19.9982 s | 99.1126% | 151.23 MiB |

Every run recorded **40,003 whole-process Yespower calls**, including startup
and cleanup, with zero hash errors. Whole-process proof CPU totals were 137.782,
135.101 and 132.627 s respectively. These are not per-message unique call counts.

For baseline staggered peers, receive-to-handler p95 was 1,831.5 ms, while
response-scheduling delays overlap across peers. Their summed 21.125 s is not
21 seconds of node idle time. This distinguishes queued processing from missing
input. It does not establish internet-wide supply latency or recreate the
historical seven-hour run. The instrumentation and one run per scenario make
these diagnosis-only data; they do not rescue the failed acceptance comparison.

## Safety coverage and remaining limits

- Private candidate: existing `header_pow_tests`, **31 cases / 54,986 assertions**
  passed, including bounded invalid-proof work, evidence and failure recovery.
- Baseline and candidate separately: **4 additional cases / 322 assertions each**
  passed for cross-API evidence/reuse/concurrent calls, 2/4/8-worker boundaries,
  partial-epoch exceptions/draining and delayed completions. No existing
  speculation assertion was relaxed. The small delay is a test stimulus, not
  a performance assertion or a guaranteed OS execution order.
- Five 6,000-input preparation smokes, 16 diagnostic ladder runs, two complete
  epoch traces, eight clock-disabled comparison runs and three real-P2P probes
  passed their fixture/call-count/normal-exit checks.
- No claim of a new full unit/functional suite, ASan/UBSan/TSan qualification,
  completed full-mainnet PRESYNC, long-run RSS ceiling or replay performance.

## Three-hour feasibility and next evidence

Conditionally using 37.5 million unique proofs and all eight CPU cores for
10,800 seconds allows **2.304 ms CPU/proof**. The raw median in this experiment
was **3.23385 ms CPU/proof**. Applying that measured cost with perfect utilization
gives approximately **4 h 13 m of compute alone**; reaching the three-hour budget
would require about **28.75% less CPU cost per proof**, before other overhead.
This is a cost model, not a physical hardware limit or a completed-run forecast.

Revisiting generic dispatch or merely increasing RAM is not justified by these
measurements. Further work should first establish a repeatable new compute
bottleneck, or measure actual long-run rehashes/supply stalls. Those would need
new evidence beyond the already rejected fixed-parameter/prefetch/cache trials.
Preserve the eight-proof failure-cost boundary; the rejected 512-item supply
experiment is not silently reintroduced to obtain a faster number.

## Artifacts and review scope

Private evidence: `$EVIDENCE_ROOT/presync-root-cause/`, including `baseline.json`,
`final-provenance.json`, `ladder-results.json`, `candidate-results.json`,
`trace/analysis.json`, `p2p-trace-results.json`, resource samples, command logs,
source copies and fixture/output hashes. See the
[investigation checkpoint](presync-dispatch-investigation.md) and
[previous compute follow-up](presync-three-hour-results.md).

Retained benchmark commit: `301f6a6971`. This work belongs to the presync part
of PR #11's future **Lazy PoW / RSS management / presync performance** split.
No PR split or merge was performed.
