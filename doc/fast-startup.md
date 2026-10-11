# Startup mode boundaries

`-fast-startup=0` is the default: historical Yespower verification is retained.
`-fast-startup=1` trusts eligible historical disk-index entries and skips their
repeated Yespower calculation. Both modes now use all retained startup optimizations (items 1–19);
selecting full verification does not select a deliberately slower index layout.
Neither mode treats disk trust as actual proof evidence.

| Stage | Default / `=0` | `=1` |
|---|---|---|
| Historical Yespower | Verify every loaded header through the PoW verifier | Trust eligible persisted non-genesis TREE-or-higher entries |
| Disk iterator | Key-prefix checks, direct deserialization where supported | Same |
| Count / capacity | Key-only pre-count; 1.5x bucket reservation | Same |
| Index storage | Standard map with existing pool allocator and local hash policy | Same |
| Pointer collection / height ordering | Collect during insertion; two-pass height sort | Same |
| Work reconstruction | Exact optimized compact-target formula, original fallback | Same |
| Referenced files | Collect while linking | Same |
| Linking / best-header read-ahead | Existing read-ahead hints | Same |
| Headers / chainstate candidates | Reuse sorted pointers; direct candidate traversal | Same |
| Chain arrays / sequence IDs / witness checks | Existing read-ahead hints and all checks | Same |
| Versionbits warnings | Shared counts across bits | Same |
| Positive Yespower cache | Grow within the configured budget; only actual successful proofs | Same |
| Block-index DB read cache | Reduced/reclaimed cache policy | Same |

The public flag now selects historical disk trust rather than which performance
algorithms run. Mode zero still computes historical Yespower and never treats
persisted status as live proof evidence. The reference algorithms remain available
internally for differential tests; there is no extra public startup mode.

The original 13-item selection is preserved as historical evidence in
[the previous screening](shared-verified-startup.md). The all-19 comparison is in
[the current screening](all-verified-startup.md), with whole small-node timings
separate from bounded large-index verification and metadata component timings.

## Progress

Both modes pre-count entries and report percentages. In full-verification mode,
progress advances only after pending Yespower checks succeed; a failed batch
never publishes proof status or premature completion. Interrupt checks remain
active during both counting and loading. The count scan introduces small extra
work on tiny databases; reduced allocations and traversals target large indexes.

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

