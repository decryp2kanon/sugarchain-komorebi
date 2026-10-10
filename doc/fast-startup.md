# Startup mode boundaries

`-fast-startup=0` is the default. It uses the startup algorithms from before
PR #10 (`796902953802cbab9d3c2201ab8eef8bc1f7cf5a`), including the existing
`-parpow` historical PoW checks. `-fast-startup=1` opts into both historical
disk-index trust and the subsequent PR #10 performance optimizations.

| Stage | Default / `=0` | `=1` |
|---|---|---|
| Disk iterator | Copying key/value streams, decode the key | Inspect key prefix, directly deserialize unobfuscated values |
| Count and capacity | One loading pass, incremental map growth | Key-only pre-count, reserve spare buckets and pointer capacity |
| Index storage | Original `std::unordered_map` type, default allocator and cached hash policy | Stable original hash/index pairs in chunks, five-byte compact lookup slots |
| Pointer collection | Collect from the map after loading | Collect newly inserted entries during loading; retain pre-existing entries on reload |
| Height ordering | `std::sort` | Integer sort with temporary compact height/pointer cache |
| Work reconstruction | Original 256-bit formula | Equivalent exact 64-bit formula for eligible compact targets |
| Index linking | Original traversal | Prefetch upcoming index fields |
| Referenced block files | Separate complete map traversal | Collect during linking |
| Best-header selection | Collect and sort a second time | Reuse this load's sorted vector and prefetch fields |
| Chainstate candidates | Temporary pointer vector | Iterate the map directly |
| Active-chain construction / sequence IDs / witness checks | Original traversals | Read-ahead hints; the same checks and traversal results |
| Unknown-versionbits warning counts | Scan historical periods separately for each bit | Share counts across bits for each period during a non-IBD tip update |
| Verified Yespower cache | Allocate the configured entry budget at startup | Allocate a small positive-proof cache first and grow in tiers within the same `-maxpowcache` budget as verified proofs accumulate |

The common DB iterator, work calculation and chain-array helpers default to
their original behavior. Only explicit fast-startup calls select their
optimized paths; ordinary live-chain callers do not silently opt in.
The flag belongs to each block manager rather than process-global mutable state.

## Progress is independent of the algorithms

The pre-existing processed-count, rate and elapsed-time display remains in the
default mode (every 2,000 entries and the final partial batch). It does not need
an extra full DB scan to determine a total. Fast mode retains the count,
percentage and ETA display. Both modes retain console/Qt notifications and
the improved post-load stage displays; the legacy mode additionally displays
its second preparation and sorting stages. Sorting never reports invented
percentage progress.

## Validation and recovery

In fast mode, eligible persisted non-genesis entries may skip repeated
Yespower computation. Persisted status is not cryptographic proof: this mode
reduces detection of historical block-index corruption or tampering. Invalid
targets, ineligible entries and genesis still follow the existing verification
path. Disk trust is not promoted into process-local live-PoW evidence. New
network and imported headers retain their existing validation.

Both modes keep DB decoding errors, non-contiguous-index checks, missing block
file detection (including side/invalid branches), chainwork reconstruction,
snapshot handling, witness checks and shutdown interrupts. The optimized work
formula is tested against the original formula; it does not approximate work.

`-fast-startup=1 -reindex=1` is rejected. A GUI recovery retry that enables
reindex constructs the block manager with fast startup disabled.
`-reindex-chainstate` can use either mode because it retains the index DB.

## Fast-mode memory tradeoff

The fast-mode Yespower cache allocates at most 16 MiB for individual entries
initially, plus the existing batch cache. It grows in bounded tiers only after
verified entries fill the active tier. Older tiers retain their successful
proofs; cache misses still run full Yespower verification. The configured
`-maxpowcache` value remains the upper entry-memory budget, including batch
entries. Default mode keeps eager allocation and its previous cache behavior.

An isolated 44,806,391-entry mainnet snapshot with `-fast-startup=1`,
`-dbcache=4096` and `-maxpowcache=2048` measured three startups per version.
The prior 3x-reserve version started in 64.507 / 64.222 / 63.827 seconds
(median 64.222) with median peak RSS 20,487,880 KiB. The tiered-cache
candidate started in 62.796 / 63.286 / 63.950 seconds (median 63.286) with
median peak RSS 18,391,680 KiB. This is 2,047 MiB (10.23%) less peak RSS
and 0.935 seconds (1.46%) faster in these runs. The snapshot, build options,
verification settings and observed best block/chainwork matched. These are
offline local results, not a measured low-memory VPS guarantee.

