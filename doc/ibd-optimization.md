# Core31 IBD optimization measurements

The final performance target is a complete fresh mainnet IBD in at most five hours,
with `-assumevalid=0`, unchanged minimum chainwork, and all PoW, difficulty,
contextual, script and commitment checks enabled. Component benchmarks and
extrapolations are not evidence that this target has been met.

The user will perform the final network IBD and judge that target. Development
completion uses repeatable offline/component/localhost validation, not repeated
multi-hour mainnet downloads. No full-IBD completion time is claimed here.

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
process-local cuckoo cache with a 16 MiB entry budget plus cache metadata. Entries
are salted SHA256 digests of the full SHA256d header identifier. No input field,
including nBits, is omitted. Invalid results are never inserted. The current
powLimit/target validity check runs before every lookup. Bitcoin-style and fuzz
checks retain their previous paths. The raw `GetPoWHash()` remains uncached.

The cache cannot be populated by peer-supplied status, an IBD flag, a checkpoint
or an on-disk TREE flag. Eviction/restart simply causes actual PoW computation
again. It does not change SugarShield, contextual validation, script validation,
PRESYNC/REDOWNLOAD, minimum chainwork, disk formats or durability.

The initial 1 MiB implementation measured A/B/B/A imports of the existing first
6000 mainnet blocks, each into a
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

A separate Debug `-O1 -g1` ASan/UBSan build of that initial cache passed all four suites
(78.08 seconds), with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. That sanitizer configuration disabled GUI/IPC;
the preceding normal production build included both. Existing baseline builds
and the user's running nodes were not modified.

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

## Cache capacity comparison

Read-only observation of the user's original node found eight simultaneous
PRESYNC peers at heights 1,110,000 through 1,194,000. A 1 MiB cache cannot cover
that spread. Separate 1 MiB / 16 MiB executables verified the first 40,000 linked
mainnet headers twice, in A/B/B/A order, with identical original compiler flags:

| Run | Capacity | First proof pass | Repeated pass |
| --- | ---: | ---: | ---: |
| A | 1 MiB | 117.297 s | 61.906 s |
| B | 16 MiB | 117.549 s | 0.068 s |
| B | 16 MiB | 118.433 s | 0.043 s |
| A | 1 MiB | 118.016 s | 61.139 s |

All proofs passed. This is about 1.52x for the two-pass workload, with no
first-proof acceleration. The default entry budget is consequently 16 MiB,
still bounded independently of peers or chain length. It does not retain an
entire mainnet history. The downloaded 100,000-header input file (of which this
experiment verified the first 40,000) has SHA256
`f64a65677abdfa525b67d9a09291868fbfd1f9fb0d4089cdfafc3b2718dd07dd`.

`-maxpowcache=<MiB>` now exposes a bounded startup entry budget of 1-2048 MiB,
with the 16 MiB default unchanged and two bits/entry of extra cache metadata.
This supports explicit experiments on longer peer/pass separation without
silently allocating gigabytes by default. The maximum also stays below the
existing CuckooCache uint32 epoch-arithmetic overflow boundary; a static assertion
guards that constraint. Cache initialization allocates a replacement before
swapping under the existing exclusive lock. It neither reads nor writes proof
evidence to disk, and eviction/reset requires a new real proof on the next miss.

After the parallel/mapping changes, the production benchmark's optional fourth
argument sets the cache budget. A/B/B/A with eight workers and 40,000 real headers
measured 1 MiB cold passes of 18.7343/18.6379 seconds and repeated passes of
9.78997/9.78405 seconds. At 16 MiB the corresponding times were
18.5900/18.5187 and 0.04363/0.04415 seconds. This confirms reuse, not reduced
first-proof work; it does not establish whole-mainnet retention or five-hour IBD.

All 51 targeted normal and ASan/UBSan tests passed. Two new ThreadSanitizer cases
passed (8.15 seconds), forcing eviction, reset during verification and rejection
of invalid headers and invalid sizes. The localhost P2P regression passed with
1 MiB, restart at 2 MiB, and startup rejection of 0, negative, oversized and
non-numeric budgets. No shared node or external peer participates in that test.

