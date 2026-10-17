# Guarded final-tier proof-cache experiment results

Date: 2026-10-18. Baseline: `b243458883`.

## Decision

**Do not adopt the runtime candidate.** The guarded remainder passed the existing
header-PoW suite, but both bounded cold PRESYNC ABBA groups regressed. Overall
cold median time increased **17.29%**, mean physical hash calls increased
**20.29%**, and mean phase CPU time increased **18.86%**.
No seven-hour mainnet speedup is established. Production source and executables
remain unchanged; only experimental patches, results and documentation are retained.

## Candidate and safety

The [private candidate patch](benchmarks/presync-cache-remainder-candidate.patch)
is not applied to production pow.cpp. It allows a final remainder only when it
has at least two slots and capacity for the current active tier's insertion count.
Budgets, salt, verification rules, eight-worker maximum, batch reuse, growth timing
and the eight-cold-job failure decision boundary remain unchanged.

The first attempt simply allowed any remainder of two entries or more. It failed
the existing 2048-byte internal-cache retention fixture: two recomputations where
zero were required. It passed 32 cases and failed one, with 55008/55009 assertions.
This was not acceptance of invalid PoW. The existing assertion was not weakened.
The guard fixes that small-cache regression while still allowing the requested
2048 MiB configuration to grow to its valid-store budget.

Guarded candidate: **33 header-PoW cases / 55009 assertions passed**. Baseline
focused checks: **4 cases / 157 assertions passed**. Both allocation diagnostics
passed small/remainder budgets, disjoint negative probes and an injected growth
allocation failure preserving the existing store and normal insertion. These
synthetic diagnostics are separate from genuine PoW verification. No new sanitizer,
full 822-case, functional, operating-node or complete mainnet qualification is claimed.

## Full-budget allocation screen

One synthetic allocation-only run per arm, 21 million keys, no node admission
and no claim of PoW throughput. Existing 44.9-million old-policy results are not
counted as new observations. Both arms passed all 216,495
sampled membership probes, 10000 negative probes and subsequent one-tier shrink.
They do not prove retention of all keys at all later heights.

| Arm | Entry capacity MiB | Tiers | Process peak RSS MiB |
|---|---:|---:|---:|
| Baseline | 1294.734375 | 5 | 1318.258 |
| Guarded remainder | 2046.000000 | 6 | 2080.008 |

Entry capacity increased 58.02%; peak fixture RSS increased
761.750 MiB (57.78%). Metadata, allocator costs and batch storage
are additional to the proof entry budget. This is not full-node RSS.

## Early control at the actual configured budget

40000 unique real mainnet headers, eight peer-state copies, cache budget 2048 MiB,
four runs per arm in two ABBA groups. Each run performed exactly 40000 physical
hashes; final remainder growth was not reached. Baseline/candidate cold medians
were 17.7251/17.4015 s
(-1.83%). Both retained one tier. This small
observational difference cannot be attributed to the unused remainder and does
not pass the predeclared 3%/recomputation gate.

## Primary bounded cold comparison after growth

Scaled 4 MiB budget, 100000 unique genuine mainnet headers, eight independent
PRESYNC states receiving 800000 peer-header copies with deliberately different
1000–1875-header chunks. State/work/transition checks were preserved. This is the
actual proof verifier plus state machine, **not actual P2P or a completed mainnet
presync**. The driver sets its state target work to the fixture end for bounded
transition checks; production minimum chainwork and peer policy are not modified
or validated by that shortcut. The scaled input reaches the last tier in both arms; an early small
fixture that never reaches growth is not used to reject this policy.

| Arm | Median cold s | Mean physical hash calls | Mean phase CPU s | Mean phase CPU utilization | Median process peak MiB |
|---|---:|---:|---:|---:|---:|
| Baseline | 61.0694 | 135368.50 | 437.508 | 709.21% | 96.984 |
| Guarded remainder | 71.6312 | 162834.25 | 520.028 | 719.37% | 97.631 |

CPU 100% means one logical CPU. Phase CPU includes cache and contextual work,
not just time inside Yespower. Physical calls include recomputations; the raw
CSV peer-header throughput is eight times the unique-input progress rate.

