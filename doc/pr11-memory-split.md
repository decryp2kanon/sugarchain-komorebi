# IBD memory changes extracted from PR11

Source: [PR11](https://github.com/decryp2kanon/sugarchain-komorebi/pull/11),
original head `90faec5afbe9a17cf0a04f4128d2e1576ef3b1bd`, base `f675133722e96389ef1906427735b91a01fa580d`.
Each optimization includes its implementation, regression checks and documentation
in one commit. The original branch and evidence remain preserved.

## M1: allocate proof entry pages on demand

Sources: `472f2a91ed`, `ee7cde5c10`, `ee4c8fbdab`, `12cbd9c69d`.
Use paged entry storage only for the existing growing Yespower cache; retain the
legacy contiguous backend and existing verification/budget/eviction rules.
The cache regression fixture exercises real headers under four scaled budgets,
delayed reuse, invalid proofs and stricter target limits.
See [measurements and limits](yespower-cache-budget.md). Those historical results
were collected on the original combined branch, not this newly separated build.
Historical background-scan cache suppression belongs to the separate Lazy PR.

### Split validation for M1

The separated build passed `header_pow_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## M2: pool dirty-index set nodes

Source: `e3f67f51d8`. Keep ordering and uniqueness while reusing pooled small
allocations; release only after the set is empty. The persistence/reopen fixture
checks unchanged headers, heights, status and predecessor identities.
Lazy-specific evidence assertions are retained in the later Lazy PR; this
optimization neither introduces nor requires persistent proof records.

### Split validation for M2

The separated build passed `blockmanager_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

EOF
