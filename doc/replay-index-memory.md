# Header-replay index memory

## Scope and implementation

Newly accepted headers create a persistent in-memory block-map index and a
dirty-index set entry until index persistence. Both counts can grow during
headers-first replay. The observed linear RSS growth is distinct from proof
cache tier allocation bursts and later DB serialization/memtable peaks. It
is not evidence that all index memory is unnecessary.

On the measured x86-64 build, CBlockIndex is 152 bytes, its keyed pair 184
bytes and the pooled hash-map node 192 bytes. Preserving all fields, types
and their alignment leaves no smaller CBlockIndex obtainable by reordering
alone; no field, genuine proof flag or serialization was changed.

The dirty container remains a standard ordered unique set. Its polymorphic
allocator forwards to the existing PoolResource<64, 8>, using small-node
pooling rather than a separate heap allocation for each entry. The adapter
resource remains at a stable address. After all set nodes are erased during
WriteBlockIndexDB, its owned pool is released and recreated lazily on the
next allocation. Destroy the set before its resource. Calls remain protected
by the existing main lock. Node sizes/alignments not served by the pool use
its existing safe fallback. No new index storage or DB record format is used. The first allocation reserves
a 256 KiB pool chunk; small dirty sets may have extra unused capacity.

The saved vector holds block-index pointers, not dirty-set nodes. Existing
block-file flush order, batch atomicity, scheduling and evidence writes are
unchanged. Releasing the pool returns storage to the allocator; it does not
guarantee the system allocator immediately returns every page to the OS.

## Isolated component measurements

A helper used the production FastBlockMap, CBlockIndex and pool implementation,
with synthetic keys never admitted as genuine verification results. It
compared the original std::set and the exact proposed pooled resource/set,
including duplicate semantics, index pointers, natural bucket growth, pointer
transfer/erasure and subsequent reuse. Two runs per count in forward/reverse
order; medians below. Host operating IBD and builds were concurrent, so timing
changes are observations, not established whole-node speedups.

| Index count | Original filled RSS (MiB) | Pooled filled RSS (MiB) | Reduction |
|---:|---:|---:|---:|
| 1,000,000 | 246.340 | 238.740 | 7.600 MiB / 3.09% |
| 10,000,000 | 2387.756 | 2312.027 | 75.729 MiB / 3.17% |
| 40,000,000 | 9545.430 | 9240.645 | 304.785 MiB / 3.19% |

| Index count | Original insertion (s) | Pooled insertion (s) | Original pointer transfer (s) | Pooled pointer transfer (s) |
|---:|---:|---:|---:|---:|
| 1,000,000 | 0.832 | 0.760 | 0.0250 | 0.0195 |
| 10,000,000 | 9.611 | 8.983 | 0.2657 | 0.1945 |
| 40,000,000 | 47.072 | 44.978 | 1.0815 | 0.8386 |

At 40 million, insertion was about 2.095 seconds shorter (4.45%) and pointer
transfer about 0.243 seconds shorter (22.45%). Peak RSS decreased from
9850.680 to 9545.770 MiB (304.910 MiB, 3.10%). After dropping the temporary
vector, RSS remained about 306.670 MiB lower; this does not mean all dirty-set
physical pages disappeared. The helper does not hash genuine Yespower or
serialize a full production DB batch. No measured whole-IBD timing or a cure
for the separate large DB-write peak follows.

A standard unsynchronized PMR pool was screened at one million entries but
used slightly more memory than the original allocation on this library. It
was rejected in favor of the already-used exact-size pool implementation.
No hash-map reservation or load-factor change was adopted.

## PR scope

This independent memory-management change is temporarily developed in PR #11.
Before merging, separate Lazy PoW/persistent evidence from memory/CPU
management into two PRs. The operating node and DB are not restarted or
changed by these isolated measurements.

## Validation

GUI/daemon builds and the existing fast-startup functional test passed. The
selected normal block-manager, chain, chainstate-write, allocator and header-PoW
suites passed 53 cases with 7,477,906 assertions. The existing block-storage
test was strengthened with duplicate registrations, two flush/reuse cycles,
empty-pool release, DB reopen and persisted hash/height/status/parent checks.
Direct registration does not create genuine Yespower evidence.

A production ASan/UBSan build passed 19 selected block-manager, allocator and
chainstate-write cases with 574,461 assertions, including the strengthened
persistence case. Leak detection was enabled with no reported error. This is
not a sanitizer run of all 813 cases. TSan and Windows/macOS builds were not
performed.
