# Presync worker-supply experiment: speed and rejection-work tradeoff

Date: 2026-10-14. Baseline: `a57c29924dd9f2b09615685228325e8021c8a19f`.

## Decision

**No runtime candidate is retained.** Larger supply queues reduced waiting and improved the measured short intervals, but did not preserve the existing invalid-PoW speculation bounds. Reusing the eight-item submission vector showed no performance benefit. Production source and normal daemon/GUI/CLI/test binaries were restored before the final report; the prior exact arithmetic and MTP changes remain present.

The 512-item prototype improved the 100,000-header synthetic presync interval by **9.61%**, not 42%. Real P2P first-peer interval observations were approximately 10–12% shorter under the tested conditions. Neither result is a measurement of a complete mainnet presync, and neither is shipped. **Do not claim a seven-hour-to-four-hour result or apply these percentages to full IBD.**

## What was changed privately

The experimental `Check()` path retained the separate first-header admission check and maximum eight concurrent proof computations. Only its supply amount was selectable as 8/32/64/128/256/512. A separate eight-item variant retained the producer vector between completed epochs; the queue still received moved elements and completed each epoch before reuse. These experiment-only environment switches are absent from the restored normal executables.

A private optional trace measured completed queue condition waits, queue-add duration and completion duration. The benchmark's real Yespower wrapper measured call wall/CPU time. A separately linked private daemon recorded real Yespower calls and thread CPU time over its entire lifetime. No proof result was substituted. None of the private tracing, switches, queue changes or new flags is installed in production.

## Baseline and conditions

The observed historical presync interval was 10:38:02–17:51:34 on October 14: **26,012 seconds**, approximately 37.5 million height progress, with 5,177 resource samples averaging **462.6% process CPU**. This includes network/producer waits and is not a count of unique physical proofs.

The earlier 42% scenario assumed that CPU would rise from 462.6% to 800% and useful throughput would increase in exact proportion. This was conditional arithmetic, not an implementation forecast or a demonstrated limit. In the new short diagnostic, the current eight-item method already spent about 6.92 cores' worth of CPU on hashing. The historical process-wide samples and this short hash-CPU metric have different scopes and cannot be directly equated.

Tests used the same Ryzen 9 5950X host, GCC 11.4.0, existing RelWithDebInfo `-O2 -g` build, eight workers, and 2,048 MiB growing-cache budget. The operating node continued running. All build/regression jobs finished before sequential timing comparisons. Host contention, scheduling and clock behavior remain possible sources of variation.

The real mainnet fixture was the existing first 100,000 headers. The synthetic `--presync` driver uses eight independent states, checks work/transitions, and receives eight times COUNT headers with shared evidence. Each cold run performed exactly COUNT actual Yespower calls; every repeat performed zero. It excludes sockets, net_processing index lookup and REDOWNLOAD, and does not exercise a fully populated production cache.

```sh
./bench_header_pow HEADERS.raw 40000 8 2048 --grow-cache --presync
./bench_header_pow HEADERS.raw 100000 8 2048 --grow-cache --presync
```

## Supply-size screening

Two runs per variant in mirrored execution order. `baseline` is the saved unmodified executable; `private-8` is the private prototype with its original eight-item supply, providing a control for prototype/layout effects. RSS includes untimed preparation and repeat processing.

| Variant | Cold median s | Cold range s | Reduction vs unmodified baseline | Median process peak MiB |
|---|---:|---|---:|---:|
| baseline | 18.525800 | 18.3184–18.7332 | 0.000% | 93.701 |
| private-8 | 18.295350 | 18.1382–18.4525 | 1.244% | 93.916 |
| 32 | 17.913900 | 17.8178–18.0100 | 3.303% | 94.057 |
| 64 | 17.502300 | 17.4054–17.5992 | 5.525% | 93.736 |
| 128 | 17.459150 | 17.1125–17.8058 | 5.758% | 95.016 |
| 256 | 17.557700 | 17.2444–17.8710 | 5.226% | 94.537 |
| 512 | 17.262750 | 17.0372–17.4883 | 6.818% | 94.611 |
| reuse8 | 19.425800 | 19.1357–19.7159 | -4.858% | 93.910 |

The private eight-item control differs from the unmodified baseline by 1.24%. The 512 prototype is **5.64% shorter against that same-prototype control**, or 6.82% against the saved unmodified binary. Do not silently choose whichever reference gives a larger gain. Two samples per variant do not establish a universal ordering of intermediate sizes.

The reused-vector variant was observed 4.86% slower than the saved baseline. No gain is demonstrated, so it is rejected rather than adding a new runtime implementation.

## Longer synthetic comparison

100,000 real headers per state; 800,000 received headers, exactly 100,000 cold physical proofs and zero repeat proofs in every process. Order: private-8 / 512 / 512 / private-8.

| Supply | Cold median s | Range s | Median process peak MiB |
|---|---:|---|---:|
| 8 | 46.412750 | 46.3395–46.4860 | 107.846 |
| 512 | 41.951200 | 41.8299–42.0725 | 107.959 |

