# Presync exact-division and endpoint-MTP optimization

Date: 2026-10-14. Baseline: `8ab68cd32405fd51571b48d35bba2c6bdf23bb87`.

## Decision and scope

Retain two small computation optimizations together. In the measured PRESYNC/REDOWNLOAD state-machine component, median time decreased **4.007%**, from **4.217660 s to 4.048665 s**. This component excludes genuine Yespower calculation and network delivery. The two cycle-specific improvements were 5.41% and 2.10%; the magnitude is not a universal constant.

Actual-proof and real-P2P comparisons did not establish an end-to-end speedup or a consistent regression. **The seven-hour full presync was not repeated, the three-hour goal is not achieved by these measurements, and full-node RSS below 5 GiB is not established.** This incremental result is not added to the earlier chainwork component percentage.

## Implementation and costs

Logical implementation commits: `51f13a2c25` (A), `b1d23b402e` (B). Each was pushed separately.

- **A: combine consecutive exact target divisions.** For positive divisors, `floor(floor(a / b) / c) == floor(a / (b*c))`. Check that the positive product fits `UINT32_MAX` before multiplying. Eligible parameters use one existing exact small-divisor pass; other parameters retain the previous two-division fallback. On mainnet, 510 and 2,550 produce 1,300,500 without embedding a new magic constant. The subsequent span multiplication, clamp and compact rounding stay in the same order. The small-divisor limb loop drops from 16 iterations to 8; this is not a 50% whole-program speedup. No persistent storage is added. This shared arithmetic function also affects full-history difficulty calculations.
- **B: retain immutable endpoint MTP in bounded per-peer history.** Store one `int64_t` alongside each temporary index, calculating a new endpoint once rather than sorting it again later. Read original ancestry when initializing at a non-genesis start, calculate new MTP before eviction, and reset the retained values with their history on pass changes. Normal steady-state endpoint sorts fall from two to one. Near genesis, this adds some early precomputation; initial resets also populate medians.

The MTP value payload is **4,168 bytes per 521-entry peer history**, or **33,344 bytes (32.56 KiB) for eight peers**, plus allocator/container costs. On this build, the temporary entry grows from 152 to 160 bytes; the global `CBlockIndex` remains 152 bytes. libstdc++ still stores three entries per deque node. This is not a new disk field or a cache proportional to all historical headers.

PoW checks, the eight-worker ceiling, fixed-epoch failure-work bounds, chainwork/commitment rules, minimum-work policy, message size limits and scheduling are unchanged. No network prefetch, peer-policy change or operational setting is introduced.

## Measurement controls

AMD Ryzen 9 5950X; GCC 11.4.0; existing RelWithDebInfo `-O2 -g` configuration. The operating node continued running. Builds and regression suites finished before timed comparisons; each comparison process ran sequentially. Changing host conditions still limit attribution.

Two cycles used `baseline / A / B / AB / AB / B / A / baseline`, giving four runs per variant. `AB` means both changes. The same baseline and variant executables were retained with SHA-256 digests in the private evidence directory.

The existing real mainnet fixture was used. The component driver checked genuine PoW before timing, then ran 300 rounds of 6,000 headers through both PRESYNC and REDOWNLOAD, checking transitions and returned headers. Its local threshold permits the fixture's two passes; production chainwork policy is unchanged.

```sh
./bench_headers_sync HEADERS.raw 6000 300
./bench_header_pow HEADERS.raw 40000 8 2048 --grow-cache --presync
```

The second command drives eight synthetic peer states, 320,000 received headers, **exactly 40,000 cold Yespower calls and zero repeat calls in every run**. The 2,048 MiB parameter is a cache budget, not allocated RSS. It has no sockets/index lookup and does not fill the long-run cache. `yespower_thread_wall_seconds` sums call wall time across threads; it is neither CPU time nor serial-path duration.

## State-machine component results

All times are seconds. Peak RSS includes the benchmark process's untimed preparation, so it is not an isolated allocation measurement.

| Variant | Median | Range | Median reduction | Median process peak MiB |
|---|---:|---|---:|---:|
| BASELINE | 4.217660 | 4.14958–4.27509 | 0.000% | 88.029 |
| A | 4.202285 | 4.06565–4.20698 | 0.365% | 88.168 |
| B | 4.127535 | 4.06957–4.21792 | 2.137% | 88.156 |
| AB | 4.048665 | 4.01609–4.11135 | 4.007% | 88.152 |

A alone is indistinguishable from variation; B alone has overlapping ranges. All four AB values were below all four baseline values in this component series. Adoption relies on that small combined result and correctness coverage, not independent whole-presync gains for each commit.

## Actual-proof benchmark

| Variant | Cold median seconds | Cold range | Repeat median seconds | Repeat range | Median process peak MiB |
|---|---:|---|---:|---|---:|
| BASELINE | 30.331450 | 18.4132–37.1862 | 0.396373 | 0.391215–0.428521 | 94.137 |
| A | 29.478600 | 24.3013–36.2374 | 0.421142 | 0.401662–0.453417 | 93.510 |
| B | 30.257450 | 28.8290–31.5629 | 0.417586 | 0.401369–0.442895 | 94.150 |
| AB | 28.572850 | 26.6104–29.8684 | 0.396686 | 0.381421–0.437495 | 94.029 |