The next isolated trial pooled fast-mode block-index nodes without changing
the map lookup algorithm or the default-mode allocator. Against the tiered
cache baseline above, three runs of the final implementation measured
61.802 / 62.646 / 62.178 seconds (median 62.178) and median peak RSS
18,048,920 KiB. That is another 335 MiB (1.86%) less peak RSS and 1.108
seconds (1.75%) less startup time in the same local snapshot. The node
addresses remained stable, and best block, chainwork, verification and clean
shutdown matched the baseline. Results on a low-memory VPS remain unmeasured.

A further trial marked the fixed-size `BlockHasher` read as non-throwing. On
libstdc++ this avoids storing a redundant cached hash in every block-index
map node. The three runs measured 63.743 / 62.010 / 61.535 seconds (median
62.010) and median peak RSS 17,695,896 KiB. Compared with the pooled-node
baseline, peak RSS fell another 345 MiB (1.96%) while startup time was within
run-to-run variation (median 0.168 seconds faster). This declaration also
changed the container's storage layout in default mode, but not its hash
value, lookup algorithm, or validation. This was a memory-policy leak across
the intended mode boundary. The later mode-isolation change restores the
original throwing signature and cached-hash policy for the legacy map; fast
mode's separate compact table does not use `BlockHasher`. The isolated
snapshot's best block, chainwork, and clean shutdown still matched. It does
not by itself make a 4–8 GiB VPS viable; that requires a larger structural
reduction and a constrained-memory test.


The next fast-only change uses two stable 16-bit radix passes for height
ordering. Scratch storage is one pointer per entry plus 512 KiB of bucket
counters, instead of a cached height and pointer per entry. Signed heights,
duplicate pointers, and the original height-only ordering are preserved. An
allocation failure releases scratch storage before using the prior direct
pointer-sort fallback. Default startup continues to use its original sort.

Three alternating baseline/candidate runs on the same isolated snapshot
measured 61.966 / 62.059 / 61.551 seconds (median 61.966) against
61.898 / 59.457 / 62.696 seconds (median 61.898). Overall time was within
run variation; sorting itself was faster. Median peak RSS fell from
17,696,148 to 17,521,848 KiB: another 170 MiB (0.985%). Ready-state RSS was
essentially unchanged; the saving is startup scratch space, not persistent
index storage. Best block, chainwork, recent/historical header results, and
`verifychain(3,100)` matched in all runs. The 39 block-manager/header-PoW
unit cases and the startup mode/reindex/missing-file functional test passed.
This result does not establish viability on a 4–8 GiB VPS.

The next Linux fast-only change bounds the block-index DB table cache to
64 tables and asks the kernel to reclaim clean file pages when the last
read-table reference is released. Existing LevelDB reads, checksums, writes,
DB format, and the caller's block/write cache budgets are unchanged; so are
`-dbcache=4096` and `-maxpowcache=2048` in these measurements. Default startup
explicitly disables this policy. Other platforms retain their existing Env.
This policy remains enabled for that block-index DB after startup; mainnet
sync throughput has not been benchmarked.

Three candidate runs measured 68.295 / 66.841 / 68.757 seconds (median
68.295) and median peak RSS 12,143,836 KiB. Compared with the accepted
pointer-scratch baseline above, peak RSS fell 5,252 MiB (30.69%) at a
6.397-second (10.33%) startup cost. The memory/time tradeoff was accepted
for OOM risk reduction. Ready-state RSS fell from 17,175,520 to
11,797,252 KiB. Persistent anonymous memory remained about 9.45 GiB;
the main saving is file-backed read cache, not smaller index objects.

A separate cold-copy comparison ran each version once in its own 24 GiB
cgroup with swap disabled. Copy preparation occurred outside that cgroup,
and only private copied files received cache-reclamation advice. Sampled
cgroup memory.current peaks fell from 16.770 to 11.610 GiB, including
file cache and kernel accounting; startup took 63.559 and 68.419 seconds.
There were no OOM events or swap use. This confirms a physical-accounting
saving in that experiment, not viability on a 4–8 GiB VPS. Early physical
measurements that charged snapshot-copy cache to the measurement cgroup
were excluded. The final three warm process-RSS runs are reported separately.

