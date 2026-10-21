# Unified IBD RSS management

## Scope

The original run began at 2026-10-13 13:17:55 KST. Resource CSV samples
showed four early steps, then linear header-replay growth and a much larger
write-related peak. The sampled maximum was 22.691 GiB, while VmHWM reported
22.698 GiB. Those are observations of an older running binary; the process
was not restarted to test these changes. There is no original allocation trace
that assigns every byte to an individual component.

| Event | Original elapsed time | Original RSS (GiB) | Treatment |
|---:|---|---|---|
| 1 | 2m51s | 0.234 to 0.266 | Retain lazy allocation of growing proof-cache pages |
| 2 | 8m13s | 0.267 to 0.362 | Same cache implementation |
| 3 | 23m47s to 23m52s | 0.362 to 0.645 | Same cache implementation |
| 4 | 70m58s | 0.645 to 1.495 | Same cache implementation |
| 5 | 419m33s to 476m57s | 1.504 to 10.249 | Pool dirty nodes; periodically persist them during header acceptance |
| 6 | 476m57s to 477m42s | 10.249 to 17.317 | Avoid accumulating a replay-sized serialized write batch |
| 7 | 482m43s peak | 22.691 | Reduce the large-batch input to memtable/recovery allocation |

## Preserve the existing cache improvement

[The proof-cache comparison](yespower-cache-budget.md) records the four
isolated allocation bursts falling from 32.25/96.75/289.875/869.75 MiB to
0.25/1/2.75/8.375 MiB with 4 KiB lazy pages. Budget, slots, eviction, proof
checks and startup modes are unchanged. No new cache adjustment is needed
for these events. This defers allocation: once all pages fill, overhead can
make RSS slightly larger. These burst reductions cannot be added to a
node-wide memory-saving percentage.

## Bound header accumulation with existing flush semantics

`ProcessNewBlockHeaders` previously accepted arbitrary numbers of headers
without a count-triggered index flush. Block processing drives periodic
flushes, so headers-first replay could accumulate millions of dirty entries
before the first full block. Pooling those entries and releasing the temporary
pointer vector helped, but left the large serialized batch and memtable.

After an accepted header, check whether at least 32,768 unique index entries
are dirty. If the active chainstate can flush, call its existing
`FlushStateToDisk(FORCE_SYNC)` path. Checking inside the loop also covers
large internal header submissions. Duplicate submissions do not increase
the unique dirty count; invalid headers retain their validation failure.

The complete existing ordering is retained: flush block/undo files, write
index entries/evidence/file metadata in one synchronous atomic batch, then
sync chainstate. No batch is split, and no new DB record, transaction marker,
WAL mechanism, or evidence format is introduced. All operations remain under
`cs_main`. A parent is either already durable from an earlier flush or part
of the same atomic index write. Failures use the existing fatal flush path.
Coins cache entries are synced rather than forcibly evicted. The trigger is
shared by both fast-startup modes; it does not change which historical
proofs must be recomputed. For the earlier synthetic record shape below, a full
32,768-entry threshold batch occupies approximately 6.09 MiB (the original
262,144-entry threshold used approximately 48.75 MiB). Other record shapes and
file metadata can change that value.

This threshold schedules persistence; it is not a byte budget. It applies
when header acceptance has a flushable active chainstate. Direct index
insertion, other dirtying paths, unusually large pre-existing sets and the
pre-initialization phase are not subject to a universal batch limit. Resident
CBlockIndex/map memory still grows with index count. Frequent sync writes and
compaction may cost time on slow disks; the observed original peak is not
proven eliminated until a new complete IBD measurement.

## Test design

The existing unpersisted-header block-manager test now seeds valid regtest
ancestors up to the real threshold and submits boundary headers through the
production validation path. Debug index-consistency checks remain enabled
for those submissions. It checks a duplicate and invalid PoW below the
threshold, parent/hash persistence at the boundary, unchanged coins best
block, absence of false Yespower evidence, and the next still-dirty header.
The direct seeding avoids an O(N) debug scan after each seeded ancestor;
it is not a replacement for the actual P2P test.