AB's apparent 5.80% cold median reduction is **inconclusive**: baseline ranged from 18.41 to 37.19 seconds, and cycle-specific comparison direction reversed. Time inside actual Yespower calls changed alongside elapsed time. Repeat medians differ by only +0.079%, with overlapping ranges. Neither a production regression nor its absence is proven by this noisy series; no useful RSS reduction is claimed.

## Real P2P interval comparison

Fresh owned mainnet datadirs, localhost responders, real 2,000-header messages, real validation, eight proof workers, no minchainwork override and no whitelist/noban privilege. Automatic outbound connections were disabled. Inbound responders follow normal getheaders locators; distinct real INV announcements activate additional peers. Each peer reaches 40,000 presynced headers while the normal active/index chain remains genesis-only and IBD remains true. The terminal response is withheld to preserve measurable presync state. All 12 runs passed these checks and exited normally.

Each scenario uses baseline/AB/AB/baseline, two runs per binary. All responses have a deliberately supplied 100 ms delay plus deterministic ±10 ms jitter. The staggered scenario additionally uses peer-index times 250 ms initial delay and 25 ms per-response delay. These are test conditions, not measured Internet RTT or a hard-coded production wait. Eight peers do not imply eight times the unique hashing work. Actual P2P Yespower calls and cache evictions were **not instrumented**.

First-peer completion measures the first peer reaching the interval boundary; all-peer completion is a separate metric, not the single-chain critical path. Resource values below cover the owned daemon only.

| Scenario | Binary | First-peer median s | All-peer median s | First-peer range s | Median peak MiB |
|---|---|---:|---:|---|---:|
| one | BASELINE | 37.937349 | 37.937349 | 37.044790–38.829907 | 140.572 |
| one | AB | 38.050307 | 38.050307 | 36.482468–39.618147 | 140.383 |
| eight-same | BASELINE | 34.149183 | 34.160506 | 31.903053–36.395312 | 147.326 |
| eight-same | AB | 33.275333 | 33.285877 | 30.968322–35.582344 | 147.588 |
| eight-staggered | BASELINE | 31.266365 | 31.417257 | 30.412787–32.119944 | 147.074 |
| eight-staggered | AB | 31.261377 | 31.414726 | 30.675916–31.846838 | 147.617 |

The first-peer changes are +0.30%, −2.56% and −0.016%, respectively; every scenario has overlapping ranges. These results establish neither a useful end-to-end gain nor a consistent regression. No memory reduction can be inferred from the mixed sub-MiB differences. Short inbound localhost fixtures do not cover long-run cache eviction, outbound peer selection, public-network behavior or full mainnet PRESYNC/REDOWNLOAD duration.

## Private stage attribution

After the uninstrumented matrix, a separate AB diagnostic daemon ran the same 40,000-header fixture with one responder and eight staggered responders. Aggregate per-message stage logs were enabled only in this private executable. The patch was removed from source and normal binaries rebuilt before comparisons; it is not shipped. Both diagnostic runs passed the RPC progress and log-boundary checks. These two runs have no instrumented baseline and are **not an additional speedup comparison**.

| Metric | One responder | Eight staggered responders |
|---|---:|---:|
| Headers messages | 20 | 160 |
| First peer interval | 30.636 s | 32.021 s |
| All peers interval | 30.636 s | 32.033 s |
| Serialized ProcessMessage time | 28.584058 s | 30.204561 s |
| Nested proof-verifier wall time | 28.556702 s | 30.029570 s |
| Proof-verifier share of handling | 99.904% | 99.421% |
| Nested index lookup/refill | 8.183 ms | 53.437 ms |
| Nested state processing | 6.240 ms | 49.433 ms |
| Receive-to-processing median | 0.041 ms | 1,305.241 ms |
| Receive-to-processing p95 | 0.047 ms | 1,636.189 ms |

Nested intervals are already contained in the handling total and must not be added again. Proof-verifier wall time includes cache work, worker scheduling/waits and Yespower; it is not measured Yespower CPU time or a physical call count. State timing does not include the complete initial claimed-chainwork/setup path; other handling includes parsing, continuity checks, initialization and logging. Receive-to-processing uses wall-clock timestamps, unlike internal stage durations.

With eight peers, receive-to-processing waits sum to 188.738 seconds because different peers wait concurrently behind work on the same message-processing thread. That is not 188 seconds added to the 32-second run or removable by a scheduling change. With one peer, queue delay was negligible; the deliberately injected approximately two seconds of responder delay is separate from processing. No cross-process monotonic-clock subtraction was needed for these figures.

These fixtures point to the proof-verifier path as the dominant measured processing cost, explaining why small context arithmetic savings need not improve total time visibly. They do not prove the bottleneck distribution of the historical seven-hour run. The index was genesis-only, so lookup/lock costs with tens of millions of resident indexes or different operating contention remain unmeasured. Long-lived cache eviction, repeated physical proofs, Internet request latency and available worker execution time remain follow-up measurements; speculative request or queue-policy changes are not justified by this evidence alone.

