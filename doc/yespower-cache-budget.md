# Yespower cache budget measurements

The growing process-local cache reuses only successful proofs. A lower
`-maxpowcache` budget limits retained entries; it does not weaken PoW checking.
Misses require real computations. The entry budget excludes metadata and is
not a measurement of whole-node RSS. The default is 16 MiB; the operating
experiment that motivated this comparison explicitly used 2048 MiB.

## Isolated component comparison

An extracted copy of the current GrowingCache implementation, with its actual
CuckooCache and SignatureCacheHasher, was filled with deterministic SHA256
keys. These keys were never injected into the node as verification evidence.
Four budgets were run in forward then reverse order, two processes per
budget/input size. Measurements below are median helper RSS, not node RSS.
After insertion, every 97th key was checked without repopulating misses.

| Entries inserted | Budget (MiB) | Helper RSS (MiB) | Sampled misses |
|---:|---:|---:|---:|
| 1,200,000 | 64 | 46.25 | 15.68% |
| 1,200,000 | 128 | 87.00 | 0.00% |
| 1,200,000 | 256 | 168.75 | 0.00% |
| 1,200,000 | 2048 | 151.62 | 0.00% |
| 2,000,000 | 64 | 46.25 | 47.59% |
| 2,000,000 | 128 | 87.00 | 1.50% |
| 2,000,000 | 256 | 168.75 | 0.00% |
| 2,000,000 | 2048 | 151.75 | 0.00% |
| 6,000,000 | 64 | 46.25 | 82.40% |
| 6,000,000 | 128 | 87.25 | 64.42% |
| 6,000,000 | 256 | 168.75 | 29.20% |
| 6,000,000 | 2048 | 441.50 | 0.00% |
| 12,000,000 | 64 | 46.25 | 91.23% |
| 12,000,000 | 128 | 87.00 | 82.23% |
| 12,000,000 | 256 | 168.75 | 64.19% |
| 12,000,000 | 2048 | 1311.38 | 0.00% |

The 128 MiB configuration used less memory and retained the sampled keys at
1.2 million entries, but lost 82.23% of sampled keys after 12 million inserts.
Even 256 MiB lost 64.19% there. These synthetic sample percentages are not
measured mainnet revalidation rates or full-IBD forecasts. A smaller budget
also need not allocate less at every prefix: its initial tier sizing and growth
boundaries differ.

Operating IBD continued on the same host, so raw component insertion timings
are affected by contention. No full-IBD speedup, completion-time prediction or
universal budget recommendation is established. Current operating options,
production cache policy and DB schema were left unchanged.

## Real-header safety and recomputation

Existing `header_pow_tests` were strengthened rather than adding a second
parallel test suite. Eviction is checked in both eager and growing backends.
A 6,000-header real-mainnet fixture was checked twice through the production
verifier with eight workers and scaled budgets that force eviction quickly.
Every cold run made 6,000 actual Yespower calls. Second-pass calls were:

| Scaled budget | Actual second-pass Yespower calls |
|---:|---:|
| 64 KiB | 5,832 |
| 128 KiB | 5,661 |
| 256 KiB | 5,049 |
| 2 MiB | 0 |

These small-budget runs demonstrate that evicted entries are recomputed;
they are not timing or hit-rate predictions for 64/128/256 MiB mainnet caches.
Invalid PoW and invalid target limits remain rejected. The selected eleven
cache-related tests passed 255 assertions, including existing cache-reset,
live-index restoration and concurrency coverage. Full sanitizer suites were
not rerun; no production storage change was adopted.

## Unadopted alternatives

Growing from the last tier rather than total allocated budget reduced an
initial allocation but made memory larger at a later input boundary. It was
rejected separately; changing growth formulas is not a guaranteed saving.

A private Linux-only prototype discarded fully zero-initialized interior pages
of a newly allocated, unpublished cache table. It never operated on existing
proof entries or the live node. At the second tier expansion, its immediate
helper RSS was about 56 MiB rather than about 152 MiB. After 1.2 million inputs,
both were back near 152 MiB. Component processing times increased in the two
prototype runs; host contention and page-fault costs were not separated.
This is initial physical allocation deferral, not total memory reduction.
No cross-platform or sanitizer validation was completed for that prototype,
so it was not integrated into the generic cache or production verifier.

## Decision

No production memory fix or automatic budget reduction was adopted. Small
budgets have substantial delayed-reuse costs, while page deferral only moves
physical allocation in time and still needs safety/performance validation.
The confirmed evidence and regression coverage are retained. Select a lower
budget only after deciding the acceptable recomputation cost, or investigate
initialization/page-fault behavior separately. The ongoing operating IBD and
its recorded baseline must not be interrupted for this experiment.