A larger offline validation subsequently passed all 1,000,000 linked mainnet
headers at the maximum 2048 MiB cache setting, then passed the repeated proof
checks in the same process. The fixture tip is
`09246c3d203e709775281602bc710622ccbd4455010a745b0bdb13bb2c0858b0`, and its SHA256 is
`76b8b06e5930b133c900d75ec838a5a9295d1ee5b7436084c8378a4103b37cce`.
The running executable SHA256 was
`8649236a35ca38f221514b0f5130b3d98dd1703a52af00740517398163b3de3c`.
Linkage, all expected difficulty targets, MTP and genuine initial PoW passed;
exit status was zero, maximum RSS 2,415,228 KiB, reported swaps zero. The timing
is not used as an A/B result because compilation and a brief component smoke
test overlapped this validation. No public-network IBD was performed by it.

## Existing peer-test timing assumption

The expanded test selection found five assertions failing in
`denialofservice_tests/stale_tip_peer_management`. The preserved, pre-optimization
bootstrap test binary reproduced the same five failures. With five-second
spacing, the test advanced mock time by only 16 seconds, but Core31's stale-tip
check is scheduled every ten minutes. The fixture now advances beyond both
thresholds; all five tests in `denialofservice_tests` pass. Production peer
timers, connection limits and eviction rules are unchanged.

## Existing subsidy-test assumptions

The wider block/chainstate/cache regression selection initially reported two
failures in `validation_tests`: a hardcoded 50-coin initial subsidy and a
14-million-block sum expected to exhaust Bitcoin's subsidy schedule. Both
failures were reproduced with the preserved pre-optimization bootstrap binary.
Official Sugarchain `64bc05ccc1dc2dcd4db9e86715c14dee12be6460`
(`src/validation.cpp`, `src/chainparams.cpp`) specifies 4,294,967,296 base units
and 12,500,000-block epochs. Production already implements those values.

The fixtures now assert those mainnet constants, test both sides of halving
boundaries, retain Bitcoin-style synthetic interval tests, and sum all 64 epochs
against the independent scheduled-reward total 107,374,182,387,500,000 base units.
No monetary production code changed. All 45 cases selected by
`validation_tests,validation_block_tests,validation_chainstatemanager_tests,validation_chainstate_tests,validation_flush_tests,blockmanager_tests,blockencodings_tests,txdownload_tests,peerman_tests,cuckoocache_tests`
then passed.

## Bounded parallel header PoW

`-parpow=1` preserves serial verification by default. Values are clamped to 1-8.
In parallel mode a single node-owned `HeaderPoWVerifier` reuses Core31's
`CCheckQueue`; peers do not create their own pools. The first header is checked
synchronously. Subsequent batches contain at most eight uncached proofs, and a
batch finishes before another is submitted. Cache hits still check current
target limits. Header continuity, SugarShield, chainwork, commitments and final
contextual validation remain in their original paths.

Parallel mode can compute up to seven additional proofs in the failing batch;
it never speculates across an entire message. The existing 2000-header message
bound is unchanged. Local computation failures propagate to the calling thread
instead of being mistaken for peer misbehavior. Thread-local Yespower scratch
storage now has a destructor, so destroying/recreating a pool releases it. The
algorithm, parameters, header serialization and 32-byte results are unchanged.

The test binary alone uses Linux linker wrapping to count actual Yespower calls,
measure maximum concurrency and inject a local library failure. Production has
no diagnostic counters or fault hooks. Regression tests cover cold first proofs,
warm reuse, invalid first/later proofs, concurrency bounds, simultaneous callers,
pool recreation, parameter changes and worker-error propagation. All 6000 real
mainnet header outputs match the previous `yespower_tls` API bit for bit.

