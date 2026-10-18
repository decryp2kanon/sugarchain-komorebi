# PR11 split review notes

## L1: persist genuine versioned local evidence with index writes

Source: `6446168046`, plus the current-process accessor from `045a30e05f`.
Layer evidence serialization onto M's PopulateBatch/rvalue writer, preserving
atomic index+evidence writes and pointer release only after all serialization.
Bind the header and rules digest, not height or disk validity. The isolated
record fixture checks missing records, rule/header mismatch, unknown version and
truncation; full genuine migration/reopen checks follow with the scanner in L2.
The fixture's manually written matching record demonstrates format matching,
not resistance to a malicious local DB writer. These are not cryptographic proofs.
Restore the original dirty-index fixture's no-evidence assertions here, once
the evidence API exists; M's standalone persistence checks remain retained.

### Split validation for L1

The separated build passed `header_pow_tests/persistent_evidence_records*:blockmanager_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## L2: scan bounded DB batches with parallel genuine verification

Sources: `e11aec48ff`, `aec27f2405`, `885e3afb07`, `dc2d162464`, `6318aa53e0`,
`038893787d`. Keep final short-lived iterators, at most 512 owned records,
parallel results, current-process reuse, sync persistence, partial-failure and
interruption handling. Exclude historical scan insertions into the foreground
RAM cache without disabling ordinary header cache use.
Migration/restart, rules mismatch, unknown/truncated records, failure/interruption,
durable reopen/reindex and real recomputation assertions are included here.
Statistics are library-level until node/RPC integration in L3.

### Split validation for L2

The separated build passed `header_pow_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## L3: integrate audit lifecycle, failure shutdown, RPC and progress

Sources: `045a30e05f`, `8d3c39f636`, `40c15bb360`, final logging changes
`5c45e871db` through `32bab122fe`, and mode-boundary tests from `aec27f2405`.
Preserve default fast-startup=0 full startup verification. Mode 1 retains the
explicit historical trust window. Start only after readiness; join before DB
destruction, distinguish storage/resource errors from confirmed invalid PoW and
request mandatory shutdown. Include RPC, worker/pause validation, immediate zero
progress and batch-boundary monotonic ten-second logging.
The reverted both-mode asynchronous-startup policy is not reintroduced. The
existing full-mainnet completion is evidence of one scan, not all failure cases.
Regtest functional checks do not run genuine mainnet Yespower auditing.

### Split validation for L3

The separated build passed `header_pow_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

## L4: reimport the exact fixed genesis during recovery

Source: `bb75efd1ce`. The configured genesis predates contextual commitments;
keep the exact identity exception without bypassing earlier header/PoW/body
checks for arbitrary blocks. Include the original contextual regression test.
This is a recovery correction required by the documented reindex path, not a
new general exemption or startup-speed claim.

Whole-PR checks: daemon/GUI/CLI/test/bench builds and the private-node `feature_fast_startup.py` functional fixture passed. No operating node was restarted.

### Split validation for L4

The separated build passed `validation_chainstatemanager_tests`. Detailed owned logs are retained privately
under `~/.local/state/pr11-split-20261019/`. These checks are not new mainnet
benchmarks. Additional whole-PR/functional validation is recorded in the PR.

EOF
