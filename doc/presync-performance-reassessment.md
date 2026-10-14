# Additional presync performance reassessment

Date: 2026-10-14. Production baseline: `3882711922`.

## Decision

Neither new runtime prototype is retained. Prepared proof keys regressed the cold path; grouped cache publication did not demonstrate a gain distinguishable from host contention. The previously adopted exact-chainwork optimization remains in place. There is **no demonstrated additional full-presync time reduction or production RSS reduction**, and this experiment does not establish a sub-three-hour presync.

The retained change extends `bench_header_pow` to exercise genuine Yespower checks, growing proof-cache behavior and eight independent presync state machines. No production verifier, consensus rule, worker limit or networking policy changes are included.

## Measurement design

The first 40,000 contiguous real mainnet headers from the existing `headers-100000.raw` fixture were used. Each of eight synthetic peers traversed all headers, with staggered chunk sizes of 1,000 through 1,875. Cold passes received 320,000 headers but performed exactly 40,000 Yespower calls. Repeated passes performed zero Yespower calls. Each state's cumulative work and transition to REDOWNLOAD were checked.

Run baseline/candidate/candidate/baseline in separate processes:

```sh
/usr/bin/time -f 'peak_kib=%M wall_s=%e' ./bench_header_pow   HEADERS.raw 40000 8 2048 --grow-cache --presync
```

The 2,048 MiB value is the cache budget, not measured allocation. This small fixture does not exercise full-cache eviction. Workers remain eight. The operating node continued running throughout; its changing CPU/memory contention limits attribution. No build or unit-test job overlapped the timed comparisons.

This is a synthetic state-machine driver, not a P2P replay: short chunks deliberately use `full_headers_message=true` to continue the stream. It excludes socket delivery, `net_processing` index lookup and REDOWNLOAD processing. It cannot predict the duration or RSS of 45 million headers.

`yespower_thread_wall_seconds` sums wall time inside actual Yespower calls across threads. It includes descheduling and memory waits; it is neither CPU time nor a rigorous measure of the serial fraction. Instrumentation is confined to the benchmark wrapper.

## Results

All durations below are seconds; peak RSS is for the benchmark process only.

| Candidate | Baseline cold median | Candidate cold median | Cold change | Decision |
|---|---:|---:|---:|---|
| Prepared keys and bulk cache lookup | 28.7789 | 32.5662 | 13.16% longer | Rejected |
| Grouped evidence publication and task-vector reuse | 36.4521 | 31.4183 | Apparently 13.81% shorter | Inconclusive; rejected |

### Prepared keys

| Order | Cold | Repeat | Summed Yespower thread wall | Peak RSS MiB |
|---|---:|---:|---:|---:|
| A1 baseline | 28.0357 | 0.409055 | 196.873 | 93.4531 |
| B1 prepared | 33.4225 | 0.415071 | 222.051 | 93.1758 |
| B2 prepared | 31.7099 | 0.398454 | 210.158 | 93.3008 |
| A2 baseline | 29.5221 | 0.432288 | 206.574 | 93.9180 |

Repeat median improved about 3.31%, but ranges overlap. Peak median differed by only -0.48%, which does not establish a useful memory gain. The cold regression outweighed the unproven warm benefit.

### Grouped publication

| Order | Cold | Repeat | Summed Yespower thread wall | Peak RSS MiB |
|---|---:|---:|---:|---:|
| A1 baseline | 39.9974 | 0.422754 | 282.038 | 93.3047 |
| B1 grouped | 31.9540 | 0.391810 | 224.944 | 95.7695 |
| B2 grouped | 30.8826 | 0.411310 | 217.326 | 95.6055 |
| A2 baseline | 32.9068 | 0.441257 | 232.418 | 93.3477 |

The baseline itself became 17.73% faster between A1 and A2. Time inside actual Yespower calls moved with total elapsed time, so the apparent gain cannot safely be attributed to publication changes. Peak median increased approximately 2.53%. No production improvement is claimed.

## Other avenues assessed

- In this bounded fixture, all duplicate-peer proofs were reused. Enlarging the cache cannot remove additional Yespower calls here. Mainnet eviction behavior remains unmeasured.
- Source inspection shows the next low-work header request is issued after PoW and state processing (`ProcessHeadersMessage`, `IsContinuationOfLowWorkHeadersSync`, `MaybeSendGetHeaders`). Early speculative requests might overlap network wait and computation, but would change behavior for unverified input and request bookkeeping. They are not adopted without dedicated anti-DoS and peer-protocol tests.
- Live ping samples were taken during replay, not the historical presync interval. They cannot quantify historical presync network stalls.

## Validation and artifacts

The prepared-key prototype passed 31 `header_pow_tests` cases / 54,986 assertions before rejection. The grouped-publication prototype had valid-input benchmark checks only, not a full regression suite. Neither prototype is shipped.

Final benchmark validation covers cold/repeat proof counts, state/work transitions, rejection of a corrupted final-header proof, invalid options and the original CSV consumer. Daemon, GUI, CLI and unit-test targets were rebuilt with production code restored. The operating node and its data were not restarted or changed.

Local raw results, rejected patches, build logs and verification output are retained under `~/.local/state/sugarchain-komorebi/presync-reassessment/`. They are not repository data. The existing `header-pow-ab.py` consumer remains compatible with the extra CSV timing field; its original cold/repeat mode is unchanged.

## Follow-up and PR scope

A meaningful next experiment needs a controlled comparison host or repeatable contention conditions, then actual mainnet presync timing. Do not convert the microbenchmark percentages into a seven-hour or three-hour promise.

PR #11 remains a staging PR to be split into three independent PRs: (1) Lazy PoW, (2) RSS/memory management, and (3) presync performance and diagnostics. This change belongs to the third group. No split or merge is performed here.