Normal tests passed, including peer eviction after the separately documented
fixture correction. A separate ASan/UBSan run passed all seven selected suites
in 122.73 seconds, with leak detection and halt-on-error enabled. The localhost
`p2p_sugarchain_header_pow.py` test also passed: real 6000-header PRESYNC,
invalid-Yespower disconnection, 2001-header rejection, shutdown/restart and fresh
PRESYNC. It uses unchanged mainnet minimum chainwork, no external peers and no
wallet; accepted block/header heights correctly stay zero. Existing
`feature_shutdown.py` passed as well. A separate ThreadSanitizer build
(`Debug`, `-O1 -g1`, GUI/IPC/wallet disabled) passed all six `header_pow_tests`
in 370.52 seconds, including the full 6000-header old/new comparison, and all
18 cases selected by `checkqueue_tests,headers_sync_chainwork_tests,denialofservice_tests`.
Both runs used `TSAN_OPTIONS=halt_on_error=1`; no races were reported. Sanitizer
duration is not a performance measurement.

Build with `-DBUILD_BENCH=ON` and run a fresh process for each comparison:

```sh
build-ibd-optimization/bin/bench_header_pow src/test/data/sugarchain_headers.raw 6000 1
build-ibd-optimization/bin/bench_header_pow src/test/data/sugarchain_headers.raw 6000 8
```

For an A/B/B/A comparison of two saved binaries with the same worker count:

```sh
python3 contrib/bench/header-pow-ab.py \
  --baseline /path/to/baseline/bench_header_pow \
  --candidate /path/to/candidate/bench_header_pow \
  --headers src/test/data/sugarchain_headers.raw --count 6000 --workers 8 \
  --work-dir /path/to/new-results-directory
```

The runner records input/executable SHA256, commands, exit statuses, child CPU
time and cold/repeated measurements separately. It refuses to overwrite an
existing result directory or continue after failed validation, inconsistent
measurements or changes to the input/binaries. Synthetic runner checks covered
malformed/NaN/zero measurements, incorrect worker counts, duplicate/missing
passes, failed child processes and preservation of existing output. Synthetic
runner checks are not performance evidence.

This tool checks continuity, difficulty and MTP outside the timer, then measures
2000-header verification messages, with explicitly separate cold/repeated passes.
Parsing, contextual prechecks and worker construction are excluded; lazy scratch
allocation is included. A repeated pass may recompute proofs if its input exceeds
cache capacity. It is not a full IBD benchmark.

| A/B/B/A | Workers | Cold 6000 proofs | Repeated pass |
| --- | ---: | ---: | ---: |
| A | 1 | 17.6894 s | 0.00655 s |
| B | 8 | 3.19008 s | 0.00667 s |
| B | 8 | 3.26878 s | 0.00660 s |
| A | 1 | 17.6865 s | 0.00656 s |

The cold verification ratio is about 5.48x. This does not establish full IBD
within five hours. No minimum-chainwork, AssumeValid or checkpoint shortcut was
used to obtain these figures.

## Optional Linux huge-page mapping advice

Yespower scratch allocations now request `MADV_HUGEPAGE` where Linux exposes it.
This is mapping-local advice, not a system setting, reservation or requirement:
failed/unsupported advice leaves ordinary pages usable. Allocation sizes, free
paths, algorithm and portable compiler flags are unchanged. The change adds
four guarded lines to the existing allocator.

With the existing host policy (`madvise`), a separate production-path A/B/B/A
comparison verified 40,000 real headers with eight workers per fresh process:

| Run | Mapping advice | Cold proofs | Repeated pass |
| --- | --- | ---: | ---: |
| A | unchanged | 20.2418 s | 0.04378 s |
| B | huge-page advice | 18.8278 s | 0.04609 s |
| B | huge-page advice | 18.1894 s | 0.04459 s |
| A | unchanged | 20.3863 s | 0.04418 s |

The cold-pass ratio is 1.098x; combined child CPU time per run fell from
151.71/152.17 to 137.09/134.55 seconds. `/proc` reported 63,488 KiB of anonymous
huge pages, 96,824 KiB RSS and zero swap for a candidate process. All 49 cases in
the ASan/UBSan selection (`header_pow_tests,sugarshield_tests,headers_sync_chainwork_tests,pow_tests,checkqueue_tests,denialofservice_tests`)
passed with leak detection and halt-on-error enabled. This is a local cold-proof
measurement, not a full IBD result or a guarantee on other memory/page policies.

## Header-first offline block validation diagnostic

