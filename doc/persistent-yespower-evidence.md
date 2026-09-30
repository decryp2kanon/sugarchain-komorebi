# Persistent Yespower verification evidence

Sugarchain validates every new header with YespowerSugar. Previously,
`LoadBlockIndexGuts()` repeated that expensive calculation for every stored
header on every restart. The block-index validity level cannot replace this
check because it does not explicitly prove that Yespower succeeded.

Core31 now records separate, versioned evidence after a real Yespower check
succeeds. The evidence key is the exact serialized header's SHA256d identity.
Its value binds the evidence format to the network genesis, `powLimit`, and the
YespowerSugar version, N, r, personalization, and header-serialization rules.
Every lookup still validates the compact target against the current
`powLimit`. A missing, malformed, unknown-version, wrong-network, or stale-rules
entry falls back to the full Yespower check and is replaced only after success.

Evidence uses a separate key space in the block-tree LevelDB. Startup migration
writes atomic batches of 4,096 entries, with a synchronous final batch. A crash
can lose only evidence that was not committed; the next startup safely
recomputes it. Freshly accepted headers are written atomically with their dirty
block-index entries, and only current-process proof state can create evidence.
Old databases contain no evidence and therefore receive a one-time full
revalidation. Full `-reindex` wipes and rebuilds the evidence with the block
index. `-reindex-chainstate` preserves it because that operation retains the
validated header index.

The deterministic 6,001-header mainnet fixture measured 6,001 real Yespower
calls on its first open and zero on its warm reopen. The measured wall times
were 16.82 seconds and 0.027 seconds respectively. These are component results,
not full-node startup measurements.

To migrate one of the dedicated full-node test copies later, start it normally
with the new binary and allow `Loading block index` to finish. For example:

```sh
build/bin/sugarchaind \
  -datadir="$HOME/Desktop/test-fullsync2/startup-shutdown-testdata/node1" \
  -server=1 -rpcuser=rpcuser -rpcpassword=rpcpassword \
  -port=21315 -rpcport=21314 -listen=0 -dnsseed=0 -discover=0 -connect=0
```

The first run performs the real historical Yespower checks and creates the
evidence incrementally. Stop it normally after RPC becomes ready. Subsequent
starts reuse matching evidence. The 44-million-header migration has not been
run as part of this change.