Observed cold time: **46.412750 → 41.951200 seconds, 9.61% lower**. Mixed sub-MiB RSS differences do not establish a memory saving or full-presync RSS below 5 GiB.

## Separate queue diagnostic

Diagnostic runs are a separate series, in 8/512/512/8 order, with thread-CPU and queue timing enabled. They are not mixed into the uninstrumented screening/longer results.

| Metric, median of two cold runs | 8 items | 512 items |
|---|---:|---:|
| Cold wall seconds | 18.871100 | 17.120800 |
| Actual Yespower calls | 40000.000000 | 40000.000000 |
| Summed Yespower thread CPU seconds | 130.678500 | 130.579000 |
| Completed supply epochs | 5026.000000 | 104.000000 |
| Completed worker condition-wait seconds, summed | 16.637150 | 4.555675 |
| Completed master condition-wait seconds | 1.878740 | 0.147811 |
| Queue-add wall seconds | 0.071466 | 0.001606 |
| Hash CPU / cold wall, percent | 692.49% | 762.70% |

The call count and total hash CPU were nearly unchanged, while queue waits and epoch count fell. This supports a waiting/supply benefit in this fixture; it does not establish the bottleneck distribution of the historical seven-hour interval.

Wait counters sum completed condition-variable wait spans across threads, clipped at the active phase start; waits still pending at phase end are not included. They include wake/relock time. They are **not unique elapsed seconds or exact total worker idle time**, and cannot be added to wall time. Completion duration includes the master doing proof work and waiting; it is not pure sleep time. Profiling adds overhead. Per-task tail timestamps and a scheduler-level decomposition were not collected. Summed Yespower wall time can overlap across threads and is not CPU time; thread CPU includes in-call memory stalls and does not identify their cause.

## Actual P2P interval comparison

One responder, eight same-chain responders, and eight staggered responders. For each scenario, private-8/512/512/private-8, two runs each. Fresh owned mainnet datadirs, loopback only, 2,000-header wire messages, normal minimum-chainwork rules, no whitelist/noban privilege. All peers reached 40,000 presynced headers, with genesis-only active/index chain, IBD still true and no unexpected peer. The terminal response is withheld to preserve the presync state; **these runs do not finish mainnet presync**.

Every response uses a deliberately injected 100 ms delay with deterministic ±10 ms jitter; staggered responders add peer-index times 250 ms initially and 25 ms per response. This is not a measurement of Internet RTT or production fixed waits. First-peer and all-peer intervals are separate metrics.

The private daemon wrapper recorded **40,003 actual Yespower calls in every one of the 12 runs**, with no library error. This is a whole-process lifetime count including startup/cleanup, not an interval-only or per-peer counter. It is not 320,000 physical computations for eight peers. The synthetic benchmark separately resets its counters around each cold/repeat pass.

| Scenario | Supply | First-peer median s | First-peer range s | All-peer median s | Median peak MiB |
|---|---|---:|---|---:|---:|
| one | 8 | 20.600041 | 20.369265–20.830817 | 20.600041 | 140.498 |
| one | 512 | 18.367607 | 18.314382–18.420832 | 18.367607 | 140.549 |
| eight-same | 8 | 20.358960 | 20.210021–20.507899 | 20.369657 | 147.488 |
| eight-same | 512 | 18.377941 | 18.338826–18.417055 | 18.388388 | 147.619 |
| eight-staggered | 8 | 20.937304 | 20.171075–21.703533 | 21.231790 | 147.170 |
| eight-staggered | 512 | 18.519452 | 18.461189–18.577714 | 19.082042 | 148.059 |

First-peer observed reductions: one **10.84%**, eight-same **9.73%**, eight-staggered **11.55%**. All 12 interval runs completed normally. These are short fixtures with two runs per supply/scenario on a shared host. Real proof CPU time also varied across processes. They do not establish a full-presync percentage, production peer responsiveness under attack, long-run cache eviction or a five-GiB whole-node limit.

## Invalid-proof work: why the larger queue is rejected

The existing `invalid_first_and_later_proofs_have_bounded_speculation` test uses genuine easy-target Yespower inputs and invalid proofs at positions 0/1/7/8/9/16/24 in a 25-header input. The current eight-item version passed all 42 assertions. The 512 variant failed five assertions:

- Three upper-bound failures observed **25 actual calls against the allowed maximum of 9**.
- Another upper-bound failure observed 18 calls against 17.
- One lower-bound assertion changed because the LIFO queue can find a later invalid proof before examining the earlier groups; that is a scheduling-order difference, not additional acceptance of an invalid proof.

The first invalid header's serial admission check, maximum eight simultaneous checks, rejection and worker cleanup still passed. **The failure is increased unnecessary work before rejecting invalid PoW**, not a demonstrated consensus acceptance bug.