`contrib/bench/warm-block-import.py` uses the functional framework's isolated
mainnet node and localhost peer, supplies genuine header proofs first, then
submits the corresponding real blocks through RPC. It preserves the production
minimum chainwork: the fixture must still be in PRESYNC with accepted height zero
before explicit block submission. This measures proof/validation reuse, **not**
public-network IBD or download scheduling. The fixture must begin at block 1 and
contain a multiple of 2000 headers. The caller supplies expected tip and UTXO hash.

```sh
BITCOIND="$PWD/build-ibd-optimization/bin/sugarchaind" \
  python3 contrib/bench/warm-block-import.py \
  --configfile=build-ibd-optimization/test/config.ini \
  --tmpdir=/path/to/new-benchmark-directory --nocleanup \
  --blocks=/path/to/6000-blocks.dat --workers=8 \
  --expected-tip=e7a04205f70e5b6e99d83a8f720748fee559a382701b39ff9891e391e6cf81d9 \
  --expected-utxo=94fda3c59b6d6410687bfacd26d858d0f85b86f6913b90016b7b02f72b3f13b8
```

One diagnostic run of each variant gave:

| Variant | First 6000 headers | RPC block submission/validation | Normal shutdown |
| --- | ---: | ---: | ---: |
| Original bootstrap | 17.8674 s | 39.5962 s | 0.1511 s |
| Cache/parallel/mapping candidate | 2.9901 s | 2.4906 s | 0.1509 s |

Both returned the exact expected tip and UTXO hash, passed `verifychain 4 6000`,
shut down, restarted with no peers, and passed the same checks again. The tool's
RPC timeout was increased after an initial baseline attempt exceeded the
framework's 30-second RPC timeout during uncached verifychain. That interrupted
attempt is not a successful measurement. Verification/restart/startup times are
outside the reported phases; these single-run figures are diagnostic rather than
a replacement for the A/B/B/A comparison or an extrapolated full IBD duration.

## Isolated header-sync component benchmark

`bench_headers_sync RAW_HEADERS COUNT ROUNDS` validates the input's linkage,
difficulty, MTP and genuine PoW before timing repeated PRESYNC/REDOWNLOAD passes.
For this isolated state-machine fixture only, its total chainwork is the transition
threshold, as in the unit tests. No node runs and no production minimum work is
changed. Every round checks state transitions and all returned header hashes.
It measures header-sync computation alone; it must not be reported as network
IBD throughput or proof-verification throughput.

```sh
cmake --build build-ibd-optimization --target bench_headers_sync
build-ibd-optimization/bin/bench_headers_sync src/test/data/sugarchain_headers.raw 6000 100
```

### Rolling target sum, unchanged SugarShield arithmetic

The header-sync state keeps one additional 256-bit sum of its newest 510 targets.
It still retains the same 521 indices and computes both endpoint MTPs normally.
Initialization/reset sums the available newest targets; after a valid append it
subtracts the outgoing target and adds the new target. Invalid difficulty is
rejected before updating the sum. Short-history behavior is unchanged. No header,
commitment, work-threshold, contextual or PoW check is removed.

The final damping/clamping/division/multiplication/compact-target calculation is
shared with the full-history `GetNextWorkRequired()` implementation, preserving
the existing integer operation order. Normal tests passed all 52 selected cases;
ASan/UBSan passed the 29 SugarShield/header-sync/PoW cases. The new irregular-time
fixture compares full-history targets against the real rolling state machine at
starts 0, 1, 509, 510, 511, 520, 521, 522 and 2000, across resets and thousands of
evictions. The localhost P2P/body-mutation/restart regression and production build
also passed.

An isolated A/B/B/A comparison (6000 real headers, 100 two-pass rounds per
process; initial genuine PoW outside the timer) measured:

| Run | Target summation | State-machine seconds |
| --- | --- | ---: |
| A | full history per header | 19.2403 |
| B | rolling sum | 12.0062 |
| B | rolling sum | 11.8460 |
| A | full history per header | 19.2087 |

The component ratio is 1.612x. This is not a full IBD acceleration factor.

Run the localhost mainnet functional regression using the framework's existing
binary override (the test itself selects its temporary config explicitly):

```sh
BITCOIND="$PWD/build-ibd-optimization/bin/sugarchaind" \
  python3 test/functional/p2p_sugarchain_header_pow.py \
  --configfile=build-ibd-optimization/test/config.ini
```

