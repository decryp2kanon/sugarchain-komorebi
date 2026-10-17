# Guarded final-tier cache experiment

Date: 2026-10-18. Baseline: `b243458883`.
Status: experiment checkpoint; no runtime candidate adopted.

## Scope and unchanged operating state

The existing growing cache stops at 1294.734375 MiB of proof entry capacity with
maxpowcache=2048, leaving 751.265625 MiB of the valid-store budget unused. This
experiment tests a final remainder tier without changing configured limits,
worker count, PoW rules, batch evidence or the eight-cold-job rejection boundary.
The production source, binaries, operating node and datadir are not replaced.

The [experimental patch](benchmarks/presync-cache-remainder-candidate.patch) is
provided for review/reproduction only. It is not applied to production pow.cpp.
Private complete translation units override the same static archive in privately
linked drivers and unit executables. Both arms use the same current source and
compiler flags. Existing earlier segmented-cache results are not attributed to
this candidate.

## A rejected first attempt

Simply accepting every remainder of at least two entries failed the existing
`growing_cache_retains_verified_proofs_and_rejects_invalid_headers` test: two
recomputations were observed where zero was required. The 2048-byte internal API
fixture can retire a substantially larger active tier into a two-entry remainder.
The initial full header-PoW selection passed 32 cases and failed one case, with
55008 of 55009 assertions passing. This was a cache-retention regression, not
acceptance of an invalid proof. The existing assertion was not weakened.

The guarded candidate requires the remainder to have at least two slots **and**
capacity for the current active tier's insertion count. This preserves the tiny
cache contract while permitting the 751 MiB remainder at the requested budget.
The allocation-failure and budget tests are private diagnostic fixtures; their
synthetic keys are never admitted as node evidence.

## Predeclared comparisons

- Finish builds and safety tests before timing.
- Original 2048 MiB budget: early cold actual-PoW/PRESYNC driver, 40000 unique
  headers and eight peer states, mirrored order. This does not reach final growth.
- Scaled 4 MiB budget: 100000 unique headers and eight peer states, two ABBA
  groups. Confirm actual final growth and count physical PoW calls independently
  of received peer-header count. This is the primary bounded cold screen.
- Separately report delayed-repeat reuse with changed message boundaries.
- Allocation-only full-budget screen checks storage and shrink behavior, never
  claiming synthetic insertion is a genuine PoW throughput result.
- Proposed cold gate: at least 3% shorter in both ABBA groups, consistent with
  reduced recomputation, and all existing correctness boundaries preserved.
  Repeat-only improvement does not qualify as initial-PRESYNC acceleration.

Owned timing processes inherit CPUs 0-3,8-11, nice 19 and idle I/O priority.
They have an 8 GiB RLIMIT_AS and 180-second per-process timeout. Affinity is not
an enforced CPU quota; RLIMIT_AS is a virtual-address limit, not cgroup/RSS
accounting or a complete-node memory guarantee. The operating node remains on
this shared host, so contention is not eliminated. No multi-hour mainnet trial
is started automatically.

Private source snapshots, commands, raw logs and binary identities are retained
under `~/.local/state/sugarchain-komorebi/cache-remainder-20261018/`.
Final results will replace this checkpoint when the bounded comparison finishes.

EOF
