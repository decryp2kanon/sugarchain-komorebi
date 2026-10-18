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

EOF