The 51 DB-wrapper/block-manager/header-PoW unit cases passed. The expanded
private regtest functional test covers mode changes, reindex, missing files,
and persistence/verification of newly generated blocks. Snapshot best block,
chainwork, 500 recent and historical header digests, and verifychain matched.

A follow-up small-cache option lowers only the Linux fast-mode block-index
DB table-cache floor to 16 entries. LevelDB previously rounded both raw
max_open_files=64 and max_open_files=32 to 74 (64 tables plus 10 other files),
so merely lowering the wrapper value gave no further saving. The opt-in
`small_table_cache` option keeps the historical floor for all other callers.
The wrapper requests 26 open files (16 tables plus that existing allowance).
Live table references, checksums, DB format and block/write budgets are
unchanged. This is independent of user -dbcache and -maxpowcache settings.

Three local baseline runs took 66.218 / 68.681 / 70.939 seconds (median
68.681), with median peak RSS 12,144,156 KiB. The 16-table candidate took
69.828 / 67.693 / 69.166 seconds (median 69.166), with median peak RSS
10,899,084 KiB. That saves 1,216 MiB (10.25%) for a measured median
0.485-second (0.71%) startup increase, within observed timing variation.
Ready-state RSS fell from 11,797,624 to 10,552,408 KiB. The 32-table
screening saved less memory at a similar single-run startup time, so the
16-table variant was selected. A sizeof audit found that field rearrangement
alone cannot shrink the current 152-byte CBlockIndex; that candidate was
left unchanged rather than altering proof evidence or field semantics.

The new regression fixture creates more than 16 SST tables and verifies
bidirectional reads, iteration and persisted contents after reopening with
both cache policies. All 52 DB-wrapper/block-manager/header-PoW unit cases
and the startup mode/reindex/missing-file/new-block functional test passed.
All six final snapshot runs matched best block, chainwork, header digests,
verifychain and normal shutdown. Mainnet sync throughput and viability on
4–8 GiB hosts remain unmeasured; resident anonymous index memory is unchanged.

One further private cold-copy run in a fresh 24 GiB/no-swap cgroup used
10.427 GiB sampled peak memory.current and 10.395 GiB process peak RSS,
starting in 70.979 seconds without OOM or swap. The previous 64-table
cold-copy record used 11.610 GiB cgroup peak. This single-run, cross-session
comparison supports physical-memory savings; the alternating three-run
comparison above is used for startup timing. Neither experiment proves
4–8 GiB host viability.

A final small-cache trial permits zero retained table-cache entries only
through the existing opt-in flag. Linux fast-mode block-index DB requests
max_open_files=10, the existing non-table allowance. LevelDB's supported
capacity-zero LRU path does not retain a cache reference; active iterator
handles continue owning their tables until normal release. Block/write cache
budgets, checksums, validation and the default 64-table floor are unchanged.
Reducing a positive capacity from 16 to 8 would not help: the 16 cache shards
round both requests up to one entry per shard.

Three alternating baseline/candidate runs measured baseline
68.005 / 71.892 / 69.027 seconds (median 69.027), against
67.675 / 71.067 / 66.907 seconds (median 67.675). Timing differences were
within run variation. Median peak RSS fell from 10,899,600 to 10,267,356 KiB,
another 617 MiB (5.80%). Ready RSS fell from 10,553,024 to 9,921,084 KiB.
All state/header checks and verifychain matched; the 52 relevant unit cases
and startup mode/reindex/missing-file/new-block functional test passed. The
multi-SST regression fixture now exercises zero retained tables as well.

A private cold-copy/no-swap 24 GiB cgroup run measured 9.811 GiB sampled
memory.current peak, 9.793 GiB process peak RSS and 68.618-second startup,
without OOM or swap. Resident anonymous index memory remains about 9.45 GiB,
and mainnet sync throughput/4–8 GiB viability remain unmeasured. The retained
read-cache policy stays in effect after startup, potentially adding disk reads.

## Fast-only compact block-index map

Fast startup uses stable append-only chunks for the original hash/index pairs
and an open-addressed lookup table instead of per-node links and pointer-sized
buckets. Each lookup slot stores a checked 32-bit entry ID and an 8-bit hash
fingerprint in five bytes. Salted full-key hashing limits predictable probe
collisions; the fingerprint is only a filter and every match still compares
the complete 256-bit key. This changes neither block hashes nor validation.

