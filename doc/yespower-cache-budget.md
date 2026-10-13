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
