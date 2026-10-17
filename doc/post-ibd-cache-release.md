# Release IBD-era cache memory after catchup

## Problem and scope

The completed mainnet run retained about 11.905 GiB RSS before shutdown; a later restart ran around 9.24–9.29 GiB. The last pre-shutdown UpdateTip reported 1330.8 MiB of coins cache versus 0.3 MiB after startup. This is evidence of retained useful cache data, not proof of a leak. That restart also followed removal and compaction of historical proof records, so it is not a controlled restart-only comparison.

This change releases IBD-era caches once after catchup. It does **not** reduce earlier IBD peak RSS, reset lifetime VmHWM, impose a total process limit, or guarantee a particular mainnet RSS. The large permanent block index remains live. Normal caches can grow again.

## Lifecycle and safety

- On Yespower-chain IBD exit, schedule cleanup. IBD can exit before actual header-tip catchup; wait until the active tip equals the best header and there is exactly one chainstate. Import/loading and interruption remain guarded. Snapshot/background chainstates defer cleanup.
- The node import completion path and the end of successful block activation drain the request. UTXO persistence uses existing FORCE_FLUSH ordering: block/undo data, block index, then coins. The coins pool is rebuilt only through the existing successful persistence path. Existing fatal disk failure handling remains in effect.
- Clear the one-shot request after successful persistence. Subsequent normal blocks retain newly cached entries. This is not a timer or per-block cache reset.
- Shrink the growing Yespower cache back to its initial tier and recreate batch evidence. Keep its salt, configured budget, target checks and genuine cache-miss behavior. It may grow again under existing rules. The eager API backend is unchanged. Current node startup uses the growing backend in both fast-startup trust modes.
- Cache replacement is serialized with lookups/inserts. Old storage is destroyed after unlocking the proof-cache mutex and outside cs_main. In-flight genuine checks may insert into the replacement. Allocation failure leaves old proof storage intact and is not treated as invalid PoW or peer misbehavior.
- On Linux/glibc, after actual cache release, call malloc_trim(0) once outside cs_main and the proof-cache mutex. The isolated measurements below justify this conditional step: freeing objects alone left large resident free arenas. This is not periodic global trimming. Other platforms still release objects and let their allocator manage residency; RSS behavior is allocator-dependent. Replacement allocators may make the glibc call ineffective.
- No new user option, RPC, consensus rule or proof record deletion. maxpowcache, parpow, dbcache and inflight settings retain their meanings. No operating-node restart or runtime cache mutation was used for these tests.

The cleanup log reports coins logical bytes and allocated proof-page/directory bytes, **not process RSS**. Proof accounting excludes hash flags, allocator overhead and batch metadata. `allocator=released` is the allocator return status, not a measured byte count.

## Isolated measurements

Source: [standalone allocation fixture](benchmarks/post-ibd-cache-probe.cpp). Raw results: [JSON](benchmarks/post-ibd-cache-2026-10-16.json).

Each process inserted 8,000,000 synthetic salted header keys into the real growing proof store with a 2048 MiB budget and 2,000,128 synthetic OP_TRUE UTXOs into an isolated on-disk coins DB. These keys bypass proof admission **only inside this standalone allocation fixture**; no synthetic evidence is admitted to a node. Genuine proof correctness is tested separately.

For the clean-cache comparison, Sync and a fixture-only explicit compaction settle DB work before the filled sample; 128 additional dirty coins exercise the final write. Production cleanup does not add an explicit compaction beyond existing FlushStateToDisk behavior. Four modes are run in forward/reverse order, two repetitions each. All modes include a separate trim phase to distinguish live-cache release from allocator-only return. The unchanged mode's trim is a measurement control, not a new normal-node action.

Mean process RSS is in GiB. All numbers include the small fixture process, not a 45-million-index node.

| Cache operation | Filled RSS | After object cleanup | After allocator return | After 100k lookups | Cleanup + return, s | 100k lookups, s |
|---|---:|---:|---:|---:|---:|---:|
| none | 1.583 | 1.583 | 1.564 | 1.564 | 0.002 | 0.055 |
| proof | 1.582 | 1.581 | 0.283 | 0.283 | 0.206 | 0.043 |
| coins | 1.583 | 1.406 | 1.304 | 1.404 | 0.331 | 0.458 |
| both | 1.565 | 1.543 | 0.023 | 0.123 | 0.534 | 0.339 |

The combined case recovered **1.542 GiB / 98.53% of this fixture's filled RSS** immediately, retaining **1.442 GiB / about 92.1%** of the saving after lookups. This percentage must not be applied to total mainnet RSS: the fixture has no permanent full block index. Proof-only recovery was 1.299 GiB. UTXO-only recovery was 0.279 GiB immediately and 0.179 GiB after reads. Effects are paired within each process and are not additive across different runs.

