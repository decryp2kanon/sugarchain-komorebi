# Follow-up to the observed 21-hour IBD

Baseline: `5866c6540a`, 2026-10-15. Investigation completed with an opt-in
IBD download-window candidate. See [results and limitations](ibd-download-window.md).
The checkpoint observations below are retained as investigation history.

The first priority is the post-header block interval, approximately 10 h 21 m
of the user's completed run. Average node CPU was about 49.8% of one logical
CPU, with roughly 511 outstanding block requests. Neither counter distinguishes
slow peer supply from serial processing or storage waits. The former run lacks
the necessary stage timings; new isolated probes cannot retrospectively fill it.

Preserve the operating node, its data/settings and the fixed-eight proof-failure
boundary. Use fresh bounded private nodes and compare network supply, message
processing, verification, block/undo/index/coins persistence and final-tip reach.
The requested prototype sequence is diagnosis, one evidence-backed candidate,
repeated comparisons and safety checks, then adoption only with a repeatable
benefit. Keep build/heavy test work out of timing windows. Record failed trials.

Also distinguish IBD-flag exit from catching the currently advertised header
tip: the graph's last sample was 07:38:16 at height 44,901,914/44,919,059; the
first subsequent equal-height sample was 07:38:51 at 44,919,067. Those correspond
to 21 h 00 m 14 s and 21 h 00 m 49 s from the 10:38:02 start.

Investigate actual header supply gaps and resident-index/cache allocation only
with measured scopes; do not repeat the rejected fixed-eight dispatch change or
add overlapping phase savings. Keep the future Lazy PoW / RSS management /
presync-performance PR split, with new block-pipeline work scoped separately
according to the final implementation. Private commands, source/binary hashes,
samples and results belong under `$EVIDENCE_ROOT/ibd-21h-followup/`.

## First diagnosis checkpoint

The operating GUI's executable hash differs from the current built GUI. The
observed 21-hour run therefore cannot be assigned all later source changes.
The operating process and normal executables are preserved for diagnosis.

A fresh requested-block regtest probe prepares solved coinbase-only blocks and
headers before timing. Eight ordinary outbound P2P responders answer real GETDATA
requests with pre-serialized blocks. They do not send unsolicited blocks. Final
tip, UTXO scan, verifychain and restart-tip checks pass. Regtest uses SHA256d;
this is not a mainnet Yespower or large-index reproduction.

| Diagnostic, 20,000 blocks | Elapsed | Node CPU | BLOCK-handler time |
|---|---:|---:|---:|
| All responder delays 100 ms | 2.936 s | 81.1% | 1.491 s |
| Delays 50/70/100/150/300/500/800/1,000 ms | 19.621 s | 16.0% | 1.430 s |

Nested private timers aggregate parse, lock scope, validation, ConnectBlock,
flush, DB writes and FileCommit. Their sums overlap; whole-process persistence
timers also include preparation/final sync/shutdown. Message queue waits overlap
and cannot be added to elapsed time. These traces establish a controlled slow-
supply example, not the missing historical mainnet stage timings.

Screen a lookahead of 4,096 versus 1,024 without raising the configured per-peer
128-request ceiling. The original source already documents the tradeoff:
larger lookahead tolerates uneven delivery but increases block-file disorder.
The first 40,000-block ABBA group was 72.84% shorter in the heterogeneous-delay
fixture. A second group and production-option safety checks remain pending.
Do not extrapolate this percentage to the 21-hour run or change defaults on it.