A 25-header fixture does not measure a 513-call worst case. With a separately admitted first header and up to 512 cold jobs supplied, the supply range can reach 513 checks before the next outer decision. Cancellation cannot undo a Yespower computation already started. A larger input/adversarial timing matrix was not pursued after the existing upper-bound requirement already failed.

The reused-eight variant passed the whole normal header-PoW suite: **31 cases / 54,986 assertions**, including the speculation/concurrency/error/cache checks. It was rejected for lack of performance benefit. The restored normal binary passed the original speculation test again: **1 case / 42 assertions**. No full 816-case run or new sanitizer run is claimed for these rejected private experiments.

## Final state and follow-up

Normal source for `pow.cpp`, `checkqueue.h` and the benchmark exactly matches the saved baseline. Normal daemon, GUI, CLI, benchmark and test targets were rebuilt; normal daemon/GUI/benchmark binaries were checked for absence of the private supply/reuse switches. The operating node was never restarted or modified, and its data/settings were preserved.

A production 512 queue would change the previous invalid-input work policy, even with unchanged validation rules and eight concurrent workers. Adopting that tradeoff requires a separate proposal quantifying per-peer/global resource budgets and an explicit user decision. This execution does not weaken the bound or silently accept a larger DoS cost. A scheduling redesign preserving the same failure boundaries remains a separate candidate; the present measurements are not proof that such a design will yield a large speedup.

Local source snapshots, rejected patch, binary digests, JSON/CSV results, test logs and P2P datadirs are preserved under `~/.local/state/sugarchain-komorebi/presync-worker-supply/`. PR #11 remains a staging PR to split into **Lazy PoW / RSS management / presync performance**. This experiment belongs to the presync group. No split or merge was performed.

## Individual runs

Screening rows are separate fresh processes, in execution order.

| Order | Variant | Cold s | Repeat s | Peak MiB |
|---:|---|---:|---:|---:|
| 1 | baseline | 18.733200 | 0.386998 | 94.137 |
| 2 | 8 | 18.138200 | 0.380525 | 94.066 |
| 3 | 32 | 18.010000 | 0.379784 | 94.340 |
| 4 | 64 | 17.599200 | 0.415454 | 93.922 |
| 5 | 128 | 17.112500 | 0.391234 | 94.762 |
| 6 | 256 | 17.244400 | 0.384462 | 93.914 |
| 7 | 512 | 17.037200 | 0.385562 | 94.457 |
| 8 | reuse8 | 19.135700 | 0.418691 | 93.566 |
| 9 | reuse8 | 19.715900 | 0.401440 | 94.254 |
| 10 | 512 | 17.488300 | 0.385561 | 94.766 |
| 11 | 256 | 17.871000 | 0.388404 | 95.160 |
| 12 | 128 | 17.805800 | 0.393014 | 95.270 |
| 13 | 64 | 17.405400 | 0.400880 | 93.551 |
| 14 | 32 | 17.817800 | 0.380827 | 93.773 |
| 15 | 8 | 18.452500 | 0.389594 | 93.766 |
| 16 | baseline | 18.318400 | 0.386681 | 93.266 |

### Longer runs

| Order | Supply | Cold s | Repeat s | Peak MiB |
|---:|---|---:|---:|---:|
| 1 | 8 | 46.486000 | 0.949888 | 107.809 |
| 2 | 512 | 41.829900 | 0.958320 | 108.270 |
| 3 | 512 | 42.072500 | 0.984788 | 107.648 |
| 4 | 8 | 46.339500 | 1.012230 | 107.883 |

### Instrumented diagnostic runs

| Order | Supply | Cold s | Repeat s | Peak MiB |
|---:|---|---:|---:|---:|
| 1 | 8 | 18.688700 | 0.433807 | 94.281 |
| 2 | 512 | 16.977400 | 0.419651 | 94.375 |
| 3 | 512 | 17.264200 | 0.473504 | 94.531 |
| 4 | 8 | 19.053500 | 0.384256 | 94.320 |

### P2P runs

| Order | Scenario | Supply | First peer s | All peers s | Process Yespower calls |
|---:|---|---|---:|---:|---:|
| 1 | one | 8 | 20.830817 | 20.830817 | 40003 |
| 2 | one | 512 | 18.420832 | 18.420832 | 40003 |
| 3 | one | 512 | 18.314382 | 18.314382 | 40003 |
| 4 | one | 8 | 20.369265 | 20.369265 | 40003 |
| 5 | eight-same | 8 | 20.210021 | 20.220276 | 40003 |
| 6 | eight-same | 512 | 18.338826 | 18.349041 | 40003 |
| 7 | eight-same | 512 | 18.417055 | 18.427734 | 40003 |
| 8 | eight-same | 8 | 20.507899 | 20.519038 | 40003 |
| 9 | eight-staggered | 8 | 21.703533 | 21.995945 | 40003 |
| 10 | eight-staggered | 512 | 18.577714 | 19.139295 | 40003 |
| 11 | eight-staggered | 512 | 18.461189 | 19.024790 | 40003 |
| 12 | eight-staggered | 8 | 20.171075 | 20.467636 | 40003 |