The existing fast-startup functional test now sends 32,769 solved regtest
headers over P2P in each startup mode, then forcibly terminates only its
private test node. On restart, exactly the completed 32,768-header prefix
survives, the one unpersisted suffix does not, and blocks, best block, chainwork
and verifychain remain correct. The second mode extends the first durable
header tip with fresh headers, so it exercises another real write boundary
rather than resubmitting already-persisted duplicates. This functional fixture
disables only the debug
index-consistency scan, not header validation. Its existing startup-mode
switch and missing-file checks remain in place. A process kill after a
completed write is not a power-loss or mid-fsync fault-injection test.

## Earlier component measurement method and limits

The isolated helper uses production FastBlockMap, CBlockIndex, node pools
and LevelDB with a 4 MiB write buffer and zero block-cache capacity. It uses
raw LevelDB table/file-cache defaults, not the full CDBWrapper fast-startup
read-reclamation policy. In particular, reopen/scan peaks are helper metrics,
not predictions of node startup RSS. It keeps
all map indexes resident, constructs paired 33-byte keys with 90-byte
header-shaped and 33-byte evidence-shaped values, and compares one large
flush against 262,144-entry flushes. Both paths release dirty nodes and input
pointers before DB insertion, so the baseline includes the preceding fixes.
Synthetic records are never admitted as actual node verification evidence.

Each comparison runs twice in forward/reverse order. Every stored key and
value is checked after reopen, including key membership in the resident map
and the exact total record count. The write-phase and whole-process peak
(including reopen/scan) are reported separately. This helper does not model
Yespower CPU cost, network activity, block files, UTXO syncing or a complete
production IBD. Builds and the operating IBD shared the host. Timing is
observational, not a whole-IBD performance guarantee. An aborted initial
helper run with a temporary-iterator bug was excluded and corrected before
the recorded comparisons; that bug was not in production code. The corrected
helper also passed ASan/UBSan with leak detection across a 300,000-input,
two-flush/reopen run. Sanitized RSS is not used for the performance tables.

## PR scope

These changes are independent memory/CPU management work temporarily carried
in PR #11. Split that work from Lazy PoW/persistent evidence into separate
PRs before merging. The operating node and data remain untouched.

## Earlier 262,144-entry component results

These measurements describe the preceding threshold, not the current 32,768
setting. The follow-up below measures the additional change independently.

Two repetitions per variant and count, median unsanitized values. Baseline
includes the preceding dirty-node pooling and early pointer-array release.

| Inputs | Baseline write peak (MiB) | Bounded write peak (MiB) | Reduction | Baseline whole peak (MiB) | Bounded whole peak (MiB) | Reduction |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000,000 | 632.867 | 380.041 | 39.95% | 822.027 | 490.244 | 40.36% |
| 4,000,000 | 2512.873 | 995.592 | 60.38% | 3269.834 | 1688.969 | 48.35% |
| 12,000,000 | 7480.611 | 2789.125 | 62.72% | 9762.594 | 4180.701 | 57.18% |

Whole peak includes DB reopen and a full key/value scan with raw LevelDB
cache defaults; it is not node RSS. Write peak ends before DB reopen.

| Inputs | Baseline ingest/flush (s) | Bounded ingest/flush (s) | Baseline reopen/scan (s) | Bounded reopen/scan (s) | Sync writes, baseline / bounded |
|---:|---:|---:|---:|---:|---:|
| 1,000,000 | 5.482 | 4.273 | 6.245 | 1.981 | 1 / 4 |
| 4,000,000 | 30.135 | 15.974 | 46.649 | 9.711 | 1 / 16 |
| 12,000,000 | 94.145 | 68.892 | 105.097 | 17.715 | 1 / 46 |

At 12 million, peak RSS sampled immediately before flushes fell from
2755.066 to 2391.807 MiB (13.19%); this is distinct from write-time and
reopen peaks. At one million the same pre-flush metric rose from 239.080
to 275.340 MiB (15.17%), because the bounded path had already begun DB
activity. Early writes do not guarantee lower RSS at every instant.

