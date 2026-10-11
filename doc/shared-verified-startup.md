# Historical 13-item full-verification screening

This records the 13-item baseline committed as `7d4a5207f0`. The newer
[all-19 candidate](all-verified-startup.md) supersedes its selection policy,
not its measured results.

Mode zero retains historical Yespower verification. Disk trust stays controlled
only by `-fast-startup=1`; a trusted disk status is never live proof evidence.
No database format, consensus rule, operating datadir or process was changed.

**Adopted for mode zero: 13 of the original 19 items.** Six remain fast-only.
Some items eliminate allocations/traversals by construction rather than showing
an independently isolated whole-node timing win. These categories are explicit
below; this is not a claim that 19 independent performance tests passed.

The final 6,000-block mainnet fixture comparison used three runs per version,
fresh private database copies, full PoW checking, identical cache/worker settings,
no network/wallet, and no concurrent builds or tests. Median RPC readiness:
**3.767 → 3.001 seconds**; peak RSS **2,231,340 → 137,156 KiB**.
All tips, heights and chainwork match; `verifychain(3,100)` and normal exits pass.
The large RSS saving chiefly reflects on-demand allocation with a 2 GiB configured
PoW-cache budget; it is not a prediction for a fully populated cache.

| Original no. | Optimization | Mode-zero decision | Evidence / limits |
|---:|---|---|---|
| 1 | Shared versionbits counts | Share | 0.270445 → 0.015665 s for a one-million-index cold warning calculation; separate from startup readiness. |
| 2 | Growing positive-proof cache | Share | Full-verification sample peak RSS 2,231,340 → 137,156 KiB; same configured maximum. Long full-chain cache/IBD throughput not measured. |
| 3 | Standard-map pool allocator | Share | One-million-entry pool-only screening: median insertion 0.371 → 0.301 s and peak 217,208 → 209,696 KiB. |
| 4 | Reuse sorted index pointers | Share | Removes a second pointer collection and sort; confirmed shared startup stage logs and restart results. No isolated seconds claimed. |
| 5 | Exact compact-target work formula | Share | 0.247082 → 0.016529 s per million calculations; exhaustive/random compact-target equivalence regression retained. |
| 6 | Bound/reclaim DB cache | Keep fast-only | No mode-zero full-verification benefit established; retain original DB cache policy. |
| 7 | Reduce DB read cache 64 → 16 MiB | Keep fast-only | Historical steps of the same cache-policy experiment; not separate additive savings. |
| 8 | Reduce DB read cache 16 → 0 MiB | Keep fast-only | No mode-zero I/O benefit established. |
| 9 | Reserve 1.5x map capacity | Share | Reserve-only insertion plus reservation ≈0.229 s versus incremental insertion 0.371 s at one million entries; uses slightly more buckets at this size. |
| 10 | Remove map-local cached hash | Share | Hash-only successful lookup 0.178 → 0.092 s per million; hash value and key equality unchanged. Individual peak RSS nearly unchanged at this allocator size. |
| 11 | Height-sort workspace / radix passes | Share | Actual sorter 0.172289 → 0.039266 s per million unique heights. Existing signed/duplicate/order regressions retained. |
| 12 | Direct DB value decode | Share | 200000 decodes: 0.021475 → 0.014569 s. Obfuscated values retain the copying fallback; decoding-error regressions pass. |
| 13 | Collect file references while linking | Share | Avoids a separate full index traversal; same file-presence checks. Structural benefit, not isolated time measurement. |
| 14 | Collect pointers while inserting | Share | Avoids the post-load full-map collection; preserves reload/parent placeholders. Structural benefit. |
| 15 | Traverse candidates directly | Share | Avoids a temporary full pointer vector (8 bytes per entry on this build). Restart/reorg regression covers resulting candidates; no isolated seconds claimed. |
| 16 | Link/header prefetch | Keep fast-only | Individual mode-zero gain not established in the time budget. |
| 17 | Chain/sequence-ID prefetch | Keep fast-only | SetTip screening regressed 0.009668 → 0.015424 s; does not prove regression for every hardware/layout, so do not force into mode zero. |
| 18 | Witness-check prefetch | Keep fast-only | Individual mode-zero gain not established; keep all original witness checks. |
| 19 | Avoid unused DB key decode | Share | 200000 key checks: 0.011593 → 0.006895 s; equivalent minimum-size/prefix boundary retained. |

Metadata component results use actual production helpers on separate synthetic
index objects; they do not bypass PoW in the node or establish full-mainnet startup
times. The negative prefetch screening uses a contiguous layout, so it is an
exclusion signal rather than proof of a universal slowdown. Cache policies and
read-ahead are the documented remaining differences between the two modes.

Existing safety tests were strengthened instead of adding a parallel smoke suite:
2,001 cold genuine Yespower proofs with persisted TREE status are all recomputed
under both algorithms and worker counts 1/3/8. Existing invalid-proof, corruption,
failure, cancellation, target, proof-promotion, cache reset and decoding tests remain.
Functional mode changes retain every tested active-chain hash, best header and work.

Complete 44-million-header mode-zero startup and sustained mainnet IBD were
**not** run. The 30-minute budget cannot establish every hardware/data-dependent
long-run result. [Sanitized metrics](benchmarks/shared-verified-startup-20261011.json)
contain no local filesystem paths, RPC credentials or private logs.

## Validation of the selected source

- Release daemon, Qt executable and unit-test builds succeed.
- Relevant release suites: 69 cases in each default and explicit fast mode.
- ASan/UBSan with leak detection: the same 69 relevant cases pass. Fixtures
  independently exercise disk-trust true/false and reference/optimized helpers.
- Existing startup-mode and invalidate/reconsider functionals pass, including
  default/0/1 switching, missing-file rejection, reindex-chainstate, active-chain
  hashes, header-only best header, chainwork and verification after restart.
- Sanitized invalidate/reconsider functional passes; this is targeted coverage, not a full-suite/fuzz/OOM guarantee.

Only the dedicated test builds were updated. The running node was not restarted;
these measurements do not mean its running executable has adopted the change.