## Validation

- Normal selected chain, PoW, header-verifier, chainwork and SugarShield tests: **66 cases / 7,006,153 assertions passed**. This includes existing invalid-input speculation bounds and shared bounded-worker coverage.
- ASan/UBSan with leak detection: **35 selected cases / 6,909,348 assertions passed**. The separate header-worker suite was in the normal run; this is not a full 816-case sanitizer suite.
- New arithmetic coverage uses an independent two-generic-division oracle for product boundaries, fallback, zero/max/random target sums, compact rounding and span boundaries.
- New MTP coverage includes equal/decreasing timestamps, genesis/short histories, 510/511/520/521/522 boundaries, non-genesis starts, eviction, REDOWNLOAD reset, and wrong-difficulty headers with valid PoW rejected in both passes.
- `p2p_sugarchain_header_pow.py` and `p2p_headers_sync_with_minchainwork.py` passed in private test datadirs after correcting harness executable/config naming. An initial eight-responder smoke also required correcting inbound slot capacity; discarded setup attempts are not timing results.
- Daemon, GUI, CLI, unit and benchmark binaries built. The final normal binaries exclude all private stage instrumentation. No TSan, cross-platform build or full mainnet rerun is claimed.

The running operating node and its datadir/settings were not restarted or modified. Built changes take effect on a later user-initiated restart; this small optimization is not a reason to discard current presync progress.

## Reproducibility and PR scope

Private scripts, source snapshots, executable hashes, JSON/CSV results, test logs and P2P datadirs are retained in `~/.local/state/sugarchain-komorebi/presync-third-pass/`. The 40,000-header input-prefix SHA-256 is `f68ef2e58dc8bac7ed58ed027c7a86ec0725673ba9f7039867050e7e105c3ed7`. P2P baseline executable SHA-256: `3a4f5ba97039f2e88e72e2489535aa59c1419079af1fa0b3ec2031519e221133`; AB executable: `f89cc5a901440a6fefaea512b21b8693f7dd30cb4f3fdcd17d93a88410c1197c`.

PR #11 remains a temporary integration PR to split into **Lazy PoW / RSS management / presync performance** before merge. These changes belong to the presync group. No split or merge is performed.

## Individual comparison results

The row order is execution order. Each component/proof row corresponds to separate processes, not concurrent measurements.

| Order | Variant | Component s | Cold proof s | Repeat s | Proof process peak MiB |
|---:|---|---:|---:|---:|---:|
| 1 | BASELINE | 4.27509 | 18.4132 | 0.391215 | 94.766 |
| 2 | A | 4.20698 | 24.3013 | 0.453417 | 93.477 |
| 3 | B | 4.21792 | 28.8290 | 0.401369 | 94.691 |
| 4 | AB | 4.02324 | 26.6104 | 0.437495 | 94.262 |
| 5 | AB | 4.01609 | 27.4717 | 0.407192 | 94.277 |
| 6 | B | 4.09432 | 29.8470 | 0.414943 | 93.941 |
| 7 | A | 4.20538 | 28.7164 | 0.431337 | 93.398 |
| 8 | BASELINE | 4.22407 | 30.2739 | 0.395242 | 94.176 |
| 9 | BASELINE | 4.21125 | 30.3890 | 0.397503 | 94.098 |
| 10 | A | 4.19919 | 30.2408 | 0.401662 | 94.672 |
| 11 | B | 4.16075 | 30.6679 | 0.442895 | 93.520 |
| 12 | AB | 4.07409 | 29.8684 | 0.381421 | 93.797 |
| 13 | AB | 4.11135 | 29.6740 | 0.386180 | 93.516 |
| 14 | B | 4.06957 | 31.5629 | 0.420229 | 94.359 |
| 15 | A | 4.06565 | 36.2374 | 0.410946 | 93.543 |
| 16 | BASELINE | 4.14958 | 37.1862 | 0.428521 | 92.883 |

| Order | P2P scenario | Binary | First peer s | All peers s | Peak MiB |
|---:|---|---|---:|---:|---:|
| 1 | one | BASELINE | 38.829907 | 38.829907 | 140.500 |
| 2 | one | AB | 39.618147 | 39.618147 | 140.547 |
| 3 | one | AB | 36.482468 | 36.482468 | 140.219 |
| 4 | one | BASELINE | 37.044790 | 37.044790 | 140.645 |
| 5 | eight-same | BASELINE | 36.395312 | 36.405958 | 148.039 |
| 6 | eight-same | AB | 35.582344 | 35.593131 | 147.863 |
| 7 | eight-same | AB | 30.968322 | 30.978624 | 147.312 |
| 8 | eight-same | BASELINE | 31.903053 | 31.915055 | 146.613 |
| 9 | eight-staggered | BASELINE | 30.412787 | 30.703425 | 147.094 |
| 10 | eight-staggered | AB | 31.846838 | 31.856975 | 148.094 |
| 11 | eight-staggered | AB | 30.675916 | 30.972478 | 147.141 |
| 12 | eight-staggered | BASELINE | 32.119944 | 32.131089 | 147.055 |