At 12 million, the largest synthetic serialized batch fell from
2,340,000,012 bytes (2231.598 MiB) to 51,118,092 bytes (48.750 MiB).
The remaining resident map/indexes and ongoing DB work are not removed.
More sync calls may have a different timing cost on another disk. No
full-IBD wall-time improvement or new mainnet peak is established.

## Earlier validation results

GUI and daemon builds passed. The selected normal cache, header-PoW,
block-manager, DB-wrapper, chainstate-write and flush suites passed 65 cases
with 951,095 assertions. After tightening test-side DB locking, the affected
boundary case passed again with 234 assertions. The expanded fast-startup
functional test passed, including its private-node crash/restart sequence.
The benchmark completed all 12 valid comparison runs and verified every
stored key/value after reopen. The corrected helper also passed its separate
sanitizer run described above.

The final production ASan/UBSan build, with leak detection and halt-on-error
enabled, passed the same 65 selected cases and 951,095 assertions. This was
not a run of all 813 test cases.

The operating IBD process remained running with the same PID/start time.
No mainnet replay, production restart, TSan, Windows/macOS build, mid-write
fault injection or simulated power failure was performed. Consequently the
original 22.691 GiB peak and total IBD duration still require a new full-run
measurement; passing these tests is not a claim that all RAM pressure is gone.

## Follow-up: 32,768-entry persistence threshold

The follow-up keeps all four proof-cache improvements and changes only the
header-accumulation threshold from 262,144 to 32,768. This makes the same
ordered, atomic persistence path run sooner; it does not reduce PoW checks,
shrink the configured proof cache, remove CBlockIndex fields, or replace the
standard map. The measured CBlockIndex is 152 bytes on this x86-64 build. Its
retained members require 145 bytes before 8-byte alignment, so simple member
reordering cannot reduce that size.

Candidate screening compared 262,144 / 65,536 / 32,768 / 16,384 / 8,192.
At four million inputs, the 16,384 candidate's whole-helper peak was another
55.48 MiB below 32,768, but ingest/flush took 64.23 versus 32.83 seconds; its
write-phase peak was not lower in that pass. The 8,192 candidate made 489
sync writes and took 74.30 seconds. Select 32,768 to retain substantial peak
reduction without the smaller candidates' additional write cost. This is a
measured compromise, not a universal optimum for all hardware.

### Comparison using the actual DB wrapper

The final helper uses the production CDBWrapper and CDiskBlockIndex
serializer, the production map and pools, a 2 MiB block-tree DB cache budget,
and the Linux inactive-read-cache reclamation policy. Consequently its write
buffer is 512 KiB, with the normal uncompressed 32 MiB target table size and
Bloom filter. It retains the entire map throughout each process and reopens
the DB to check every key, serialized index field, synthetic value and count.
It holds cs_main while accessing indexes. The paired `y`-shaped records use
**version zero**, never valid proof evidence; no benchmark data is sent to a
node. Keys and headers are synthetic, so this is a persistence-memory
experiment, not mainnet validation. File metadata, real proof hashing,
network activity and UTXO syncing are not reproduced here.

Each input/threshold pair runs twice in forward/reverse order. Values below
are medians. Baseline already includes the 262,144 limit, pooled dirty nodes,
and early pointer-vector release. These results use a different DB policy
from the earlier raw-LevelDB helper; do not combine their percentages.

| Inputs | Write peak before → after (MiB) | Reduction | Whole helper peak before → after (MiB) | Reduction | Ingest/flush before → after (s) | Sync writes before → after |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000,000 | 428.168 → 285.268 | 33.37% | 451.275 → 379.818 | 15.83% | 4.784 → 4.263 | 4 → 31 |
| 4,000,000 | 1124.744 → 869.061 | 22.73% | 1365.139 → 984.389 | 27.89% | 17.965 → 34.277 | 16 → 123 |
| 12,000,000 | 3029.959 → 2411.209 | 20.42% | 3029.959 → 2473.568 | 18.36% | 61.830 → 153.349 | 46 → 367 |