### Exact small-divisor SugarShield arithmetic

SugarShield's two ordered divisions now use unsigned base-2^32 long division
when the divisor fits in 32 bits. Eight limb steps replace the generic bitwise
256-bit division. The remainder is strictly smaller than the divisor, so the
next 64-bit intermediate cannot overflow. Larger/nonpositive divisors retain
the generic operation. No division is combined or reordered, and damping,
clamping, multiplication, powLimit and compact conversion are unchanged.

The independent test oracle retains the old generic arithmetic. It compares
86,004 final consensus targets across random 256-bit inputs, every power-of-two
boundary, quotient boundaries, clamp endpoints, both powLimit settings, and
32-bit/fallback divisors. All 48 selected normal tests passed (420,299
assertions); ASan/UBSan passed 30 cases (405,735 assertions). The 6000-header
mainnet fixture, 521-history/reset/sliding regressions, localhost P2P invalid
PoW/body-mutation/restart test, and GUI/IPC/multiprocess production build passed.

A/B/B/A against the immediately preceding rolling-sum implementation, using the
same 6000-header/100-round component benchmark, measured 11.8674, 1.69143,
1.69769, 12.0110 seconds respectively: 7.046x for the state-machine component.
Initial genuine Yespower checks remain outside this timer. This is not a full
IBD measurement or a claim of meeting the five-hour target.

### Read-only live observation

`contrib/bench/observe-ibd.py` records JSONL snapshots of chain progress, peer
presync/in-flight counts and byte totals, plus Linux process CPU ticks, RSS,
major faults and I/O counters. It calls only read-only RPCs, never changes or
stops a node, uses CLI cookie authentication, and omits peer addresses. Missing
progress and RPC failures remain explicit rather than becoming false zeros.
It refuses to overwrite output and detects PID reuse. Clock ticks/page size
are recorded so analysis can use actual elapsed time and process counter deltas.

```sh
python3 contrib/bench/test-observe-ibd.py
python3 contrib/bench/observe-ibd.py --cli /path/to/sugarchain-cli \
  --datadir /path/to/fresh/run/data --rpcport 38421 --pid NODE_PID \
  --output /path/to/new-observations.jsonl --interval 10
```

Four deterministic parser/RPC-error tests passed, followed by a two-sample
read-only smoke check against the fresh network run. The full network run uses
an immutable copy of the binary from source `775ef0b94e`, `-parpow=8`,
`-maxpowcache=2048`, `-dbcache=4096`, `-assumevalid=0`, unchanged minimum chainwork,
and a new empty datadir. Startup and initial peer/PRESYNC progress are confirmed;
completion time, block-stage throughput and full-IBD success remain unverified.

The user subsequently deferred all public-network full-IBD/block-download
measurement to their final validation. The development observation node was
stopped normally via its cookie-authenticated RPC, with `Shutdown done`, still
at blocks=0 and about 392,000 presynced headers. Its data/logs were preserved;
no user node was stopped. Further development uses fixtures, offline imports,
components and localhost tests only. No completion-time claim follows from this
short observation. The observer now also terminates on an exited/zombie process
without waiting for the parent to reap its PID; five tests include this real
child-process lifecycle case and verify that no RPC is attempted after exit.

### Localhost block-download latency proxy

`contrib/bench/block-download.py` uses the existing functional-test P2P framework,
real regtest block validation and disk ingestion, four outbound localhost peers,
and deterministic coinbase-only blocks. Every getdata response receives a fixed
async delay; the network event loop itself is never slept. The harness checks
peer/request bounds, exact tip, UTXO fingerprint and `verifychain(4, count)`.
Node CPU and Python harness CPU are recorded separately to expose generator
bottlenecks. The result file cannot be overwritten.

Initial unchanged-code sensitivity runs over 4096 blocks measured 5.843s at
0ms delay and 9.518s at 100ms, both with the existing 16-request per-peer bound,
identical tip and UTXO results. A 1024-block smoke check also passed. These are
latency/scheduler/ingestion proxies, not mainnet PoW, presync, complex late-chain
scripts, Internet bandwidth or full IBD measurements. Pair them with the real
header/PoW and offline mainnet-block benchmarks rather than extrapolating a
full-chain completion time from these small synthetic blocks.

