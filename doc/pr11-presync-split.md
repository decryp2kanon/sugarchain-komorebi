# PR11 split review notes

## P1: reuse exact compact-target work arithmetic

Sources: `e242072fc1`, `2ddf658415`, `3882711922`, `8ab68cd324`, plus the retained
benchmark attribution/reuse drivers (`cc06618d89`, `301f6a6971`) and Yespower
known-output/scratch regression checks (`481cdf9e3a`).
Both PRESYNC and REDOWNLOAD call the existing exact GetBitsProof path, with its
generic fallback. No PoW, target, minimum-work or commitment check is bypassed.
The benchmark driver is a measurement tool, not an adopted prefetch or dispatch
policy. It is shared by the later arithmetic/MTP optimizations in this PR.
Historical measurements on the mixed branch do not prove whole-presync speedup.

### Split validation for P1

The separated build passed `sugarshield_tests:yespower_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## P2: combine exact positive target divisions

Source: `51f13a2c25`. Preserve floor rounding, product guard, generic fallback,
subsequent multiplication and clamp. Include the rounding/fallback regression
case in this same optimization commit. No consensus outcome change is intended.
The combined measurement series is published with P3; do not assign the combined
percentage to this optimization alone.

### Split validation for P2

The separated build passed `sugarshield_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## P3: retain immutable bounded-history endpoint medians

Sources: `b1d23b402e`, `a57c29924d`; research provenance remains in original PR11
commits `49b9f7ed78` through `5866c6540a` and `1e2507af39` through `90faec5afb`.
Keep non-genesis initialization, history reset/eviction and difficulty outcomes
identical; do not enlarge global CBlockIndex. The duplicate/decreasing-time MTP
regression belongs to this commit.
The arithmetic+MTP component result is about 4.01% shorter; real P2P results are
inconclusive. Two mainnet runs had presync 1.43% longer, not a demonstrated
seven-hour reduction. No adopted worker/prefetch/final-remainder growth policy
is added. Candidate .patch files are explicitly un-applied historical artifacts.

### Split validation for P3

The separated build passed `sugarshield_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

EOF