At 12 million inputs, the largest serialized batch falls from 48.500 to
6.063 MiB (87.50%), while synchronous writes rise from 46 to 367. Ingest/flush
increases by 91.518 seconds (148.02%) in this component experiment. This is
not a measured whole-IBD slowdown. The remaining resident indexes dominate
the reduced peak. Earlier one-million-input screening peaks varied from the
repeated results because of allocator/compaction timing; no single screening
run is presented as the final measurement.

### Actual private-node comparison

An isolated regtest node accepts one million solved headers over P2P in
2,000-header messages, with `-fast-startup=0`, `-checkblockindex=0` (debug
consistency scan only), and `-dbcache=16`. The block-tree DB still has its
normal 2 MiB budget. Compare copied pre-change and rebuilt daemons in
baseline/candidate/candidate/baseline order. Both the operating mainnet node
and its data remain untouched. Each test node then shuts down gracefully and
restarts; header count, active best block and verifychain are checked.

| Metric, median of two runs | 262,144 | 32,768 | Reduction |
|---|---:|---:|---:|
| Peak RSS through header acceptance | 370.289 MiB | 295.082 MiB | 20.31% |
| RSS at end of header acceptance | 304.055 MiB | 266.203 MiB | 12.45% |
| Peak RSS during restarted process | 353.223 MiB | 311.232 MiB | 11.89% |
| Higher peak across acceptance/restarted processes | 370.289 MiB | 311.232 MiB | 15.95% |
| P2P fixture elapsed time through header acceptance | 40.850 s | 39.883 s | Within observed variation |

Baseline elapsed times were 37.667/44.033 seconds; candidate times were
39.396/40.370 seconds. Treat them as similar in this experiment, not proof
of speedup. Regtest uses SHA256d and does not reproduce mainnet Yespower or
genuine persistent evidence writes. Builds, other checks and operating IBD
shared the host during the measurements. Mainnet IBD peak and total time
remain unmeasured for the new binary; no percentage above can be applied
directly to the original 22.691 GiB observation.

### Follow-up validation

Normal GUI/daemon builds and the selected 65 unit cases passed all 951,095
assertions. ASan/UBSan with leak detection and halt-on-error passed the same
cases/assertions. The expanded fast-startup functional test passed fresh
write-boundary crash/restart sequences in both modes, including the lost
unpersisted suffix and unchanged active chainstate. All four private-node
benchmark runs passed restart/verifychain; all 12 final helper runs passed
full record checks. The complete 813-case suite, TSan, Windows/macOS and
power-loss/mid-write fault injection were not run. No operating node was
restarted, and no full mainnet IBD result is claimed.

## Follow-up: ordinary full-block persistence

The next baseline is `e7b8a7937072a6aaeada04b64cdcfd8fb79bb0c1`, including
the 32,768-entry header trigger and all earlier memory changes. Full blocks
can enter `AcceptBlock` without passing through `ProcessNewBlockHeaders`.
Their index/status changes could still accumulate until the ordinary timer,
coin-cache pressure or another forced write. A private full-block-only test
confirmed that the baseline did not persist the expected count boundary.

Add `BlockIndexWriteDue()` to the existing `PERIODIC` decision in
`FlushStateToDisk`. Keep the existing mode, timer, coin-cache pressure and
pruning conditions. In particular, replacing PERIODIC with FORCE_SYNC would
suppress its LARGE coin-cache eviction; the implementation does not do that.
The header trigger, threshold, atomic index write, block/undo-before-index
ordering, chainstate ordering, PoW policy and DB formats remain unchanged.
NONE and IF_NEEDED gain no new count trigger. COINDB debug output records
`index=1` when index pressure requests the write.

This is an accumulation safeguard for another path, not proof that this
path caused the original events 6–7. It schedules complete writes at existing
safe opportunities; it is not a universal dirty-set or serialized-byte cap.
Pre-initialization, large pre-existing sets and bulk state transitions may
still exceed it. Mempool callers using PERIODIC may also incur an index
write. More frequent writes can increase validation/RPC delay and disk work.

### Full-block comparison method