```sh
BITCOIND="$PWD/build-ibd-optimization/bin/sugarchaind" \
  python3 contrib/bench/block-download.py \
  --configfile=build-ibd-optimization/test/config.ini \
  --blocks=4096 --latency-ms=100 --result=/tmp/new-download-result.json
```

### Opt-in bounded IBD request budget

`-maxibdblocksinflight=16..128` adjusts only bulk IBD requests. Default remains
16; non-IBD/direct-fetch/compact-block limits remain 16, and the existing
1024-block lookahead, stall detection/backoff, service eligibility, minimum work
and validation gates are unchanged. Values outside the range clamp to its
endpoints, including the full signed-64-bit input boundaries. Increasing this
option explicitly allows more outstanding requests per peer; it does not
increase wire-message size, receive-buffer or lookahead limits, nor remove any
block validation. Do not conflate this bounded budget with PR 225's 122880-block
scheduling horizon or install such a number as a per-peer cap.

The first proxy used regtest's per-block exhaustive index diagnostics and the
framework's trace logging. That inflated CPU cost (6.6–6.9 CPU seconds/4096
blocks), unlike normal mainnet defaults. The performance harness now explicitly
uses mainnet's diagnostic defaults (`-debug=0 -checkblockindex=0`) for **both**
variants, while dedicated correctness/stalling tests keep exhaustive index
checks enabled. Consensus, scripts and `verifychain` are not disabled.

A/B/B/A with 4096 deterministic blocks, four peers and 100ms response delay:

| Run | Per-peer IBD cap | Seconds | Node CPU seconds | Python CPU seconds |
| --- | ---: | ---: | ---: | ---: |
| A | 16 | 6.612860 | 0.90 | 0.811 |
| B | 128 | 0.982984 | 0.76 | 0.583 |
| B | 128 | 0.967126 | 0.66 | 0.561 |
| A | 16 | 6.612844 | 0.83 | 0.845 |

This latency-limited proxy improved 6.782x with identical tip/UTXO/verifychain
results. It does not predict full mainnet speed or late-chain script/disk cost.

Validation: seven peer/DoS unit cases passed normally and under ASan/UBSan;
upstream's 1024-window stall/eviction/backoff/recovery scenarios passed with 128
under both v1 and v2, and again with 16. A near-tip transition confirmed that
subsequent request batches return to <=16 while previously issued requests
drain. An eight-peer stress run with configured value 1000000 remained <=128
per peer. A 128-block, 900000-byte-padding-per-block stress test transferred
115220865 bytes with exhaustive index checks enabled and passed verifychain.
These stress runs are correctness checks, not performance comparisons. The
GUI/IPC/multiprocess production build passed.

### Bounded receive batching before send-side processing

The message handler processes at most 64 available messages, stopping at a
one-millisecond steady-clock deadline between messages, before servicing sends.
This adapts PR 225's batching idea with a time/fairness bound. Slow individual
messages are not preempted, exactly as before, but a single slow message is
followed immediately by send-side work. Interrupt, disconnect and send-buffer
backpressure terminate the batch immediately. Peer order remains randomized;
all message parsing, proof and contextual checks are unchanged.

A/B/B/A at 10000 deterministic regtest blocks, four localhost peers, zero added
latency and the same explicit 128-request budget for both binaries measured:

| Run | Receive handling | Seconds | Node CPU seconds | Harness CPU seconds |
| --- | --- | ---: | ---: | ---: |
| A | one message | 1.384944 | 1.76 | 1.342 |
| B | bounded batch | 0.931611 | 1.08 | 0.880 |
| B | bounded batch | 0.936430 | 1.14 | 0.864 |
| A | one message | 1.349077 | 1.68 | 1.309 |

The localhost proxy ratio is 1.464x with matching tip/UTXO/verifychain. Python
peer overhead is visible and also decreases with fewer request messages, so
this is not an end-to-end mainnet prediction. Reduced node CPU independently
supports the send-side overhead explanation.

