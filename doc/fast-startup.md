# Startup mode boundaries

`-fast-startup=0` is the default. It uses the startup algorithms from before
PR #10 (`796902953802cbab9d3c2201ab8eef8bc1f7cf5a`), including the existing
`-parpow` historical PoW checks. `-fast-startup=1` opts into both historical
disk-index trust and the subsequent PR #10 performance optimizations.

| Stage | Default / `=0` | `=1` |
|---|---|---|
| Disk iterator | Copying key/value streams, decode the key | Inspect key prefix, directly deserialize unobfuscated values |
| Count and capacity | One loading pass, incremental map growth | Key-only pre-count, reserve spare buckets and pointer capacity |
| Index node allocation | Standard per-node allocator | Stable-address pooled nodes, with large bucket allocations unchanged |
| Pointer collection | Collect from the map after loading | Collect newly inserted entries during loading; retain pre-existing entries on reload |
| Height ordering | `std::sort` | Integer sort with temporary compact height/pointer cache |
| Work reconstruction | Original 256-bit formula | Equivalent exact 64-bit formula for eligible compact targets |
| Index linking | Original traversal | Prefetch upcoming index fields |
| Referenced block files | Separate complete map traversal | Collect during linking |
| Best-header selection | Collect and sort a second time | Reuse this load's sorted vector and prefetch fields |
| Chainstate candidates | Temporary pointer vector | Iterate the map directly |
| Active-chain construction / sequence IDs / witness checks | Original traversals | Read-ahead hints; the same checks and traversal results |
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
changes the container's storage layout in default mode, but not its hash
value, lookup algorithm, validation, or startup mode boundary. The isolated
snapshot's best block, chainwork, and clean shutdown still matched. It does
not by itself make a 4–8 GiB VPS viable; that requires a larger structural
reduction and a constrained-memory test.

## Regression coverage

`header_pow_tests` covers real Yespower validation, opt-in disk trust, failure
ordering, reservation callbacks and the two progress policies. The chain and
DB tests compare both explicit helper modes. The block-manager and chainstate
tests can also run with `-- -fast-startup=1` to cover the optimized traversal
paths. `feature_fast_startup.py` uses private regtest data to check mode
switching, unchanged chain state, progress, invalid flags, reindex-chainstate
and missing block files. No full historical mainnet PoW replay is required for
these boundary tests.