Live keys and CBlockIndex objects retain their addresses during growth and
rehash. Deleted entries are destroyed, and their storage is reused through an
allocation-free free list. Iteration skips deleted entries. The representable
entry count is bounded by the reserved 32-bit IDs and address-space capacity;
oversized reservation/insertion throws instead of truncating an ID.

The compact backend is selected only by `-fast-startup=1`. Mode zero retains
the original unordered_map backend, allocator, full CBlockIndex fields,
historical Yespower checks and startup algorithms. The public container facade
has constant overhead in both modes, but there is no shared compressed field
representation. DB formats, progress messages, user `-dbcache` and
`-maxpowcache` values are unchanged. The shared compact-chainwork prototype
was rejected after the user requested fast-only structural changes.

Three alternating fresh offline-snapshot runs used 44,806,391 entries and
the same `-parpow=8 -dbcache=4096 -maxpowcache=2048 -assumevalid=0` settings.
The control was tag `fast-startup-rss-stable-20261009` (8943f1f).

| Metric | Control | Compact map | Change |
|---|---:|---:|---:|
| Startup median | 73.963 s | 74.877 s | +0.915 s / +1.24% |
| Peak RSS median | 10,266,932 KiB | 9,162,092 KiB | -1,079 MiB / -10.76% |
| Ready RSS median | 9,920,540 KiB | 8,815,240 KiB | -1,079 MiB |

Control times were 73.963 / 75.061 / 72.373 seconds; candidate times were
74.877 / 74.550 / 76.706 seconds. The ranges overlap, so the small startup
difference is not a guaranteed per-run cost. The repeatable memory saving is
substantially larger than the measured percentage time increase. All six runs
matched best block, chainwork and 500 recent/500 historical header digests;
verifychain passed and the nodes shut down normally, without process swap.

Separate cold-copy trials in fresh 24 GiB/no-swap cgroups sampled physical
memory.current peaks of 9.822 GiB and 8.763 GiB. Process peak RSS was 9.793
and 8.737 GiB; startup took 73.233 and 75.618 seconds. Snapshot copying
occurred outside the measurement cgroups. No OOM or swap events occurred.
These single-run physical checks corroborate memory savings; the alternating
three-run comparison is the timing result. They do not prove 4-8 GiB VPS
viability or mainnet synchronization throughput.

The requested additional 50% RSS reduction was not reached: 9.79 GiB fell to
8.74 GiB, rather than about 4.9 GiB. On this build, the unchanged 152-byte
CBlockIndex and 32-byte key alone require about 7.68 GiB for this snapshot.
Much larger savings require a separate representation of index fields and
changes to their consumers, beyond this lookup-container refactor. Chunks
retain their high-water allocation for reuse. Future lookup-table growth can
temporarily retain both old and new tables; this is not an OOM guarantee.

The 101 relevant block-manager/header-PoW/chain/chainstate/wallet unit cases
passed with default options and with explicit `-fast-startup=1`. The final
erase/reuse tests and a 100,000-entry lifecycle harness passed ASan/UBSan.
Private functional tests cover competing branches, invalidate/reconsider,
mode-zero/mode-one restarts, reindex-chainstate, missing files and persistence.
The daemon, Qt executable and unit-test executable built successfully.
Raw measurements and the scope/goal analysis are preserved in
`~/work-archive/sugarchain-tests/ibd-optimization-evidence/20261009-pr10-compact-index/report.md`.

## Original-mode storage isolation

The legacy backend is now exactly
`std::unordered_map<uint256, CBlockIndex, BlockHasher>` with its default
allocator, as before PR #10. Compact chunks, lookup slots, free-list state
and salted hashing are owned by a separately constructed fast backend.
Mode zero creates none of those allocations. `BlockHasher` has its original
non-`noexcept` signature, preserving the original libstdc++ cached-hash policy.
The regression test compares the exact map type, bucket growth, reservation,
iteration order and erasure against a directly instantiated original map.

The Yespower cache similarly selects either the original single eagerly
allocated cache or a separately owned tiered fast cache. Tier metadata is
not constructed in mode zero. The configured budget, successful-proof-only
evidence, batch domain separation and locking are unchanged. Tests switch
backends repeatedly and check genuine-proof caching and invalid-proof
rejection after every reset.