| ABBA group | Baseline mean s | Candidate mean s | Candidate time change |
|---|---:|---:|---:|
| 1 | 61.0694 | 73.1228 | +19.74% |
| 2 | 62.3653 | 71.4919 | +14.63% |

Both groups fail the proposed requirement of at least 3% shorter cold time with
less recomputation. The policy was not expanded into actual P2P/full-budget
multi-hour trials or installed in normal node binaries.

## Separate delayed-repeat diagnostic

One ABBA group, two runs per arm; single traversal of 100000 real headers,
4 MiB cache, repeat chunk boundaries 2000 -> 1999. This differs from the concurrent
peer-state working pattern above and must not be substituted for initial PRESYNC.

| Arm | Mean cold s | Mean delayed repeat s | Mean repeat physical calls |
|---|---:|---:|---:|
| baseline | 43.8210 | 36.0520 | 85219.0 |
| candidate | 42.9196 | 26.8299 | 62166.0 |

Mean delayed-repeat time changed -25.58% and physical repeat calls changed -27.05%.
This is conditional on delayed reuse, not evidence of faster first
synchronization. Identical-message batch reuse, cache lifetime and input order
can change the tradeoff.

## Interpretation and limitations

New entries are inserted only into valid.back(). The existing last active table
is 863.15625 MiB at the requested budget; the added remainder is 751.265625 MiB,
about 12.96% smaller. Older tiers are retained but no longer receive new inserts.
Thus more total capacity can coexist with a smaller recent working table. The
scaled comparison makes that tradeoff observable as more genuine recomputation.
This is a code-supported explanation, not per-key eviction tracing: per-tier hit
ages were not recorded, and the exact contribution of each eviction path remains
unmeasured. The fixed-segment design is different and was not tested or adopted here.

Measurements are sequential; owned builds/tests finished before timing. The same
fixture, compiler flags, eight-worker limit, affinity 0-3,8-11, nice 19 and idle I/O
priority were used. Owned timed process groups had an 8 GiB RLIMIT_AS and a
180-second timeout. Affinity is not a CPU quota; RLIMIT_AS is a virtual-address
limit, not cgroup RSS accounting or a complete-node memory guarantee. The host
remained shared with the operating node; contention, random cache salt and code
layout limit fine-grained timing attribution. Differences in repeated hash counts
support rejecting this tested workload, not a universal mainnet slowdown percentage.

No seven-hour duration extrapolation, new option, higher worker count, fake node
proof admission or operating restart is claimed. The primary cold gate failed;
further whole-mainnet testing requires a new reason and separately scoped plan.

## Reproduction and artifacts

[Derived per-run data and aggregates](benchmarks/presync-cache-remainder-2026-10-18.json)
exclude operating logs/settings and private command paths. The
[driver patch](benchmarks/presync-cache-remainder-driver.patch) adds private
cold-only selection, capacity/tier output and phase CPU accounting; it is not
applied to the normal benchmark or daemon. Copy baseline sources to a private
build and apply the supplied patches only there. Compile with the same normal
flags, explicitly linking the private pow object before the normal static
archives. Do not replace operating executables.

```sh
# Private drivers only; substitute the genuine fixture path.
bench-cold-baseline HEADERS.raw 40000 8 2048 --grow-cache --presync --cold-only --no-pow-timing
bench-cold-candidate HEADERS.raw 100000 8 4 --grow-cache --presync --cold-only --no-pow-timing
bench-candidate HEADERS.raw 100000 8 4 --grow-cache --delayed-reuse --no-pow-timing
```

Run baseline/candidate/candidate/baseline twice for each cold scenario; delayed
reuse has one such group. Private complete sources, compiler/link commands,
raw stdout/time records and binary identities are retained under
`~/.local/state/sugarchain-komorebi/cache-remainder-20261018/`.
The initial and revised provenance files describe different candidate revisions;
only the guarded candidate's revised identities apply to the accepted measurements.
This belongs to the presync/cache investigation portion of PR11, to be separated
from Lazy PoW and unrelated RSS work before final review/merge.

EOF
