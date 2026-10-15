# Internal IBD block-download lookahead

## Current behavior: no new user option

IBD uses an internal 4096-block lookahead. **There is no `-maxibddownloadwindow` option.** The earlier opt-in implementation and startup instructions are superseded. Remove that argument if it was copied into a startup command. Rebuild/restart is required to run the new implementation; this work did not restart or reconfigure the operating node.

Existing resource/concurrency controls keep their meanings and defaults. The comparison fixed the user's settings:

```text
-parpow=8 -maxpowcache=2048 -maxibdblocksinflight=128 -dbcache=4096
```

No additional tuning option is needed. These controls are not a hard cap on whole-process RSS/CPU. The 4096 value is height lookahead beyond the peer's last common linked block, not a global request count or byte limit. Per-peer inflight remains independent and capped at 128; outside IBD the existing 1024-block lookahead remains unchanged. Faster peers can continue useful requests while earlier blocks from slower peers are outstanding.

PoW, chainwork, proof-failure bounds, evidence reuse, DB formats and durability semantics are unchanged. This does not directly accelerate presync or Yespower computation.

## No-option comparison

Both measured binaries removed the option, differing only in the internal 1024/4096 constant. Four conditions each used baseline/candidate/candidate/baseline order, fresh data folders and identical fixture hashes/final tips (16 measured runs). Preparation preceded acceptance timing; the table includes acceptance plus final sync/UTXO scan. There were only two observations per arm, so percentages are descriptive, not statistical confidence intervals.

| Condition | Time, 1024 → 4096 | Shorter time | Mean lifetime/restart peak RSS | RSS change | Process CPU time change | Process write-byte change |
|---|---:|---:|---:|---:|---:|---:|
| Small blocks, uniform 100 ms | 4.702 → 4.199 s | 10.71% | 97.21 → 97.77 MiB | +0.56 MiB (+0.57%) | -9.58% | +0.000% |
| Small blocks, uneven delays | 39.768 → 11.878 s | 70.13% | 97.50 → 99.61 MiB | +2.11 MiB (+2.17%) | -8.36% | +1.305% |
| 64 KiB padding, uneven delays | 8.390 → 2.972 s | 64.57% | 86.75 → 92.56 MiB | +5.81 MiB (+6.70%) | -12.38% | -0.002% |
| 64 KiB padding, delayed first response batch | 3.785 → 3.328 s | 12.09% | 100.07 → 104.53 MiB | +4.46 MiB (+4.46%) | -2.01% | +0.000% |

The delayed-first-batch condition had overlapping time ranges (baseline 3.336–4.235 s, candidate 3.204–3.451 s), so its mean 12.09% reduction is not a consistent per-run speedup. RSS increases were observed: the result is **not zero memory cost**. In the padded uneven condition, baseline peaks were 84.58–88.93 MiB versus 90.64–94.48 MiB, a mean 5.81 MiB increase. The largest mean end-of-sync RSS increase was 11.92 MiB in that condition. Neither value is a general upper bound.

Small conditions used 40,000 blocks. Padded conditions used 8192 blocks with 65,536 bytes of valid OP_RETURN coinbase output padding (about 512 MiB of payload). Padding stresses transport/storage, not representative transaction script verification or spendable UTXO growth. Uneven scheduled response delays were 50,70,100,150,300,500,800,1000 ms. The front-delay condition used 100 ms generally and 1500 ms for the response batch containing the first block. No unsolicited blocks or minimum-chainwork overrides were used. All eight responders remained connected at completion.

The node was observed using getblockcount every 100 ms and getpeerinfo every second, plus Linux process counters. Eight distinct physical-core affinity slots and a 10,000,000,000-byte cgroup memory fence (kernel rounded) applied only to disposable test processes. There was no CPU quota. No OOM occurred. The operating node continued running, so this is a controlled same-host comparison, not an otherwise idle host guarantee. No builds/heavy tests overlapped timing.

**Decision:** adopt internal 4096 without another user setting. Repeated reductions in the uneven-delivery cases and modest absolute measured memory costs support this choice. This is an engineering judgment under the tested conditions, not proof of unchanged mainnet memory usage. Larger out-of-order work can affect storage layout, pruning and reindexing. Full mainnet/HDD/pruning/reindex results and whole-IBD time remain unmeasured.

## Safety checks and build

All 16 runs passed final height/tip, UTXO height/bestblock, verifychain(3,100), normal shutdown and restart checks. A 1024-block padded smoke passed after correcting an initial helper-only use of an obsolete transaction rehash API; the failed smoke was retained and excluded from timing results. A later portable-helper launch initially used a relative path under systemd and failed before starting a node; rerunning with an absolute path passed.

Peer-manager unit tests passed (4 cases). Functional stalling/backoff/recovery passed at the default 16-per-peer budget and explicit 128 budget on v1/v2 transports, all exercising the internal 4096 boundary. The redundant option-specific functional wrapper and option-parsing unit case were removed. Malformed/mutated/duplicate/inflation/future-timestamp rejection passed; that test does not hold IBD true throughout. Daemon, GUI, CLI and test targets were built. No new sanitizer campaign or full-mainnet rerun was performed.

## Manual reproducer

`contrib/bench/ibd_download.py` uses the four fixed settings above and has no window-selection environment variable. Compare separate 1024/4096 internal-constant builds. Run from the repository root, set a fresh result path/tmpdir for every run and apply external resource controls (the helper does not enforce them).

```sh
ibd_bench_root=$(mktemp -d)
PROBE_RESULT="$ibd_bench_root/result.json" \
PROBE_COUNT=8192 PROBE_PEERS=8 PROBE_PADDING=65536 \
PROBE_DELAYS_MS=50,70,100,150,300,500,800,1000 \
BITCOIND="$PWD/build/bin/sugarchaind" \
BITCOINCLI="$PWD/build/bin/sugarchain-cli" \
python3 contrib/bench/ibd_download.py \
  --configfile="$PWD/build/test/config.ini" \
  --tmpdir="$ibd_bench_root/node-run" --nocleanup
```

For the front-delay condition set PROBE_DELAYS_MS=100 and PROBE_FIRST_DELAY_MS=1500. The helper has a 180-second acceptance timeout. Child RSS assumes the daemon is the only sizable child. Its process I/O counters end after final sync/check, before shutdown; they are not total physical-device writes. SHA256d regtest does not reproduce mainnet Yespower costs.

Measured no-option daemon hashes:

- daemon1024: `27ea3ee2818d61c7f69827523369091353d29146b41f02e21117a7c16f32e2a9`
- daemon4096: `b77fff820c30cd261d8d135d21d923d24666aabd4525db27514cbabd465ec9fc`

Final builds may differ in comments/version metadata; measured binaries and private raw results are retained separately. Fixture payload hashes and tips are identical within each condition.

## Earlier investigation and limits

The previous opt-in 100k uneven-delay test showed 72.93% shorter acceptance-plus-sync time; the earlier small-fixture measurements remain historical evidence, not current option instructions. See the prior revision of this document for that protocol. The no-option matrix above is the basis for the current default decision.

The old 21h00m14s graph marks IBD-flag exit sampling, not exact tip catchup; first equal heights were observed at 21h00m49s. The post-header interval was roughly 10h21m13s. Applying laboratory percentages directly to those hours is not a validated mainnet prediction.

The earlier bounded mainnet presync probe and read-only operating-memory inspection did not establish an additional presync/RSS optimization. No such change is adopted here. PR11 still needs later separation of Lazy PoW, RSS management and presync performance; classify block-download performance separately before that split. No split or merge was performed.