Private regtest nodes receive solved, coinbase-only blocks directly over P2P,
without prior header announcements. Both daemons use `-fast-startup=0`,
`-checkblockindex=0` (only the expensive debug consistency scan), `-dbcache=512`
and COINDB logging. A fixed mock clock prevents elapsed-time writes. Actual
consensus checks, block/undo files, block-tree DB and UTXO persistence remain
active. Regtest SHA256d is not mainnet Yespower.

After accepting the input, both arms call `gettxoutsetinfo("none")` to persist
the remaining suffix and scan UTXOs. Include that final write in elapsed time
and peak memory: excluding it would hide the baseline's deferred large write.
Sample RSS/anonymous/file-backed memory every 100 ms and record Linux VmHWM.
Record process write_bytes and completed chainstate writes before shutdown.
Then restart and compare block/header heights, best block, chainwork, UTXO
count/best block and verifychain. Reopened-process VmHWM is recorded separately.
Timing includes the Python block generator/P2P fixture and the final UTXO scan;
it is not isolated validation CPU time. RPC samples after each P2P barrier do
not measure worst-case RPC latency during a synchronous write.

One million blocks per process, baseline/candidate/candidate/baseline order,
two runs per binary on a Ryzen 9 5950X Linux host. Builds and the sanitizer
suite finished first. The operating IBD continued throughout; a private
four-node DB-crash test overlapped the first three runs, ending just before
the final baseline. This is not an idle-host or slow-storage comparison.
Every run passed restart and UTXO checks with the same one-million-output
UTXO tip. Values below are medians; each restarted process has its own HWM.

| Metric | Baseline | PERIODIC count trigger | Change |
|---|---:|---:|---:|
| Peak RSS through acceptance and final sync/scan | 727.762 MiB | 479.461 MiB | -34.12% |
| Higher peak including restarted process | 727.762 MiB | 479.461 MiB | -34.12% |
| RSS after acceptance, before final sync/scan | 442.625 MiB | 451.961 MiB | +2.11% |
| RSS after final sync/scan | 676.289 MiB | 479.461 MiB | -29.10% |
| Restarted-process peak RSS | 395.869 MiB | 308.887 MiB | -21.97% |
| Block generation/send/acceptance time | 147.356 s | 147.886 s | +0.36% |
| Final sync plus UTXO scan | 3.461 s | 0.281 s | -91.89% |
| Acceptance plus final sync/scan time | 150.817 s | 148.167 s | -1.76% |
| Process write_bytes during the measured window | 703.904 MiB | 1472.711 MiB | +109.22% |
| Logged chainstate writes before shutdown | 1 | 31 | 30 additional writes |

Baseline peak values were 727.758/727.766 MiB and candidate values
482.848/476.074 MiB. Total times were 151.217/150.417 versus
147.448/148.886 seconds. Acceptance-only ranges overlap; the final-sync
reduction reflects work moved into earlier writes, not eliminated work.
The modest total-time difference is observational and does not establish a
mainnet speedup. Restart time is not included in that time total.

Both candidate runs performed 30 count-triggered PERIODIC writes and one
final FORCE_SYNC; no periodic-timer or LARGE/CRITICAL pressure write replaced
the tested trigger. The baseline deferred all of that persistence to its
single final FORCE_SYNC. Earlier DB activity raises intermediate RSS slightly
and roughly doubles observed write_bytes. This counter excludes work after
the measurement window and is not a physical-device lifetime-write total.
Real transactions that create and spend coins between former flushes may
add other write costs. Accept the change for lower observed peak memory,
not a promised speed gain or unchanged disk workload. No swap was observed
in the sampled private nodes. One candidate run's maximum sampled RPC took
12.316 ms, versus 1.095 ms in the other candidate and 1.330/1.352 ms in the
baseline runs. The limited, post-barrier samples do not establish improved
responsiveness or bound stalls during writes.

### Other reassessment candidates

Two-million-entry production-map helper screening retained the same
CBlockIndex, standard map, hasher and node pool. Natural growth was compared
with local 1.25x/1.5x capacity reservations, twice each in reversed order.
All keys, heights, key/parent pointers, misses and duplicates were checked.
The helper models allocation and lookup, not validation or DB persistence.