The sequence below records earlier optimization experiments. Intermediate
map allocators and layouts are not the current implementation; both modes
now use the standard pooled map and 1.5x reservation. Historical results below
were measured in fast mode and are not mode-zero performance claims. For the final map policy
and updated measurements, see [conservative reservation](#conservative-block-index-memory-reservation).

The fast-mode Yespower cache allocates at most 16 MiB for individual entries
initially, plus the existing batch cache. It grows in bounded tiers only after
verified entries fill the active tier. Older tiers retain their successful
proofs; cache misses still run full Yespower verification. The configured
`-maxpowcache` value remains the upper entry-memory budget, including batch
entries. Both modes now use on-demand allocation; the reference eager backend remains tested.

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
mode uses a local non-throwing hasher with the identical hash value; the
compact-table experiment was removed. The global `BlockHasher` is unchanged. The isolated
snapshot's best block, chainwork, and clean shutdown still matched. It does
not by itself make a 4–8 GiB VPS viable; that requires a larger structural
reduction and a constrained-memory test.


The next fast-only change uses two stable 16-bit radix passes for height
ordering. Scratch storage is one pointer per entry plus 512 KiB of bucket
counters, instead of a cached height and pointer per entry. Signed heights,
duplicate pointers, and the original height-only ordering are preserved. An
allocation failure releases scratch storage before using the prior direct
pointer-sort fallback. Both production modes now select this sorter.

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

## Conservative block-index memory reservation

Both startup modes now use standard node-based `std::unordered_map` containers.
The reference backend keeps `std::unordered_map<uint256, CBlockIndex, BlockHasher>`.
Both production modes use the existing `PoolResource`/`PoolAllocator` and a
local non-throwing hasher with the same `ReadLE64` result. The global
`BlockHasher` remains unchanged; the node-layout optimization stays local to
the pooled map. The pool owner outlives the standard map and its nodes.

CompactBlockMap, custom raw object storage, free list and open-addressed slots
have been removed. The existing backend-selection facade now forwards to two
standard maps, avoiding changes to block-index consumers. Standard containers
manage object lifetime, insertion and erasure. No block-index fields,
serialized DB formats, consensus checks or proof-trust policies change.

Both startup modes reserve buckets before loading. The requested entry
capacity is `count + count / 2` (1.5x), with a checked fallback to `count` if
that addition would exceed the container's maximum size. This replaces the
previous exact-count choice after a same-code, repeated startup comparison.
The standard library may round actual buckets up; 1.5x is not an exact bucket
count or a fixed upper bound. Parent placeholders and later incoming headers
can trigger normal growth, which preserves references to keys and indexes.
Allocation failure during optional reservation retains the existing fallback
to incremental growth. Later entries retain normal standard-map growth.

### Reservation sweet spot: same-code repeated measurement

Four binaries from the same source (`aa2ff0883d`) differed only in requested
bucket capacity: 1x, 1.5x, 2x and 3x. Three startups per setting used the same
44,806,391-entry offline snapshot, in mixed order. Each run reset its private
block-index and chainstate DBs from that snapshot. Only private copied files
received cache-reclamation advice; no global cache drop was used. Preparation
time is excluded. Worker, cache and verification flags were identical:
`-fast-startup=1 -parpow=8 -dbcache=4096 -maxpowcache=2048 -assumevalid=0
-checkblocks=6 -checklevel=3`, wallets/P2P disabled.

| Requested capacity | Startup median (range), seconds | Peak RSS median, KiB | Saving versus 3x, MiB |
|---|---:|---:|---:|
| 1x | 80.534 (79.176–80.940) | 9,542,596 | 708.42 |
| **1.5x (selected)** | **75.723 (75.488–75.794)** | **9,716,788** | **538.31** |
| 2x | 74.876 (72.868–74.962) | 9,914,604 | 345.13 |
| 3x | 74.631 (72.329–74.940) | 10,268,016 | 0 |

1.5x reduces median startup time by 4.810 seconds (5.97%) versus 1x.
Compared with 3x, it saves 538.31 MiB at a 1.092-second (1.46%) median cost.
2x saves less memory, requiring another 193.18 MiB for just 0.848 seconds
of median improvement over 1.5x. This makes 1.5x the selected balance for this
snapshot. Time ranges overlap between 2x/3x; three samples do not establish
statistical equivalence or a guaranteed performance change on another host.
Ready-state median RSS was 9,370,628 KiB at 1.5x versus 9,921,448 KiB at 3x.

All 12 runs had identical best block, height, chainwork and 500 recent/500
historical header digests (each read twice), passed `verifychain(3,100)`, and
exited normally. No process swap was sampled; the host itself has swap in use.
These are process-RSS measurements without a constrained cgroup. Neither
4–8 GiB VPS viability nor mainnet IBD throughput is established. Operating
node data and processes were not used for the experiment.

[Sanitized 12-run results](benchmarks/blockindex-reservation-sweetspot-20261011.json)
include the order, binary hashes, individual timings and matched chain state.
Raw records and checkpoint are retained in
`~/work-archive/sugarchain-tests/ibd-optimization-evidence/20261011-blockmap-sweetspot/`.

### Historical exact-count and compact-map trials

A component screening test inserted 44,806,391 deterministic unique keys into
the original map, with two runs per reservation size. Compared with its older
three-times reservation, exact-count reservation reduced median peak RSS by
708 MiB. Insertion increased from 10.827 to 14.668 seconds; one million hits
increased from 0.1153 to 0.1484 seconds. These are component measurements,
not full startup or mainnet IBD throughput predictions. This preliminary
screening used the default allocator, not the pooled
control below. Discarding the earlier pool optimization largely cancels the
bucket savings in full-node measurements, so fast mode retains that existing
optimization. The earlier exact-count choice preserved standard containers; the repeated
comparison above subsequently selected 1.5x headroom to recover startup speed.

The removed compact implementation previously saved 1,079 MiB in full-node
measurements (10,266,932 to 9,162,092 KiB median peak RSS). Those results are
historical and do not describe this standard-map implementation. It retains
more memory than CompactBlockMap in exchange for much less custom
code. The unchanged 152-byte CBlockIndex and 32-byte key alone require about
7.68 GiB for that snapshot, before map and other process overhead; this is not
a guarantee of operation on a 4-8 GiB VPS.

The original-container tests cover duplicate insertion, reference stability
across growth and reserve, erase/reinsert and preserved chainwork. The private
`rpc_invalidateblock.py` test now covers competing branches, invalidation,
reconsideration and restarts in both startup modes (integrating the former
standalone `feature_blockindex_map.py` fixture).

Before the same-code comparison above, one cold, private copied-snapshot
run per version compared the earlier pooled
three-times implementation (`8943f1f3`) with pooled exact-count reservation:

| Metric | Earlier pooled 3x | Pooled 1x |
|---|---:|---:|
| Startup | 77.597 s | 85.070 s |
| Peak RSS | 10,267,584 KiB | 9,541,804 KiB |
| Ready RSS | 9,920,956 KiB | 9,195,116 KiB |

Peak RSS fell by 708.77 MiB, about 9.79 to 9.10 GiB. Startup took 7.47 seconds
longer in this single pair; this is not a repeated median or guaranteed cost.
The control predates other PR changes, so this full-node pair is not an
isolated reservation-only timing experiment. Both used 44,806,391 entries,
`-parpow=8 -dbcache=4096 -maxpowcache=2048 -assumevalid=0 -checkblocks=6
-checklevel=3`, with wallets and P2P disabled. Only each private copy's file
cache was discarded before measurement; no global cache drop was used.
Best block, height, chainwork and 500 recent/500 historical header digests
matched. Both passed `verifychain(3,100)`, exited normally and had no sampled
process swap. These are process RSS results without a constrained cgroup;
4-8 GiB VPS viability and mainnet IBD throughput remain unmeasured.

Before the test consolidation below, the selected 1.5x release daemon, Qt
and unit-test executable built successfully.
The relevant block-manager, header-PoW, chain, chainstate, DB and pool suites
passed 69 cases in each of default and explicit fast modes. ASan/UBSan with
leak detection passed the block-manager and pool suites (19 cases per mode),
plus the competing-branch/restart functional test. The normal startup-boundary
and competing-branch functional tests also passed. This is targeted coverage,
not a complete-suite or libFuzzer claim.

[Sanitized results](benchmarks/blockindex-exact-reservation-20261011.json)
contain no local paths or RPC credentials. Raw test records remain in
`~/work-archive/sugarchain-tests/ibd-optimization-evidence/20261011-conservative-blockmap/`.

## Historical proof-cache isolation experiment (superseded)

Earlier experiments restored the original map and eager proof-cache backend in
mode zero. The shared-optimization change supersedes that policy: production
mode zero now uses the pooled standard map and grows the positive-proof cache,
while continuing to check every historical header. Reference backends remain
for differential tests. Original measurements describe the historical experiment;
they are not claims that current mode zero retains those old storage policies.


## Regression coverage

`header_pow_tests` covers real Yespower validation, opt-in disk trust, failure
ordering, reservation callbacks and the two progress policies. The chain and
DB tests compare both explicit helper modes. The block-manager and chainstate
tests can also run with `-- -fast-startup=1` to cover the optimized traversal
paths. `feature_fast_startup.py` uses private regtest data to check mode
switching, unchanged chain state, progress, invalid flags, reindex-chainstate
and missing block files. No full historical mainnet PoW replay is required for
these boundary tests.

## Test audit and consolidation

The test audit removes a runtime comparison of standard-library bucket counts
and iteration order. The compile-time guard on the original legacy map type
remains in the address-stability regression, which checks keys, parent links,
duplicate insertion and preserved node addresses across growth in both modes.

Instead of separate smoke fixtures, existing regressions now check:

- `dbwrapper_iterator`: full values after repeated failed key/value decoding,
  both iterator readers, both obfuscation settings and both iterator positions.
- `startup_block_index_load_checks_every_header`: the existing 2,001-header
  load also checks the legacy 2,000-entry progress boundary for 1/3/8 workers,
  replacing a duplicate proof-generating fixture.
- The growing-cache regression: all 25 verified headers remain warm after
  growth, and repeated invalid proofs are recomputed and rejected.
- `rpc_invalidateblock.py`: invalidated/reconsidered competing branches survive
  default/fast restarts with every active-chain hash and chainwork preserved;
  the header-only best header also survives. This replaces the standalone
  block-index-map functional fixture.
- The existing splash test: the progress row must visibly change while the
  title/artwork remain fixed, replacing non-null/same-size screenshot checks.

After consolidation, the relevant release suites pass 66 cases in each mode.
ASan/UBSan with leak detection pass the block-manager/DB suites (26 cases per
mode), the growing-cache case in each mode, and the extended invalidate RPC
functional. The normal invalidate RPC functional also passes. Release and
sanitized test builds pass. The Qt splash test passes; the full Qt run still
fails the previously recorded URI, RPCNested and Wallet cases. This does not
claim a passing complete test suite. Production code and benchmark results
are unchanged by this test-only change.

## First-block warning delay: final remeasurement

Commit `5293ff3dba` batches unknown-versionbits warning counts only when
`-fast-startup=1`. Both modes now use shared per-bit counts. Shared
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
