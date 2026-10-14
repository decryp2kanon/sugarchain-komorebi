# Presync chainwork calculation optimization results

2026-10-14. This report was written after implementation, repeated measurements,
and the normal validation tests.

## Implementation

PRESYNC and REDOWNLOAD chainwork accumulation in `src/headerssync.cpp` now reuses
the existing exact `GetBitsProof(bits, true)` calculation. Eligible compact targets
use 64-bit arithmetic; other targets retain the original 256-bit calculation.
The function's `fast_startup` argument selects an arithmetic implementation. These
calls do not change the runtime `-fast-startup` setting or historical PoW trust
policy.

Yespower computation, the eight-worker limit, the maximum of eight cold checks
before a failure decision, difficulty, MTP, linkage, commitments, and minimum
chainwork requirements are unchanged. No additional cache or memory allocation
was introduced. This change does not impose a global 5 GiB RSS limit.

## Measurements

The baseline was commit `a63275b21c`. Baseline and candidate executables used the
same build configuration and were measured in baseline/candidate/candidate/baseline
order. Each run processed 6,000 real mainnet headers through 300 PRESYNC and
REDOWNLOAD rounds. Genuine input PoW verification was outside the timer. Every
round checked state transitions and returned headers.

| Measurement | Baseline | Candidate |
|---|---:|---:|
| First run | 5.08020 s | 4.21122 s |
| Second run | 5.12126 s | 4.16071 s |
| Median | 5.10073 s | 4.185965 s |

State-machine component time decreased by **17.934%**, corresponding to about
**21.85% higher throughput**. This is not an end-to-end presync measurement:
actual Yespower computation and network delivery are excluded. Do not apply this
percentage to the full seven-hour presync duration.

A separate eight-worker proof benchmark checked 6,000 headers in 2.98655 s on its
cold pass (2,009.01 headers/s, 6,000 actual Yespower calls). The repeated pass took
0.00653611 s with zero actual Yespower calls. The operating node was still using
CPU resources. These figures are neither a dedicated-machine throughput ceiling
nor a baseline/candidate comparison. They confirm that proof reuse already works.

## Expected benefit and limitations

This is a small arithmetic optimization, not a demonstrated solution for reducing
seven hours to three. Initial Yespower verification is much more expensive, and
the end-to-end reduction from this change remains unmeasured. Verification was
not weakened, and the worker count was not increased beyond eight to pursue the
three-hour target.

Cache expansion and queue redesign are outside this change. They require further
evidence about repeated PoW computation and worker waiting time. Existing shared
caches, rolling target sums, and SugarShield division optimizations are not counted
as new improvements.

## Validation

- Normal chain, PoW, and header-sync tests: 21 cases and 6,872,266 assertions passed.
- Normal SugarShield tests: 12 cases and 404,609 assertions passed, including
  6,000 real mainnet headers and both synchronization passes.
- Existing exact-arithmetic tests passed against an independent 256-bit reference
  across compact exponents, sign, overflow, and boundary inputs.
- Isolated `p2p_headers_sync_with_minchainwork.py` passed.
- Isolated `p2p_sugarchain_header_pow.py` passed.
- Daemon, GUI, CLI, and unit-test builds completed.
- ASan, UBSan, and leak checks: the same 33 cases passed with 7,074,267 assertions.
  Assertion totals differ from the normal run because the chain fixture uses
  randomly generated branches and queries.

The operating node and its data were not restarted or modified. The rebuilt
executables take effect on the next user-initiated restart. Restarting during
presync can lose its progress; applying this small change does not justify
interrupting the current run.

## PR separation

PR #11 is a temporary integration branch. Before merge, separate it into three
PRs and review and measure each scope independently:

1. **Lazy PoW:** background historical verification, persistent evidence and
   reuse, and parallel verification.
2. **RSS optimization:** retained memory and transient IBD peak reductions.
3. **Presync speed optimization:** time reductions, including this chainwork
   calculation change.

A change whose main purpose is presync speed belongs in the third scope even if
it adjusts memory usage. No PR split or merge was performed as part of this work.

## Reproduction and evidence

```bash
build/bin/bench_headers_sync src/test/data/sugarchain_headers.raw 6000 300
build/bin/bench_header_pow src/test/data/sugarchain_headers.raw 6000 8 16
```

SHA256 of the 6,000-header input:
`d0560434d8927def1b9a656822b74715cc030440877c18735d8256c6d0e0c67b`.

Local raw measurements, build output, and test logs:
`/home/ak/.local/state/sugarchain-komorebi/presync-speed/`.

Earlier performance measurements: [IBD optimization](ibd-optimization.md).
