# Optional IBD block-download lookahead

## Behavior and scope

`-maxibddownloadwindow=<n>` selects block-height lookahead beyond each peer's last common linked block during IBD. The default remains 1024; values are clamped to 1024–4096. A value of 4096 allows useful later requests while slower earlier responses are outstanding. It is neither a global request count nor a byte/RAM limit. Outside IBD, the existing 1024-block window remains unchanged. Per-peer inflight limits are independent and remain capped at 128.

For a subsequent launch, add these options to the user's existing command:

```text
-maxibddownloadwindow=4096 -maxibdblocksinflight=128
```

The operating process was not restarted or reconfigured. The new behavior is opt-in: rebuilding alone does not enable it. PoW, chainwork, proof-failure bounds, evidence reuse, DB formats, final synchronization and durability semantics are unchanged.

## Controlled measurements

Fresh Linux regtest nodes received deterministic, solved, coinbase-only SHA256d blocks from eight normal outbound responders. Headers were accepted and payloads serialized before timing; blocks were delivered only in response to GETDATA. No minimum-chainwork override was used. Each matrix used baseline/candidate/candidate/baseline order, with no overlapping builds or heavy tests. Delays are scheduled response delays, not measured Internet RTT or bandwidth. The main comparisons used the actual public option, not private instrumentation.

| Workload | Default 1024 | Option 4096 | Shorter duration |
|---|---:|---:|---:|
| 100,000 blocks, uneven response delays, acceptance plus final sync/UTXO scan | 98.768 s | 26.735 s | 72.93% |
| 40,000 blocks, uneven delays, reduced observation frequency | 39.917 s | 10.966 s | 72.53% |
| 40,000 blocks, uniform 100 ms delay, reduced observation frequency | 4.936 s | 4.216 s | 14.57% |

Uneven delays were 50, 70, 100, 150, 300, 500, 800 and 1000 ms. Reduced observation used getblockcount every 100 ms and getpeerinfo every second; the 100k matrix polled every 20 ms. There are two observations per arm, not a confidence interval. Earlier constant-window prototype 40k ABBA matrices independently showed 72.84% and 72.57% shorter acceptance time.

For the 100k actual-option matrix:

- Acceptance alone: 98.723 → 26.678 seconds.
- Process write_bytes through final sync: 61,065,216 → 61,997,056 bytes, **1.53% higher**. These are process counters, not device lifetime writes.
- Final sync/UTXO scan: 44.74 → 56.22 ms, approximately **11.48 ms extra**.
- Mean lifetime/restart peak RSS: 144.49 → 142.55 MiB, with overlapping ranges. **No RSS reduction is established.** Reduced-observation controls also showed no consistent RSS saving.
- Restart duration: approximately 0.504 seconds in both arms.

Small warm-cache regtest measurements do not establish full-mainnet, HDD, pruning or reindex performance. Larger out-of-order lookahead can increase buffered/storage work. Do not apply the 72.93% figure to the previous 21-hour IBD, and do not claim reduced proof computation from observer-sensitive process CPU counters.

## Validation and provenance

Peer-manager unit tests passed (5 cases, 475 assertions). Functional tests passed for default inflight, 128 inflight on v1/v2, and 4096 lookahead with 128 inflight on v1/v2, including stalling recovery. A malformed/mutated/duplicate/inflation/future-timestamp block rejection test also passed with the option configured; that test does not hold IBD true throughout. Final heights, tips, UTXO height/bestblock, verifychain(3,100), and restart checks passed in the benchmark matrices. A portable 4000-block reproducer smoke passed. Daemon, GUI, CLI and unit-test builds succeeded. No full mainnet rerun or new sanitizer campaign was performed.

Baseline source: `5866c6540acfa1b0dbdad3c77d577f8c205ff5ec`.
Feature commit: `4d3b0b9be7`; functional coverage: `308889f6e6`.
Measured daemon SHA256 values:

- Baseline: `f89cc5a901440a6fefaea512b21b8693f7dd30cb4f3fdcd17d93a88410c1197c`
- Candidate: `2bdeefd696a9421ae14f99e76518fb0a7e4c78fedeed2af342fea1b8a9b22dc1`

The final normal build refreshes version metadata; the measured binary was preserved separately. Measurements used eight distinct physical-core affinity slots and a 10,000,000,000-byte memory fence (kernel-rounded). No CPU quota is claimed.

## Manual reproducer

Run from the repository root, using fresh output directories and external resource controls. The helper itself does not enforce a resource budget. It has a 180-second measured-interval timeout; slow machines or larger fixtures may exceed it.

```sh
ibd_bench_root=$(mktemp -d)
PROBE_RESULT="$ibd_bench_root/result.json" \
PROBE_COUNT=40000 PROBE_PEERS=8 \
PROBE_DELAYS_MS=50,70,100,150,300,500,800,1000 \
PROBE_WINDOW=4096 \
BITCOIND="$PWD/build/bin/sugarchaind" \
BITCOINCLI="$PWD/build/bin/sugarchain-cli" \
python3 contrib/bench/ibd_download.py \
  --configfile="$PWD/build/test/config.ini" \
  --tmpdir="$ibd_bench_root/node-run" --nocleanup
```

Repeat with windows 1024, 4096, 4096, 1024 and compare identical fixture hashes and final tips. Payload preparation is outside the acceptance timer; reported acceptance-plus-sync adds the final UTXO scan/sync duration, while verifychain and shutdown are checked separately. Lifetime child RSS assumes the daemon is the only sizable child. The Linux helper uses repository test-framework internals.

## Separate observations and remaining work

The prior graph's 21h00m14s marks IBD-flag exit sampling, not exact tip catchup. First observed equal block/header heights were at 21h00m49s. The post-header interval was approximately 10h21m13s. Historical logs cannot reconstruct all supply, CPU or storage waits.

A bounded fresh mainnet presync probe reached 20k leading headers with three existing outbound peers: completed-handler span 15.418 s, handler sum 9.568 s, and between-handler intervals 5.850 s. Nearly all of those gaps preceded next-message receipt in this sample. This is not a decomposition of the historical seven-hour presync or evidence for a three-hour target. No further presync change was adopted.

Read-only operating-node observations found a 152-byte CBlockIndex layout (approximately 6.36 GiB for 44.9 million objects before maps/caches), roughly 11.59 GiB anonymous RSS, and about 118.8 MiB allocator-visible free space. This does not establish a leak or multiple GiB reclaimable through trimming. No additional RSS optimization was adopted.

PR11 still requires later separation of Lazy PoW, RSS management and presync performance. This block-download change is a distinct performance sub-scope to classify before that split. No split or merge was performed here.
