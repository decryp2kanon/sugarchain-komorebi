# Lazy PoW Validation in the Background

A fast startup may trust existing block indexes before recomputing their historical
Yespower proofs. This change audits those headers after the node becomes ready and
persists genuine successful results for subsequent audits. It does not turn disk
trust into proof and does not change validation of new network or imported headers.

## Modes and defaults

* `-fast-startup=0` remains the default. Startup checks every historical header's
  PoW as before. Saved evidence does not skip that check. After startup, the worker
  can persist current-process verified results without calculating them again.
* `-fast-startup=1` preserves the fast startup path. A background worker checks
  headers without matching evidence; matching evidence is reused on later runs.
  There is a historical-trust window until the scan completes.
* `-background-pow-workers=<n>` selects 1–8 background workers, defaulting to
  `-parpow` (normally 8). Missing proofs use the existing
  `HeaderPoWVerifier::CheckEach()` queue; a separate background pool leaves the
  foreground validation pool intact. Total concurrency can include both pools.
  Lower the background count if CPU or memory contention affects operation.
* `-background-pow-pause=<ms>` now pauses **between computed DB batches**, not
  between individual headers. The range remains 0–1000; the default is now zero.
  Explicit configurations using the old option need review: 10 ms per batch is
  much less throttling than the previous 10 ms per header. Pure evidence reuse
  does not sleep. Removing the old default eliminates about 5.2 days of deliberate
  per-header waits for a cold 44.9-million-header migration, not the actual work.

Background checks may reuse genuine process-local cache hits, but do not insert newly verified historical headers into the foreground RAM cache. Their successful results are persisted as disk evidence instead. This avoids growing the foreground cache solely because the background scan traverses millions of old headers; ordinary startup and live-header caching remain unchanged. Other allocations and DB activity can still increase RSS.

The worker starts after `Done loading`. Migration does not synchronously verify all
historical headers before readiness. Full-mainnet startup time and steady-state
resource interference must still be measured; a small fixture cannot establish
that a 44-million-header migration finishes quickly.

## Evidence and trust boundary

Evidence is stored in the existing block-index LevelDB, keyed by the header hash
under the `y` namespace. Version 2 records bind the exact serialized header to a
rules digest containing genesis, PoW limit, algorithm version, N/R and
personalization. Different forks have different header identities. The existing
`CDiskBlockIndex` serialization is unchanged.

Only genuine verification in the current process creates evidence. Merely loading
an index with a trusted validation status does not. Reused disk evidence never sets
`m_checked_yespower`, which continues to mean current-process verification.
Normal index flushes atomically save genuine live evidence with their index update;
background results use synchronous atomic batches of at most 512 entries.

Absent, unknown-version, mismatched or unreadable evidence is a cache miss and
requires verification. A failed DB iterator/read is a storage error, not successful
completion. The worker copies at most 512 index records and releases each iterator
before verification, avoiding a long-lived LevelDB snapshot. Owned copies of at
most 512 headers remain alive until queue completion; no DB
iterator or live index pointer is held during computation. Failed batches are
resolved with individual genuine checks, and only confirmed successes preceding
the failure are persisted. Exceptions propagate as resource/storage errors.
It takes `cs_main` only briefly to inspect live results. Shutdown interrupts and
joins the worker
before the database is destroyed. Interruption finishes the bounded in-flight
batch before joining, rather than leaving tasks referencing freed headers.
Committed batches survive interruption; an uncommitted batch can be checked again after a crash.

These are **local reusable results, not independently verifiable cryptographic
proofs**. An unkeyed digest binds data and detects mismatches; someone able to
maliciously replace the database can also manufacture matching records. Protect
and back up the database. A completed scan does not verify all block contents,
scripts, chainwork relationships or UTXO state, and is not a substitute for those
checks. `-fast-startup=0` is the way to force historical PoW recomputation.

## Progress and failure handling

`getblockchaininfo` includes `historicalpow`:

* `state`: disabled, running, complete, interrupted or failed.
* `total`, `checked`, `remaining`: estimated totals while running, exact scan count
  when complete. These are scan progress, separate from chain synchronization.
* `verified`: missing-evidence entries backed by genuine current-process
  validation. This is not a count of all foreground startup computations.
* `reused`: entries accepted from matching persistent evidence.
* `historical_trust`: whether fast-mode historical trust remains pending.

Counters are concurrent snapshots. New headers continue through normal validation;
the scan covers stored historical entries, including forks. Progress also appears
in `debug.log` as `Lazy PoW`: start/completion/interruption/errors are
reported separately. Zero progress is always logged immediately; subsequent
progress is logged at most once every 10 seconds, checked after each bounded DB
batch using a monotonic clock. This also limits fast evidence-reuse logging.
Progress includes a two-decimal percentage against the current estimated total;
only the separate completion message establishes scan completion.
SHA256d networks such as regtest do not run this worker.

If actual historical Yespower validation fails, the node reports the height/hash
and requests mandatory shutdown. Storage or
resource errors are reported separately and also stop the node rather than leaving
an incomplete audit silently running.

**Do not continue operating after a failure. Preserve data and logs first.**
Recover using the same data directory with:

```sh
sugarchaind -datadir=/path/to/data -fast-startup=0 -reindex
```

`-reindex` rebuilds the index and removes its evidence. Mode zero checks actual PoW
during recovery. `-fast-startup=1 -reindex` is rejected because rebuilding an index
cannot rely on that same index's prior trusted status. Reindex cannot repair an
invalid block or damaged block file; recovery may require a sound backup or a new
synchronization. `-reindex-chainstate` alone does not rebuild block-index evidence.

## Storage and verification scope

Each logical record has a 33-byte key and a 33-byte value, before LevelDB overhead
and compression. For 44.9 million headers that is approximately 2.8 GiB of raw
key/value payload; actual database growth and compaction overhead are unmeasured.
Old records stay tied to their original header/rules, rather than being promoted
when policy changes. Ordinary whole-datadir backups include evidence.

Regression coverage extends the existing `header_pow_tests` and
`feature_fast_startup.py`: cold migration, warm reuse, rules/header mismatch,
unknown/truncated records, interruption/resume, invalid proof rejection, durable
DB reopen, reindex removal, full-mode revalidation, startup mode boundaries and
option validation. Mainnet-scale completion and real-time node resource impact
are outside these fixtures and must not be inferred from them.

## Parallel revision measurements and limits

A private mainnet fixture with 6,000 public headers plus genesis was cloned for
**each** cold run. Saved `y` records were removed only from those private copies;
each new process started with a cold RAM cache. Every run reported 6,001 genuine
results and zero persisted reuse (the foreground genesis check can already be a
genuine RAM-cache hit). The existing operating node was left running, but builds
and heavy unit suites had finished before the performance comparisons. These are
small, two-run observations on the same host, not controlled full-mainnet results.
Rates include DB work after RPC readiness; polling has about 20 ms resolution.

| Background workers | Mean checked headers/s | Mean process CPU | Maximum peak RSS | RPC readiness range |
|---|---:|---:|---:|---:|
| 1 | 349.0 | 100.5% | 72.35 MiB | 0.183–0.205 s |
| 2 | 683.9 | 196.6% | 80.36 MiB | 0.184–0.184 s |
| 4 | 1,347.1 | 380.1% | 96.45 MiB | 0.183–0.184 s |
| 8 | 2,434.6 | 728.6% | 129.00 MiB | 0.184–0.184 s |

The saved pre-change serial binary, despite `-parpow=8`, processed about 69.3
headers/s with its default per-header 10 ms pause. Final eight-worker processing
was about 35 times faster. Reusing all 6,001 saved records took about 0.042 s after
RPC readiness in a separate warm run; that is **not** a Yespower computation rate.
The initial worker-sized queue groups were also compared with bounded continuous
queue feeding; the latter improved eight-worker throughput from about 2,220 to
about 2,400 headers/s. Larger queue variants had overlapping small-run ranges;
the final queue retains at most `workers * 64` task descriptors (maximum 512).
Startup callers supplying only worker-sized groups preserve their group size.

At approximately 2,435 headers/s, 44,869,074 cold entries would take about
**5 hours 7 minutes** by simple linear extrapolation. **The five-hour target has
not been demonstrated.** Full database size, sustained CPU frequencies, I/O and
compaction, foreground activity and reuse can change the result. The original
full-mode watcher’s first completion was corroborated in `debug.log`: 44,862,137
entries, `-parpow=8`, mode zero, 19,029 seconds (5 h 17 m 9 s). Later appended
watcher records are not additional measurements of that same run.

On these fixtures, cold eight-worker `getblockchaininfo` p95 latency was at most
1.263 ms; normal completion shutdown was 0.064–0.114 s. A separate foreground
comparison submitted 1,000 valid new headers through normal validation: after a
warm audit, it took 4.693 s (p95 6.538 ms); during a cold eight-worker audit, 5.534 s
(p95 6.944 ms), approximately 18% longer total and 6% higher p95. Correctness held,
but operation is **not unaffected**. This is not a full live-peer/transaction or
mainnet block-latency test. Use fewer background workers if contention matters
more than audit completion time.

The final code passed 75 relevant Release cases plus the genesis-reimport case,
and the existing fast-startup functional test with invalid worker/pause options.
The strengthened existing suites cover 1/2/4/8 workers, real computation counts,
concurrency limits, cold/warm reuse, failed-batch evidence handling, local worker
exceptions, interruption and durable reopen. Private process tests accepted 1,000
new valid headers while auditing, rejected an invalid new header, and exited with
status 1 on an invalid historical side-fork proof. A shutdown during a cold audit
retained 512 committed results; restart reused those 512 and verified the remaining
5,489. Existing operating data/processes were not changed or restarted.

Sanitizers and TSan were not rerun for this parallel revision; the earlier
sanitizer results below apply to the prior implementation. Full-mainnet performance
and sustained live-node interference remain unmeasured.

## Earlier serial implementation measurements