| Policy | Peak RSS (MiB) | RSS reduction | Insertion (s) | Insertion cost | Full lookup/check (s) |
|---|---:|---:|---:|---:|---:|
| Existing natural growth | 391.836 | — | 0.887428 | — | 0.424384 |
| Local 1.25x growth | 385.801 | 1.54% | 1.411485 | +59.05% | 0.489121 |
| Local 1.5x growth | 388.494 | 0.85% | 1.020671 | +15.01% | 0.443252 |

Reject both: the small observed saving does not justify additional rehash
and lookup costs. These short shared-host experiments do not predict the
44.9-million-entry result. Keep the disk-load reservation policy unchanged.

Proof-cache lifetime and UTXO/allocator changes were audited but not adopted.
Skipping proof reinsertion does not release already allocated cache pages;
removing them safely requires accounting for presync, replay, multiple peers
and genuine evidence reuse. Existing smaller-cache/page experiments show
recomputation or allocation-overhead costs. There is no new evidence for an
unconditional cache reset, Sync-to-Flush substitution or malloc_trim policy.
Retain the earlier events 1–4 improvement and its documented filled-cache
cost. Do not count retained changes as newly achieved savings.

### Validation

Normal daemon/GUI builds passed. The selected 65 unit cases passed all
1,180,524 assertions in both the normal build and ASan/UBSan with leak
detection and halt-on-error. The complete 813-case suite was not run.
`feature_fast_startup.py`, `feature_abortnode.py` and `rpc_invalidateblock.py`
passed. The existing four-node `feature_dbcrash.py` completed all 40 rounds:
21 crash-driven restarts across its three fault nodes and another 30 crashes
during recovery, with final UTXO hashes matching. That test injects chainstate
batch crashes; the index-WAL sync failure described below is a separate test.

The existing multi-block activation unit test is strengthened with a real
disk-backed strict-mode fixture: just below/at/above the dirty threshold,
unchanged NONE/IF_NEEDED behavior, unchanged mock time, normal cache retention,
DB reopen and block/undo metadata/readback. The existing coin-cache threshold
test now combines LARGE coins with index pressure and checks that eviction
still occurs. The existing fast-startup functional test sends full blocks
without header announcements in both startup modes, kills only its private
node after a completed boundary, checks the durable prefix/UTXO and missing
suffix, then reaccepts the suffix. Its headers-only crash test is retained.

A separate private mode-zero fault experiment injects one EIO at the
count-triggered block-index WAL `fdatasync`, using a wrapper scoped to that
test's exact index directory and an armed marker. At the 32,768th new full
block the log reports `PERIODIC ... periodic=0, index=1`, followed by
`System error while flushing: Fatal LevelDB error ... Input/output error`.
The node exits itself with code 1. Removing the marker permits normal
restart: the complete WAL batch is recovered, the older durable UTXO tip
is advanced through normal activation, and full-chain `verifychain(4, 0)`
passes. After another block and restart, UTXO MuHash, count, total amount and
best block match. No DB/WAL bytes are manually changed. This tests one sync
error and process recovery, not power-loss durability, partial writes or a
permanent device failure.

No operating node or data was restarted or changed. A new complete mainnet
IBD peak and duration, slow-device costs, TSan and Windows/macOS remain
unmeasured. None of these percentages can be added to earlier results or
applied to the old 22.691 GiB peak. This is memory-management work to split
from Lazy PoW before merging PR #11.


## Additional methods investigated after the reassessment

[Additional peak-RSS experiments](https://github.com/decryp2kanon/sugarchain-komorebi/blob/90faec5afbe9a17cf0a04f4128d2e1576ef3b1bd/doc/ibd-rss-new-methods.md) compare higher map
load factors, packing immutable proof-cache tiers, and block-index native
pread with the already optimized baseline. Header-stage RSS improved in the
pread experiment, but the complete full-block fixture peak did not. None of
these new candidates was adopted; their distinct benefits and costs are
recorded without adding them to earlier savings.
