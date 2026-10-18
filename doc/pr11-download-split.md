# PR11 split review notes

## D1: use internal IBD download lookahead without another option

Sources: `4d3b0b9be7` through `51c07edbcb` (download code, limits/stall tests,
reproducer and final no-option measurements); diagnosis `2e2321140d`, `c5985f873e`.
Keep only final IBD lookahead 4096. Non-IBD uses existing 1024. Per-peer inflight
limits remain independent and unchanged. Do not reintroduce maxibddownloadwindow.
The same optimization commit contains v1/v2 stalling/limits checks, the bounded
reproducer and CPU/RSS/I/O limitations.
The full-mainnet pair is observational: 16.71% shorter total IBD cannot be
attributed exclusively to this change. No whole-presync improvement is proven.
Only the user's previously authorized completed graphs and derived aggregate
are included, never a live graph or raw operating logs.

Whole-PR checks: daemon/GUI/CLI/test/bench builds, `p2p_ibd_download_limits.py` and `p2p_ibd_stalling.py` passed in isolated test datadirs.

### Split validation for D1

The separated build passed `peerman_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

EOF