Release build, GCC 11.4, no peer connections, public mainnet heights 1–6000 plus
production genesis, `-background-pow-pause=0`. The pre-change PR #10 local build (`9cf2dc5b5b45`) seeded
an isolated datadir through `submitheader`, then stopped. The new binary opened
the same stored history. A sanitizer build was running concurrently, so these
single runs show behavior and approximate cost, not controlled mainnet forecasts.
The baseline predates the final PR #10 progress-display adjustments; the diff to
merged base `f675133722` is limited to progress display/logging and its tests.

| Run | RPC readiness | Time from process launch to audit completion | Audit results |
|---|---:|---:|---|
| PR #10 fast-mode baseline | 0.203 s | No background audit | Existing disk trust |
| First lazy fast-mode run | 0.203 s | 20.393 s | 6,001 genuine verifications, zero reused |
| Next lazy fast-mode run | 0.203 s | 0.254 s | Zero recomputed, 6,001 reused |
| Full mode with saved evidence | 4.526 s | 4.578 s | Startup still recomputes PoW; later audit reuses records |

The final private block-index directory occupied 1,258,263 bytes, including indexes,
WAL and evidence. This is not the incremental evidence size or a peak-disk estimate.
The equivalent 1,025-header in-memory unit fixture also crosses three batch writes;
its actual computation counters distinguish cold validation from warm reuse.

A copied private fixture was given a height-1 side-fork header with an invalid
nonce and no evidence. Fast mode loaded the trusted entry, the background worker
then detected the invalid Yespower proof, printed the recovery command and exited
with status 1. This checks the real process shutdown path, in addition to component
failure/interruption tests. No operating datadir or running node was modified.

For reproducing the migration comparison, build the baseline and candidate into
separate directories, create a **new private mainnet datadir**, and start with
`-server=1 -connect=0 -listen=0 -dnsseed=0 -discover=0 -minimumchainwork=0`.
Submit the 80-byte records in `src/test/data/sugarchain_headers.raw` in order using
`submitheader`, stop the baseline cleanly, then compare candidate cold/warm/full
starts with the same datadir. Measure RPC readiness separately from
`historicalpow.state=complete`. Do not lower chainwork requirements or inject
malformed records into an operating node.

The recovery process test also exposed a pre-existing mainnet reindex assertion:
height-zero CSV activation attempted to access the genesis predecessor. A separate
commit exempts only the exact configured genesis from contextual post-genesis
rules; header PoW and basic block validation remain checked. The isolated recovery
command now rebuilds the genesis-only block file successfully. Header-only entries
are removed by reindex and must be obtained again; this is expected behavior.

A separate run used the default `-background-pow-pause=10` with the same public
header fixture. RPC was ready at 0.202 s; the cold audit completed at 84.532 s.
The process used 21.83 CPU-seconds over that interval, approximately 26% of one
CPU on average (including foreground work). `/proc` peak RSS was 69.6 MiB versus
62.6 MiB for that run's pre-change fast baseline, approximately 7.0 MiB additional.
Warm reuse completed at 0.254 s without a per-header sleep. These are small-fixture,
single-run observations under concurrent compilation, not CPU or memory limits.

When verifier semantics change without parameter changes, maintainers must bump
the evidence format/domain version. An unchanged parameter digest alone cannot
identify a corrected verifier implementation.

## Validation recorded for this change

Release daemon, CLI and unit builds succeeded. The relevant header/PoW/DB/block
manager suites passed 72 cases; block-validation and genesis-reimport coverage
passed four additional cases. Focused migration/live-flush/recovery cases were
rerun after the final lock-contract change and passed. The existing
`feature_fast_startup.py` functional test passed with the private test harness
explicitly loading its `bitcoin.conf` into the renamed Sugarchain daemon.

Before the parallel revision, an independent `-DSANITIZERS=address,undefined` build passed 34 selected
header/evidence, PoW, DB and genesis-reimport cases with ASan leak detection and
UBSan halt-on-error enabled. No sanitizer errors were reported. This is not
exhaustive fuzzing, a thread-sanitizer run or a full-mainnet resource test.

## Historical mainnet completion observed during the split

Read-only inspection of the preserved operating debug log on 2026-10-19 found
one background scan with these boundaries (UTC):

| Event | Timestamp / count |
|---|---|
| Workers and computed-batch pause | 8 workers, 0 ms |
| Start | 2026-10-16 12:40:59; estimated 44,946,269 entries |
| Completion | 2026-10-16 18:05:10; 44,947,883 checked |
| Completion breakdown | 44,946,269 verified; 1,614 reused |
| Log-boundary elapsed | 5h24m11s |

This is a historical run of the combined implementation, not a new benchmark of
the separated branch. The live DB can change during the scan, so the final
checked total need not equal the starting estimate. Only derived timestamps and
counters are published; raw operating logs and DB records remain private.
This confirms one completed background pass, not all startup, tampering, disk
failure, crash, concurrency or foreground-interference cases. Startup latency,
CPU/RSS impact and qualification of the newly separated combinations are separate
questions. Earlier small-fixture results must not be extrapolated to this run.

The earlier 2026-10-15 log completed with zero checked entries after a genesis-only
start. That fresh-IBD observation is not a full historical background audit and
is not counted as another 44.9-million-entry completion.

EOF
