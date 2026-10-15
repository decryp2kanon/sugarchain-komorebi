# Follow-up to the observed 21-hour IBD

Baseline: `5866c6540a`, 2026-10-15. Investigation in progress; no new speedup
or runtime change is established at this checkpoint.

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