These boundaries restore the original storage and startup algorithms, not
an identical historical executable: progress improvements and constant mode
dispatch remain shared. More memory in mode zero is expected and is not a
regression against the user's requested original policy. No mode-zero
startup benchmark is required for this change.

Existing mode-one policies are preserved. In particular the compact lookup
map, tiered proof cache and block-index DB read-cache policy still remain
selected for that block manager/process after startup; this change does not
turn them into transient startup-only policies or measure IBD throughput.

Three alternating before/after runs, all with `-fast-startup=1`, used fresh
copies of the same 44,806,391-entry offline snapshot and the same cache,
verification and worker settings as the compact-map comparison above.

| Median metric | Before separation (133bc57) | After separation |
|---|---:|---:|
| Startup | 77.585 s | 75.237 s |
| Peak RSS | 9,162,200 KiB | 9,161,488 KiB |

Control times were 77.585 / 75.023 / 77.776 seconds; candidate times were
75.218 / 77.479 / 75.237 seconds. The time ranges overlap, so this confirms
preservation of the fast path rather than a guaranteed new 3% speedup.
Peak RSS remains about 8.74 GiB. All six runs matched best block, chainwork
and recent/historical header digests, passed verifychain and shut down cleanly
with no sampled process swap. The 103 relevant unit cases passed in both
default and explicit fast modes; both private functional suites and the
100,000-entry ASan/UBSan lifecycle harness passed. Mode-zero startup was
not benchmarked, as requested. Raw results and the scope audit are in
`~/work-archive/sugarchain-tests/ibd-optimization-evidence/20261009-pr10-mode-isolation/report.md`.

## Regression coverage

`header_pow_tests` covers real Yespower validation, opt-in disk trust, failure
ordering, reservation callbacks and the two progress policies. The chain and
DB tests compare both explicit helper modes. The block-manager and chainstate
tests can also run with `-- -fast-startup=1` to cover the optimized traversal
paths. `feature_fast_startup.py` uses private regtest data to check mode
switching, unchanged chain state, progress, invalid flags, reindex-chainstate
and missing block files. No full historical mainnet PoW replay is required for
these boundary tests.

## First-block warning delay: final remeasurement

Commit `5293ff3dba` batches unknown-versionbits warning counts only when
`-fast-startup=1`. Default mode retains the separate per-bit scans. Shared
counts preserve the existing eligibility rules, deployment thresholds, fork
boundaries and per-bit state caches; they do not skip new-block verification.

Three runs of parent `c221e78cb5` and three runs of `5293ff3dba`, in
A/B/B/A/A/B order, connected the same real mainnet block to independent copies
of a 44,806,391-entry offline snapshot. Both versions used `-fast-startup=1`.
The metric runs from the script-verification announcement to the first
`UpdateTip` log, using microsecond log timestamps.

| Metric | Parent | Batched warning counts |
|---|---:|---:|
| Three elapsed times (seconds) | 162.289 / 165.093 / 159.355 | 8.776 / 9.026 / 9.079 |
| Median (seconds) | 162.289 | 9.026 |
| Median process peak RSS (KiB) | 9,175,880 | 9,175,980 |

The median saving is 153.263 seconds (94.44%). All six runs reached height
44,805,168 with identical tip hash and chainwork, passed `verifychain(3,100)`
and shut down normally. No process swap was sampled. The original snapshot's
618 file names, sizes and modification times remained unchanged; this was a
metadata comparison, not a full content-hash audit. The operating node's PID
and start time remained unchanged.

Both versions used `-parpow=8`, `-dbcache=4096`, `-maxpowcache=2048`,
`-assumevalid=0`, `-checkblocks=6`, `-checklevel=3`, disabled wallets and P2P,
and received the block through local `submitblock`. **The experiment also
used `-maxtipage=31536000` to exercise the non-IBD warning path with this old
snapshot.** IBD was confirmed false before and after submission. This override
is a measurement condition, not a recommended normal-node setting.

This measures actual block connection, but excludes peer discovery and block
download. It does not replay the original 168-second network session, establish
IBD throughput or guarantee the same improvement on every machine. Background
host load and filesystem cache can affect these local measurements. Default
mode startup was not benchmarked in this remeasurement. The focused
`versionbits_tests/unknown_warning_shared_counts` regression test passed again.
Sanitized timing data is in [the benchmark summary](benchmarks/first-tip-warning-20261010.json).
