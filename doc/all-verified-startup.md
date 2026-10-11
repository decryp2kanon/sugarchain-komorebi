# All-19 full-verification mode comparison

The candidate enables the final implementations represented by items 1–19 in
both modes. The previous baseline `7d4a5207f0` shared 13 items and excluded DB
read-cache policy (6–8) and read-ahead (16–18). Items 6–8 describe successive
steps of one cache-policy experiment; the candidate shares the final policy,
not three independent caches or switches.

**Historical disk trust remains controlled only by `-fast-startup=1`.** Mode zero
retains the existing full Yespower verification path. No DB format or stored proof
flag changes, and trusted status is never promoted to actual proof evidence.

## Whole-node sample: 6,000 blocks, mode zero

Five runs per version, mixed order, fresh private DB copies, network/wallets off,
`-parpow=8 -assumevalid=0 -dbcache=450 -maxpowcache=2048 -fast-startup=0`.
No concurrent builds or tests during timing. All tips, heights and chainwork match;
`verifychain(3,100)` and normal shutdown pass for every run.

| Median | Previous 13 items | All 19 | Interpretation |
|---|---:|---:|---|
| RPC readiness | 2.851478 s | 2.840680 s | 0.010798 s shorter; effectively unchanged |
| Peak RSS | 136,964 KiB | 137,076 KiB | 112 KiB higher; effectively unchanged |

The small DB does not establish the large-index cache-policy tradeoff.

## Large-index DB-policy comparison: bounded verification

Use a private copy of the 44,806,391-entry index. Execute its actual full key
pre-count and load the first 50,000 entries through the real verifier with disk
trust **false**, then intentionally interrupt. An instrumented wrapper forwards
unchanged to the real Yespower function; every run records **exactly 50,000 calls**.
Counting excludes chain-parameter initialization's separate genesis hash.
Both runs per policy have identical entry checksums and succeed at the intended
bound. Only private copied SST files receive cold-file advice.

| Median | Original DB read cache | Shared reduced/reclaimed cache | Difference |
|---|---:|---:|---|
| Count + 50,000-header verification | 28.9163 s | 31.21115 s | **2.29485 s longer (+7.94%)** |
| Helper peak RSS | 7,363,928 KiB (7.02 GiB) | 135,190 KiB (132.0 MiB) | **6.89 GiB lower** |
| Actual Yespower calls | 50,000 | 50,000 | No verification skipped |

This is **not a completed 44-million-header node startup**. It isolates the added
DB policy: the helper has no chainstate, linking or candidate reconstruction and
reserves a prefix-sized map. Peak RSS savings mostly remove mapped DB pages; they
do not mean the full node can fit in 132 MiB. Two repetitions are screening
rather than a hardware-independent timing guarantee.

## Prefetch depends on index layout

One million index objects, actual `CChain::SetTip`, five runs per selection:

| Layout | Without hints | With hints | Result |
|---|---:|---:|---|
| Contiguous | 0.0104754 s | 0.0158284 s | Slower by 0.005353 s |
| Shuffled placement | 0.111883 s | 0.0731165 s | Faster by 0.038767 s |

Every active-chain pointer matches. The earlier negative contiguous screening
did not establish a universal slowdown. These are metadata helper measurements;
other prefetch hints and full historical startup are not independently timed here.

## Assessment and limits

All-19 mode zero preserves the verification boundary. The bounded large-index
screening shows a substantial DB-memory saving with a modest measured time cost;
the small complete-node sample is effectively unchanged. For a memory-first
policy this supports sharing the final DB cache policy, but does not prove that
all 19 features independently speed up every machine or every stage.

The 44-million-header full run, sustained IBD, wallet behavior and constrained-RAM
OOM survival were not measured. The running node, original DB and Enikk were not
restarted or modified. [Sanitized raw metrics](benchmarks/all-verified-startup-20261011.json)
contain no local filesystem paths or RPC credentials. Raw logs and the sample
harnesses are retained privately; the scratch DB copy may be discarded after use.

## Regression validation

Release daemon, unit-test and Qt builds pass. The relevant 69 unit cases pass
in default mode and with explicit `-fast-startup=1`; the same 69 cases pass under
ASan + UBSan with leak checking. Startup functional checks and invalidate/reconsider
functional checks pass; the latter also passes under ASan + UBSan. Existing
verification fixtures exercise both trust selections and real header verification.
No new implementation-mirroring test was added for the six condition changes.
