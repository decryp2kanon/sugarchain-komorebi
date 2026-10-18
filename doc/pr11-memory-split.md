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

## M3: release the serialized batch input before DB insertion

Source: `0e31c20cbf`. Extract the rvalue write overload without Lazy consensus or
evidence arguments. Borrowed callers keep their vector; owned storage is freed
after serialization and before the existing synchronous atomic DB write.
The standalone reopen test extracts the owned-vector assertions from the original
mixed historical-evidence fixture. The full fixture remains in the later Lazy PR.

### Split validation for M3

The separated build passed `header_pow_tests/block_index_owned_batch_survives_reopen`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## M4: bound header-replay dirty-index accumulation

Sources: `bf9eb5ce8d`, `c6ad3c0c5f`, `e7b8a79370`. Keep the final 32768 scheduling
threshold, documented costs and headers-only count-boundary crash/restart checks
in both startup modes. Do not publish the superseded 262144 intermediate policy.
The existing fixture uses private SHA256d regtest nodes, not historical Yespower
evidence or power-loss injection. Full-block PERIODIC writes follow in M5.

### Split validation for M4

The separated build passed `blockmanager_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

EOF
