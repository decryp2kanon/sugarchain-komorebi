# Large block-index write memory

## Limited mitigation, not a full peak-memory fix

The observed large first index write had an approximately 7 GiB rise followed
by a roughly 22.69 GiB process peak. Code review shows a large serialized
WriteBatch and its LevelDB memtable can coexist: DBImpl writes the entire
batch to the WAL and inserts it into the current memtable before another
MakeRoomForWrite call. A buffer budget is not a per-batch insertion limit.
No allocation trace establishes the exact attribution of the observed peak.

This change releases only the temporary block-index pointer vector after
serialization and before DB insertion. PopulateBatch retains the original
record construction, order and genuine-proof checks. The consuming overload
takes an rvalue reference; production passes its temporary via std::move.
The existing const-reference overload remains non-copying and does not
consume caller storage. The owned batch contains copied serialized bytes,
not references to vector elements. Index objects remain in the block map.
One batch, synchronous write, WAL/sequence, file metadata and evidence format
are unchanged. LevelDB itself has no adopted modification.

## Screened alternative

A private batch-reservation prototype was compared twice per variant. At
four million paired synthetic records, batch construction fell from about
0.903 to 0.443 seconds, but write-phase peak RSS was 1729.29 versus 1729.53
MiB: no saving. Unused reserved string capacity is not necessarily resident,
and the memtable dominates the peak. This prototype was rejected and its
source changes restored.

## Measurement boundaries

An isolated helper used the production LevelDB library, one synchronous batch,
33-byte keys, 90-byte block-like values and 33-byte evidence-like values.
These synthetic records were not node verification evidence. An 8-byte pointer
array model compared retaining it through Write versus releasing it before
Write. Records were scanned after DB reopen and matched their expected values.
This models allocation overlap, not actual node header verification, block-file
flushes or complete compaction behavior.

Write-phase peak is sampled before DB close/reopen; a separate process-global
peak includes recovery. The latter did not show a meaningful reduction in the
completed small comparisons: early vector release is not a fix for recovery
batch memory. Do not present the write-phase percentage as whole-node peak
improvement. Operating IBD and builds shared the host; timing differences
are not controlled full-IBD measurements.

## Remaining work and PR scope

Large batch/memtable overlap remains. Plain chunking is not adopted: it changes
all-index atomicity and requires explicit analysis of parent availability,
block-file metadata, chainstate ordering, partial failures and crash recovery.
A bounded transactional persistence design needs its own proposal and tests.
This independent partial memory-management change belongs to the future
memory/CPU-management PR, separate from Lazy PoW/persistent evidence. PR #11
remains temporarily combined; no operating restart or DB modification occurs.

## Repeated results

Two processes per variant and count in forward/reverse order; median values.

| Inputs | Retained-vector write peak (MiB) | Early-release write peak (MiB) | Reduction |
|---:|---:|---:|---:|
| 1,000,000 | 442.781 | 435.371 | 7.410 MiB / 1.67% |
| 4,000,000 | 1760.039 | 1729.758 | 30.281 MiB / 1.72% |
| 12,000,000 | 5273.068 | 5182.127 | 90.941 MiB / 1.72% |

| Inputs | Retained-vector build/write (s) | Early-release build/write (s) |
|---:|---:|---:|
| 1,000,000 | 0.245 / 2.191 | 0.243 / 2.293 |
| 4,000,000 | 1.007 / 13.371 | 1.000 / 13.280 |
| 12,000,000 | 3.275 / 53.242 | 3.352 / 55.763 |

Timing changes were mixed and host-load sensitive; no IBD speedup is claimed.
A 37,462,782-entry pointer array occupies approximately 285.8 MiB at eight
bytes per pointer. That is an arithmetic scale reference, not a measured
whole-node RSS reduction or proof that the observed dirty count matched it.

## Validation

Normal block-manager, DB-wrapper, header-PoW and chainstate-write suites passed
59 cases and 758,991 assertions. The existing durable-reopen test now exercises
both unchanged borrowed-vector calls and consuming calls, persisted header
identity, no false proof evidence, actual audit/reuse and durable reopen.
Production ASan/UBSan with leak detection passed 57 selected cases and 758,985
assertions; not all 813 tests. GUI/daemon builds and fast-startup functional
tests passed. TSan and Windows/macOS builds were not run. The operating node
and database were not restarted or modified.