The combined case freed about 1.52 GiB of allocator-used storage, but before trim roughly 1.53 GiB remained as allocator-free arena space. malloc_trim made those pages nonresident while much of the virtual address reservation remained. A trim-only control with live caches recovered only about 0.019 GiB on average. Thus a trim call alone is not a substitute for releasing live caches.

## Costs and limitations

- Combined cleanup plus allocator return: **0.471–0.597 s**, mean **0.534 s**; main lock includes the coins flush, while proof destruction and trim run outside it. Global allocator locking can still stall allocations in other threads. RPC tail latency has not been measured.
- The first 100,000 distinct UTXO lookups took 0.339 s versus 0.055 s with the cache retained: **about +522% / +0.285 s** in this fixture. File mappings, DB read state and 100,000 new cached entries raised RSS again. The file cache was warm; this is not a cold-device latency guarantee or a block-throughput percentage.
- Three extra fixtures with 2,000,128 dirty coins and no prior Sync took **1.948–2.122 s**, mean **2.044 s**, to flush and trim. About **157–162 MiB** of process write_bytes was observed during the flush; asynchronous DB work also overlapped later samples. These are OS process I/O counters, not exact extra storage attributable solely to this policy. Such dirty data would otherwise be persisted later.
- Future genuine old-header checks can lose RAM hits and require actual PoW or existing valid live-index evidence. The allocation benchmark is not a PoW throughput benchmark and does not measure full reorg, wallet/RPC or transaction latency.
- Small fixtures, two clean repetitions and three dirty repetitions on a shared host are not a complete mainnet benchmark. The existing eight-worker Lazy audit continued running. Own builds/tests did not overlap timing, but host contention was not eliminated. Do not claim unchanged foreground performance.
- The original roughly 2.66 GiB restart gap has not been decomposed completely. No modified binary was applied to the operating node. Mainnet post-cleanup RSS and future cache regrowth remain to be measured.

## Regression coverage

Final daemon, CLI, GUI and unit builds passed. The final targeted selection passed **41 test cases / 1,640,616 assertions**; 781 unrelated cases were skipped. The functional feature_fast_startup test also passed on the final binary. The targeted unit selection covers real proof recomputation after shrink, invalid targets, eager-mode compatibility, concurrent checks/shrinks, IBD request scheduling, catchup deferral, one-shot behavior and coins-cache persistence. The cleanup persistence fixture uses a real private LevelDB and reopens it before checking the stored coin and best-block marker. Existing coins, cuckoo-cache, chainstate-write and flush suites are included.

The functional feature_fast_startup test covers normal/fast startup, reindex-chainstate and private full-block/header persistence boundaries. Regtest uses SHA256d, so this is complementary regression coverage, not a genuine mainnet cleanup resource test. A broader legacy 6000-header hash comparison was intentionally stopped for runtime; it is not counted as passed. Earlier test-launch errors from Bitcoin-named binary/config defaults were corrected with a private wrapper; failed launches are not passes.

## Reproducing the allocation fixture

First build the node and unit target. Compile the standalone source against the same static libraries as test_bitcoin, without its test objects or yespower wrapper. It includes pow.cpp to access the internal allocation store, and is not compiled into the node. From the repository root:

```sh
cmake --build build --target bitcoind test_bitcoin -j2
python3 - <<'PYCODE'
import pathlib, shlex, subprocess
root = pathlib.Path.cwd()
args = shlex.split((root / 'build/src/test/CMakeFiles/test_bitcoin.dir/link.txt').read_text())
libs = args[args.index('-o') + 2:]
libs = [str((root / 'build/src/test' / s).resolve()) if not s.startswith(('-', '/')) else s for s in libs]
command = ['c++', '-O2', '-std=c++20', '-Isrc', '-Ibuild/src',
           '-Isrc/univalue/include', '-Isrc/secp256k1/include',
           'doc/benchmarks/post-ibd-cache-probe.cpp', '-o', '/tmp/post-ibd-cache-probe'] + libs
subprocess.run(command, check=True)
PYCODE
# Supply a NEW, private database path for each process. Never use a node data directory.
/tmp/post-ibd-cache-probe both /tmp/unique-private-coins-db 8000000 2000000
# Dirty-cache cost fixture; fresh private path, no initial Sync/fixture compaction:
/tmp/post-ibd-cache-probe both /tmp/another-private-coins-db 0 2000000 dirty
```

Repeat none/proof/coins/both/both/coins/proof/none with separate paths. The fixture requires Linux/glibc mallinfo2 (glibc 2.33+) for its extra allocator diagnostics; production trim does not require mallinfo2.

EOF
