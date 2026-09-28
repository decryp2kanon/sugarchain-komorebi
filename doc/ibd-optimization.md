# Core31 IBD optimization measurements

The acceptance target is a complete fresh mainnet IBD in at most five hours,
with `-assumevalid=0`, unchanged minimum chainwork, and all PoW, difficulty,
contextual, script and commitment checks enabled. Component benchmarks and
extrapolations are not evidence that this target has been met.

## Reference implementation audit

Reference: [Sugarchain PR 225](https://github.com/sugarchain-project/sugarchain/pull/225),
base `a131e9c5e404200419608e86f361326447328fd9`,
head `17fa7590fc83f3c200e75ef16604bb7ceb9cd40c`.
The PR's reported 10h49m15s full sync uses a different trust model: historical
Yespower is replaced by checkpoint authentication. It is not a directly
comparable full-verification baseline for this work. Its historical sampling
harness also installs default assume-valid context. Neither shortcut is eligible
for the acceptance run here.

Classification: A = directly applicable idea; B = needs Core31 implementation;
C = already handled upstream; D = unnecessary here; E = incompatible with this
work's security requirements; F = retain only after measurement.

| Reference commit | Change | Classification / Core31 disposition |
| --- | --- | --- |
| ac9b033219 | Boost bind includes | D: old build compatibility; no corresponding build failure |
| ec8dc24213 | Checkpoint headers and persisted PoW evidence | E for replacing historical PoW; B/F for reuse of actually verified identical headers |
| bda8c9ae62 | Parallel Yespower | B/F: bounded work, lifecycle and early-invalid-input resource costs need independent tests; no checkpoint exemptions |
| 85be82de78 | Adaptive block requests | B/F: measure Core31 scheduling first; distinguish scheduling horizon, per-peer count and memory exposure |
| bbd002afe2 | 64-message receive batching | B/F: Core31 still invokes sending after each receive; bound fairness and shutdown latency, measure send cost first |
| 0f39b7a9f8 | Index cache and rewind writes | C/D for removed RewindBlockIndex path; A/F for cache sizing (Core31 still caps block-tree cache at 2 MiB) |
| f69af7ff96 | Suppress IBD tip logs | A/F: Core31 still logs every tip; preserve observability and measure benefit before changing |
| 643011d767 | Security regression tests | B: reuse invalid-PoW, mutated-header, disk-ingress and ordered-failure scenarios; checkpoint-specific expectations do not apply |
| a5d5c30624 | Checkpoint Qt progress | D: retain existing Core31 presync progress UI |
| ebc27426ec | Checkpoint Qt tests | D: no checkpoint UI is being introduced |
| 54257a1638 | Historical sampling harness | B: useful stratification/fixture isolation; old globals and default assume-valid setup cannot be copied |
| ea4b854153 | Yespower throughput tool | A/B: useful worker-count sweep; link current implementation and distinguish raw throughput from IBD |
| 03fdee3f65 | Full IBD estimator | B: estimates may guide experiments, never satisfy full-run acceptance |
| 84e82234d0 | Estimator tests | B: relevant only if estimator is adapted |
| 0e507898fe | Security/benchmark documentation | B: preserve lessons about unchecked first headers, trust provenance and bounded queues, not the checkpoint trust model |
| 17fa7590fc | README benchmark link | D: no change to the project's intentionally minimal README |

Additional differences:

- Core31 already increases its two-second block-stalling timeout up to 64 seconds
  and decays it after progress. Blindly changing it to 20 seconds ignores this.
- Core31's PRESYNC/REDOWNLOAD commitments remain in place. PR 225's checkpoint
  quarantine, replay snapshots and four-hour deadline are not replacements.
- A 122880-block scheduling horizon is not permission to accept that many
  in-flight blocks from each peer. Core31 currently caps this at 16.
- PR 225's parallel precomputation first validates a new leading header; this
  mitigates, but does not eliminate, speculative work on an invalid batch.
  Any Core31 implementation needs an explicit bound and adversarial tests.
- Persistent TREE validity must not be interpreted as proof of a Yespower check
  made by arbitrary older software. Cache entries must come from actual successful
  verification and bind all header bytes and the applicable PoW rules.

## Initial component measurements

Baseline source: merge commit `435d30e536` (before optimization).
Ryzen 9 5950X, existing portable RelWithDebInfo build (`-O2`), existing user
applications/nodes left running. Three runs over the committed 6000-header
mainnet fixture, without network traffic or data-directory access:

| Component | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Yespower validation, 6000 headers | 17.5123 s | 17.6291 s | 17.6111 s |
| SugarShield full-history checks, 6000 headers | 0.08472 s | 0.08505 s | 0.08645 s |
| PRESYNC + REDOWNLOAD, 12000 steps, excluding caller PoW | 0.28477 s | 0.18437 s | 0.18655 s |

These isolate the cost of existing code; they do not measure network scheduling,
late-chain scripts, permanent-index growth, disk durability or full IBD. The
first candidate to measure is bounded parallel Yespower, not a change to the
SugarShield formula. No five-hour result has been established.

## Reproducing the raw PoW worker sweep

After building the current crypto library, from the repository root:

```sh
c++ -O2 -std=c++20 -pthread -Isrc contrib/bench/yespower-throughput.cpp \
  build-ibd-optimization/lib/libbitcoin_crypto.a -o /tmp/yespower-throughput
/tmp/yespower-throughput src/test/data/sugarchain_headers.raw
```

An optional final argument caps the sweep at 1 through 16 workers. Each setting
hashes all 6000 actual headers with the unchanged YespowerSugar parameters.
It compares every 32-byte result with the single-worker result, then prints the
SHA256 digest of their ordered concatenation for cross-build comparisons.
Allocation, thread creation, computation and joining are timed; comparison and
reporting are outside the timer. No cached PoW results are used.

This diagnostic does not implement a production worker queue, validate the
rest of a block, measure adversarial-input costs, or establish a safe production
worker count. Run it without concurrent builds/benchmarks and record other load.
Never treat its rate as the end-to-end IBD rate.

Initial sweep after the separate GUI/IPC/multiprocess build completed:

| Workers | Raw hashes/s | All 6000 hashes identical |
| --- | ---: | --- |
| 1 | 328.73 | yes |
| 2 | 669.29 | yes |
| 4 | 1316.09 | yes |
| 8 | 2167.42 | yes |
| 16 | 1364.44 | yes |

The ordered result digest was
`6b35b9c11c78d6ad6a96e4dac04c06a8f56f205881dce8be7d5a3caee4e14bce`
in every case. More threads were not always faster. These are short initial
measurements with the user's applications left running, not a production change
or a full-sync result. The benchmark also rejected missing/truncated input and
invalid worker limits. The clean baseline build included daemon, CLI, Qt, IPC,
multiprocess, unit tests and benchmarks with GUI ON, IPC ON and RelWithDebInfo.

## Bounded reuse of verified Yespower

`CheckBlockProofOfWork` now caches successful YespowerSugar verification in a
process-local cuckoo cache with a 1 MiB entry budget plus cache metadata. Entries
are salted SHA256 digests of the full SHA256d header identifier. No input field,
including nBits, is omitted. Invalid results are never inserted. The current
powLimit/target validity check runs before every lookup. Bitcoin-style and fuzz
checks retain their previous paths. The raw `GetPoWHash()` remains uncached.

The cache cannot be populated by peer-supplied status, an IBD flag, a checkpoint
or an on-disk TREE flag. Eviction/restart simply causes actual PoW computation
again. It does not change SugarShield, contextual validation, script validation,
PRESYNC/REDOWNLOAD, minimum chainwork, disk formats or durability.

Measured A/B/B/A imports of the existing first 6000 mainnet blocks, each into a
new empty datadir, networking and wallets disabled, `-assumevalid=0`, dbcache
1024 MiB, including orderly shutdown and flush:

| Run | Original PoW checks | Cached PoW checks |
| --- | ---: | ---: |
| First | 71.879 s | 18.446 s |
| Second | 71.483 s | 18.445 s |

The mean ratio is 3.886x **for this small offline workload only**. All runs
reached height 6000, tip
`e7a04205f70e5b6e99d83a8f720748fee559a382701b39ff9891e391e6cf81d9`.
The candidate was restarted offline and `verifychain 4 6000` returned true,
with zero peers and no validation errors. UTXO `hash_serialized_3` was
`94fda3c59b6d6410687bfacd26d858d0f85b86f6913b90016b7b02f72b3f13b8`.

The fixture fits the cache; a tens-of-millions-of-headers separation between
checks does not. This measurement must not be extrapolated to full IBD.
First-time Yespower throughput is unchanged. The baseline executable was linked
from the same clean build with the exact original committed pow.cpp object and
identical compiler flags; the candidate differs in this production TU only.

Validation: full GUI/IPC/multiprocess rebuild; `sugarshield_tests`,
`headers_sync_chainwork_tests`, `pow_tests`, and `checkqueue_tests` all passed.
Added regression coverage for changes to every serialized header field, invalid
compact targets, a stricter powLimit, SHA256d-vs-Yespower separation and concurrent
cache readers. This is component-level validation, not full mainnet completion.

### Reproducing the offline import comparison

Keep separate baseline and candidate executables, and supply an existing raw
block file (not a live node's datadir). The output directory must not exist:

```sh
python3 contrib/bench/offline-ibd.py \
  --baseline /path/to/baseline/sugarchaind \
  --candidate /path/to/candidate/sugarchaind \
  --blocks /path/to/mainnet-blocks.dat --height 6000 \
  --tip e7a04205f70e5b6e99d83a8f720748fee559a382701b39ff9891e391e6cf81d9 \
  --work-dir /path/to/new-comparison-directory
```

The tool retains four fresh datadirs, commands, binary/input hashes, logs, wall
and child CPU times, and expected-tip checks. Its active-process record identifies
only the child it owns. It does not connect peers or alter existing node data.
Repeat validation of this tool gave A/B/B/A times of 71.689 / 18.645 / 19.446 /
71.779 seconds, all at the expected tip with successful shutdown. The raw block
file SHA256 was
`39ba457e491589267dbc4c3d916aa863a4112fdf99b6d4b86ddaf0ede23769ed`.

### Additional profiling, not production changes

An isolated gprof build attributed 75.50% of sampled time to
`blockmix_xor_1_0` and 24.10% to `blockmix_xor_save_1_0`; SHA256 Transform was
0.06%. Temporary AVX and O3 builds did not establish a meaningful improvement
and were not adopted. No release compiler flags were changed.

A temporary Linux MADV_HUGEPAGE experiment, with no system policy changes,
gave eight-worker A/B/B/A raw rates of 2204.87 / 2484.80 / 2384.10 / 2240.18
hashes/s on CPUs 0-3,8-11. All 6000 output hashes matched. Single-worker throughput
was unchanged, so this has not been added to the serial production path. These
are diagnostic experiments, not full IBD results or a production parallel queue.
