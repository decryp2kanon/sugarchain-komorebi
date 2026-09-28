# SugarShield regression fixtures

`sugarchain_headers.raw` contains Sugarchain mainnet headers at heights **1 through
6000**, in order. Each record is the standard 80-byte serialized block header;
there is no record count, transaction count or genesis record.

These are the headers retained from the IBD bootstrap's earlier 6,000-header
offline validation, not a new network download. Tests embed the data through the
existing `target_raw_data_sources` mechanism and never contact a peer or read a
developer's node/datadir. Genesis comes from the production mainnet parameters.

- Size: 480,000 bytes.
- File SHA256: `d0560434d8927def1b9a656822b74715cc030440877c18735d8256c6d0e0c67b`.
- Genesis block ID: `7d5eaec2dbb75f99feadfa524c78b7cabc1d8c8204f79d4f3a83381b811b0adc`.
- Height 1 block ID: `ce8a0df339f2edceb99c5325c95b2b0ae752e29de1193f6113549f0e1cae7c91`.
- Height 6000 block ID: `e7a04205f70e5b6e99d83a8f720748fee559a382701b39ff9891e391e6cf81d9`.

Block IDs above are SHA256d identifiers, not YespowerSugar hashes. The regression
test separately verifies every header's YespowerSugar PoW, its link to the prior
header, and the recorded `nBits` against the full-history difficulty calculation.
The fixture captures the first mainnet difficulty change: height 511 has
`0x1f3fffff`, height 512 has `0x1f35c28e`.

## Calculation vectors

The mainnet calculation being preserved is the official Sugarchain
[`GetNextWorkRequired` / `CalculateNextWorkRequired`](https://github.com/sugarchain-project/sugarchain/blob/64bc05ccc1dc2dcd4db9e86715c14dee12be6460/src/pow.cpp)
implementation. Its mainnet window is 510, target spacing 5 seconds, and damped
timespan limits are 2142 and 3366 seconds. Synthetic fixed expected values were
calculated independently with integer arithmetic; tests do not generate expected
answers by calling a duplicate of the production function.

For constant target T and endpoint median difference D, the expected target is:

1. `S = 2550 + trunc_toward_zero((D - 2550) / 4)`.
2. Clamp S to `[2142, 3366]`.
3. `floor(T / 2550) * S`, capped at the mainnet powLimit, then compact-encoded.

With target `0x1e123456` and five-second timestamps, histories containing
511, 520 and 521 entries have MTP differences 2525, 2545 and 2550 respectively.
Their expected compact targets are `0x1e12295e`, `0x1e123282`, `0x1e123455`.
This also checks the upper median used for the early ten-entry MTP window.
At 521 entries, retaining only 520 shifts the older median by five seconds and
incorrectly produces `0x1e123282` instead of `0x1e123455`.

The small target `0x03012345` represents integer 74565. Dividing first gives
`floor(74565 / 2550) * 2550 = 73950`, compact `0x030120de`. Multiplying before
dividing would give a different consensus result.

The averaging-membership vector changes exactly one of the 510 targets from
`0x1e123456` to `0x1e100000`: the expected result is `0x1e12333a`. Changing the
target immediately outside the averaging interval must have no effect.

## Test layers

- Fixed arithmetic, clamp, compact-rounding and MTP vectors.
- Full versus truncated context at 510/511/520/521 entries and repeated sliding.
- Actual `HeadersSyncState` PRESYNC, reset, REDOWNLOAD and returned-header checks.
- Incorrect difficulty with otherwise valid PoW, distinguished by the actual
  difficulty-rejection diagnostic in both passes.
- Different valid chains rejected by the salted commitment check (fixed RNG seed).
- Non-genesis starts, small redownload buffers, and zero-window Bitcoin behavior.
- All 6000 real mainnet headers, plus two-pass sync from genesis and height 4000.

The existing Bitcoin retarget vectors in `pow_tests.cpp` keep their original
expected results and explicitly use Bitcoin v31 mainnet PoW parameters instead
of Komorebi mainnet parameters. Chain-parameter sanity tests check overflow for
the selected algorithm: SugarShield's target sum and divide-then-multiply path,
or Bitcoin's multiply-then-divide retarget path.

Synthetic header-sync tests retain the mainnet SugarShield parameters and
powLimit, but select SHA256d for inexpensive deterministic nonce searches. This
isolates difficulty/commitment rejection from invalid PoW. It does not replace
the real-header YespowerSugar test. Direct calculator vectors need not be valid
mined chains; the public state-machine fixtures satisfy its PoW/link preconditions.

Run with the existing unit-test target:

```sh
cmake --build build --target test_bitcoin -j4
ctest --test-dir build --output-on-failure -R '^(sugarshield_tests|headers_sync_chainwork_tests|pow_tests)$'
```

For pointer lifetime checks, configure a separate build with
`-DSANITIZERS=address,undefined`, build `test_bitcoin`, and run the same suites.
No production test-access hooks, network IBD, or performance benchmark is needed.
