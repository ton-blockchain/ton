# W2: cross-account parallel transaction execution in the collator — design note

Status: DESIGN ONLY (not implemented). Produced by the W1 (io-mux) workstream after
account I/O was fully overlapped; updated with final-binary measurements. Context:
benchmark/PHASE2.md, benchmark/RESULTS.md phase-2 section.

## Why

With account I/O multiplexed (W1) and reads bundled (W5), collation intake is bound by
single-threaded per-transfer CPU. Final-binary measurement on a RAM-resident state:
476 jetton TPS ≈ 190 transfers/block ≈ ~1.0 ms effective collation CPU per transfer
(3 txs) — block limits are NOT the binder anywhere. 10⁴ jetton TPS = ~12k txs per
400 ms block requires ~15–20 effective cores executing transactions, plus admission
scaled to ~10k VM checks/s (~12 cores; W7's checker pool scales by constant).
TON's mostly-independent accountchains make this parallelizable in principle; this
note is the minimal-change scheme.

## Scheme

- **Canonical logical order stays sequential.** A single-threaded scheduler performs
  today's message selection exactly as the serial collator does (externals batch →
  generated-internals cascade → repeat, all existing limit/timeout logic). It does
  NOT execute; it assigns each selected message a logical sequence number.
- **Per-account lanes.** Each account has a lane; a transaction may start executing on
  a worker when (a) its in-msg is available and (b) the account's previous transaction
  (in logical order) has finished. Distinct accounts execute concurrently; same-account
  chains stay serial by construction. Special accounts (config, elector, anything
  tick-tock or in fundamental_smc_addr) are pinned to one lane and never parallelized.
- **Commit in logical order.** Workers produce finished `block::transaction::Transaction`
  objects; the scheduler commits them strictly in logical sequence: block-limit
  accounting, InMsg/OutMsg descriptor inserts, account-dict update, out-queue ops, and
  **lt finalization happen at commit time**. Per-account lt monotonicity and the global
  max_lt therefore evolve exactly in logical order.
- **Determinism / validity.** The committed block is valid TON (all invariants enforced
  at commit) but not bit-identical to what serial execution would produce (lt
  interleavings can differ). Acceptable: the collator *defines* the block; validators
  validate the result, they do not re-derive the schedule. No public-ABI change.
- **Usage-tree (collated-data proofs).** Workers execute over RAW prev-state cells
  (same snapshot the W1 walkers use) recording a per-tx cell-load log; at commit the
  scheduler replays the log against the usage-tracked tree (pure RAM, cells already
  materialized) so collated-data proof coverage is identical to serial execution.
- **Abort/limits.** If a committed-so-far prefix hits a block limit or the soft
  timeout, in-flight transactions beyond the cut are discarded; their messages follow
  the existing defer/enqueue paths (cascade deferral, W3 mempool holds). Discarded
  work never touched shared state — only the commit step mutates collator state.

## Expected gain & Amdahl

Per-block CPU at the time of the W1 measurement: TVM execution ~18 ms + per-message
scaffolding ~80 ms parallelize; ~70 ms of serialization/commit/finalization is the
serial residue (W6's serialize-tail overlap design attacks part of it). Expected
~3–4× on the execution portion; combined with the rest of the stack the realistic
single-shard ceiling moves into the few-thousand jTPS range — 10⁴ additionally needs
the admission scale-out and the serialize-tail/finalization work, i.e. a full-machine
project.

## Prerequisites / blockers (in order)

1. Fix the second instance of the §4b time-bounding bug (inbound-internal/dispatch
   queue loops) — already the binder in the unleashed RAM-resident regime (406 vs
   476 jTPS with mega-block/empty-block cycling).
2. Profile why effective per-transfer CPU is ~1.0 ms on the final binary vs the 0.54 ms
   measured pre-W1 (suspects: per-account collated-data proof building, io-mux
   bookkeeping at RAM speeds) — every µs here multiplies through W2.
3. Thread-safety audit of `block::transaction::Transaction` construction against
   shared collator state (config, libraries, storage-stat cache) — workers must touch
   only immutable snapshots + their own account state.