## Intermediate budgets and the approximately 288 MiB expansion

A separate isolated reproduction with the same component implementation
allocated a 301,694,976-byte new tier. Helper RSS rose from 151.75 to
441.50 MiB and remained there during deliberate 10- and 60-second idle waits.
Those waits are not a real-IBD interval or throughput measurement.

Intermediate budgets were run twice each in forward/reverse order. The
44.9-million-input runs extend beyond the point where growth becomes disabled.
The existing formula can stop growth before consuming the nominal entire
entry budget; configured budget, actual allocation and whole-node RSS differ.

| Entries inserted | Budget (MiB) | Helper RSS (MiB) | Sampled misses |
|---:|---:|---:|---:|
| 2,200,000 | 512 | 332.00 | 0.00% |
| 2,200,000 | 1024 | 223.75 | 0.00% |
| 2,200,000 | 2048 | 441.62 | 0.00% |
| 6,000,000 | 512 | 332.00 | 0.00% |
| 6,000,000 | 1024 | 658.50 | 0.00% |
| 6,000,000 | 2048 | 441.62 | 0.00% |
| 12,000,000 | 512 | 332.00 | 29.02% |
| 12,000,000 | 1024 | 658.38 | 0.00% |
| 12,000,000 | 2048 | 1311.12 | 0.00% |
| 44,900,000 | 512 | 332.00 | 80.71% |
| 44,900,000 | 1024 | 658.50 | 61.43% |
| 44,900,000 | 2048 | 1311.25 | 24.02% |

The 1024 MiB budget used less memory at 2.2 million inputs but more than the
2048 MiB baseline at six million, due to different growth boundaries. The
512 MiB budget retained the sampled keys at six million but lost about 81%
at 44.9 million. These synthetic percentages are not mainnet cache miss
rates or measured Yespower recomputation costs. No default/operating option
change or full-IBD performance claim follows from them.

The private Linux page prototype was also exercised at this larger expansion.
It reduced RSS to about 154 MiB after discarding fresh zero-filled pages, but
peak RSS was already about 441 MiB during initialization. At 2.2 million
inputs its RSS grew to about 344 MiB. Thus it did not demonstrate a lower
peak or OOM protection. It remains unadopted, with cross-platform, sanitizer
and actual-IBD performance validation incomplete. Production code and the
operating node were not changed in this comparison. Existing cache regression
results are unchanged; they were not rerun for this documentation-only update.

## Adopted paged storage for the growing proof cache

The growing backend now constructs initialized proof objects in 4 KiB pages
only when written. Unallocated pages read as zero, preserving the original
empty-slot semantics. The generic cache defaults to contiguous storage; eager
proof caches and other users retain that default. Slot counts, hashing, growth
thresholds, eviction and configured entry budgets are unchanged. Existing
cache locks protect page access. No platform-specific mapping or raw object
lifetime management is used.

In an isolated reproduction at a 2048 MiB budget, the approximately 864 MiB
tier expansion increased RSS by 870.00 MiB with contiguous storage and
8.25 MiB with paged storage (99.05% less immediate growth). Deliberate idle
waits did not change those readings. With 6.5 million inputs, repeated median
peak RSS decreased from 1311.38 to 851.50 MiB (35.07%). With 12 million and
44.9 million inputs, pages filled and final RSS increased by approximately
7–8 MiB (0.55–0.58%) due to storage metadata/allocation overhead. This defers
physical allocation; it is not a permanent 870 MiB whole-node saving.

Sampled misses matched the baseline at all three input counts. Synthetic
insertion time increased approximately 6.4% and 8.8% at the first two counts;
the last count did not show an increase. Concurrent operating IBD introduces
host-load uncertainty. Repeated 6,000-header production verification runs
showed no consistent cold regression; median warm reuse at a scaled 2 MiB
budget increased from 3.147 to 3.332 ms. These measurements do not establish
full-IBD speedup or OOM avoidance.

The existing header-PoW and CuckooCache suites passed 36 cases and 189,069
assertions. GUI/daemon builds and the existing fast-startup functional test
passed. An isolated paged-storage component passed ASan/UBSan and leak checks;
this is not a full production sanitizer run. TSan and Windows/macOS builds
were not rerun. The operating process and database were not restarted or
modified. Larger index/replay and database-write memory peaks remain separate.

This memory-management change is temporarily carried alongside Lazy PoW in
PR #11. Before merging, split Lazy PoW and memory/CPU management into two
separate PRs, retaining this change as an independent commit.