Tests cover work/time bounds, two busy peers, a slow callback, send backpressure,
disconnect, empty queues and immediate shutdown. Normal network/peer/DoS tests
passed all 25 cases (152236 assertions), and TSan passed all 25. The localhost
v2 stall/backoff/recovery and real Sugarchain invalid-PoW/body/restart regressions
passed, as did the complete production GUI/IPC/multiprocess build.

**Existing sanitizer issue:** the full ASan/UBSan network suite reports
`streams.cpp:99` passing a null pointer to zero-length `fwrite` from
`CaptureMessageToFile`, in `net_tests/initial_advertise_from_version_message`.
That test directly calls `ProcessMessagesOnce`, not the changed message loop.
It reproduces identically in the preserved pre-optimization sanitizer binary
SHA256 `34d780347b1937360d0fa5fc3729789d07b322ef688725559d60cb87d59af52d`.
No source workaround or test expectation change was made. All other 24 selected
network/peer/DoS cases passed ASan/UBSan (150395 assertions). Thus the full
sanitizer suite is not claimed clean; this unrelated capture-path issue remains.

## Final integration and measurement limits

A separate clean `build-ibd-optimization-final` build completed with GUI, IPC,
multiprocess, daemon, CLI and benchmarks enabled, using Cap'n Proto from
`/usr/local`. Actual yespower compilation remains portable `-O2` with SSE2;
no `-march=native`, forced AVX instruction set, or system policy change was made.

The final clean binaries passed 78 selected C++ cases (571488 assertions):
SugarShield, header PoW, low-work header sync, PoW, checkqueue, networking,
peer management, DoS and validation. Existing functional ping, malformed/flooded
messages, v1/v2 network deadlock, initial headers, minimum-chainwork, compact
blocks/blocksonly and shutdown tests passed all eight scenarios. The three
IBD request-limit/stall scenarios also passed. The new real-header/body test
exposed a test-only fixture lookup error when invoked through CMake's build
symlink; resolving the source path fixes that invocation without changing
production or validation expectations. Its build-directory rerun passed,
including mutated-body rejection, genuine block acceptance and worker restart.

Final offline integration imported all 6000 real mainnet blocks, obtained the
documented exact tip and UTXO digest, passed `verifychain(4, 6000)`, shut down,
restarted without network peers, and passed those checks again. This run
overlapped regression tests and is correctness evidence, not a new performance
comparison. Final daemon SHA256:
`33a94a1d58ceb56eddd16b95ef5478ab90460bacf4df4d6d9cbb417c8265c476`.

Two further experiments were not adopted:

- An ordered asynchronous proof-window prototype improved a short 40000-header
  A/B/B/A proxy by only 1.031x while adding queue/lifetime complexity. This was
  insufficient evidence of a repeatable benefit to retain production changes.
- Eight physical-core affinity measurements varied substantially: unrestricted
  22.3435/21.9948 seconds versus pinned 23.0636/19.8457 seconds. No reliable
  affinity benefit was established and no CPU affinity policy was installed.

No speculative larger scheduling window, adaptive peer scheduler, stall-timeout
override, block-index cache increase or tip-log suppression was retained. The
available short fixtures do not establish those as current bottlenecks.

For the user's final run on Nana, an explicit candidate configuration is
`-parpow=8 -maxpowcache=2048 -maxibdblocksinflight=128 -dbcache=4096 -assumevalid=0`
with a new datadir. These are opt-in resource budgets, not claims of optimal
settings for every host. Defaults remain conservative; full-chain memory growth
and long-run cache retention need measurement. Existing datadirs are not reused.

The proxies deliberately complement one another: real headers exercise genuine
Yespower and contextual difficulty; header-sync benchmarks cover both commitment
phases; real block import checks state/durability/restart; localhost experiments
isolate latency, request bounds and message processing. They do not reproduce
Internet peer heterogeneity, the entire historical transaction/script mix,
44-million-entry index growth, long-run disk pressure or full-chain shutdown.
Their speedup ratios cannot be multiplied into a full-IBD prediction. PR 225's
10h49m15s result also uses a different validation/trust policy. Final full IBD
time, improvement ratio against a full-run baseline, and the five-hour target
remain unmeasured and belong to the user's final validation.
