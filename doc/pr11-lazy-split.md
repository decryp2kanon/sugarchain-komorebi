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

EOF
