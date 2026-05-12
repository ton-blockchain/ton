# Simplex TLA+ Formal Verification

This directory contains the TLA+ formal specification of the Simplex consensus protocol
used in TON (Catchain 2.0), along with multiple TLC model-checking configurations.

### Design Decisions

- **Real time abstracted away**: Timeouts (Rule 6) are modeled as nondeterministic actions enabled once the window is active. Weak fairness ensures they eventually fire, matching the "finite timeout" guarantee. The exponential growth `T₀ · α^{k-k*-1}` is an ordering constraint that TLA+ abstracts — in the model, any interleaving is explored.
- **Probabilistic network → nondeterminism + fairness**: The post-GST guarantee "delivered with probability ≥ p > 0" is modeled by strong fairness on delivery: if messages repeatedly exist, eventually one is delivered.
- **Per-validator candidate knowledge**: `knownCandidates[v]` replaces the prior global `candidates[slot]`. A validator must *receive* a candidate (via `CandidateMsg` delivery or `CandidateReplyMsg`) before voting on it, exactly matching Rule 4's "Upon receiving a candidate."
- **Local proof for leader base selection**: `ProposeCandidate` checks `observedNotar[leader]` and `observedSkip[leader]`, not the global `reachedNotar`/`reachedSkip`. This matches Rule 3's "v chooses any base for which **it can prove**."

## Files

| File                    | Description                                                                   |
|-------------------------|-------------------------------------------------------------------------------|
| `simplex.tla`           | Main TLA+ specification (all 8 rules, safety invariants, liveness properties) |
| `simplex_quick.cfg`     | Quick safety-only check (no temporal properties, fastest)                     |
| `simplex_tiny.cfg`      | Smallest model (3 validators, 1 slot) for fast invariant smoke-checks         |
| `simplex_normal.cfg`    | Normal operation: all honest, reliable network (safety + liveness)            |
| `simplex_byzantine.cfg` | Byzantine fault tolerance: 1 of 4 validators is Byzantine                     |
| `simplex_failure.cfg`   | Network failures: message drops enabled                                       |
| `simplex_combined.cfg`  | Combined: Byzantine + network failures (worst case)                           |
| `simplex_weighted.cfg`  | Non-uniform validator weights with Byzantine                                  |
| `simplex.md`            | Protocol specification document                                               |
| `simplex_safety.tex`    | LaTeX write-up of the safety argument and TLC results                         |
| `simplex_safety.pdf`    | Rendered safety report — see for the full proof sketches and result tables    |

## Running Model Checking

### Prerequisites

Install TLA+ tools: https://github.com/tlaplus/tlaplus/releases

### Running TLC

```bash
# Quick safety check (fastest, no temporal)
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_quick.cfg simplex.tla

# Normal operation (safety + liveness)
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_normal.cfg simplex.tla

# Byzantine fault tolerance
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_byzantine.cfg simplex.tla

# Network failures
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_failure.cfg simplex.tla

# Combined worst-case
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_combined.cfg simplex.tla

# Non-uniform weights
java -XX:+UseParallelGC -jar tla2tools.jar -workers auto -config simplex_weighted.cfg simplex.tla
```

## Configurable Parameters

| Constant                      | Type     | Description                                  |
|-------------------------------|----------|----------------------------------------------|
| `NumValidators`               | Nat      | Number of validators (n)                     |
| `ByzantineSet`                | Set      | Which validators are Byzantine               |
| `Weight(_)`                   | Operator | Weight assignment per validator (w_i)        |
| `ValidSeq(_)`                 | Operator | Ledger validity predicate (§1.2)             |
| `LeaderWindowSize`            | Nat      | Slots per leader window (L)                  |
| `MaxSlot`                     | Nat      | Maximum slot number (bounds state space)     |
| `MaxMessages`                 | Nat      | Maximum in-flight messages                   |
| `EnableMessageDrop`           | Bool     | Adversarial phase: can drop messages         |
| `EnableByzantineEquivocation` | Bool     | Byzantine can propose conflicting candidates |
| `EnableByzantineWithholding`  | Bool     | Byzantine can withhold votes                 |

## Verification results

See [`simplex_safety.pdf`](./simplex_safety.pdf) for the full safety
write-up: lemma-by-lemma proofs, the invariants that mechanise them,
and the complexity comparison against PBFT / Tendermint / HotStuff.

### Long-running TLC run on `simplex_combined.cfg`

The combined configuration is the worst-case scenario — 4 validators
with 1 Byzantine (validator 4), Byzantine equivocation and withholding
both enabled, and message drops enabled — so the model checker has to
explore every interleaving of adversarial network behaviour together
with conflicting Byzantine proposals and votes. The run was carried
out on an Apple Silicon machine (Mac OS X 26.4.1 aarch64) with
12 worker threads pinned to 12 cores and a 12 743 MB JVM heap, using
TLC's checkpointing to resume the search across multiple sessions.

Run summary:

| Item                          | Value                                                                                     |
|-------------------------------|-------------------------------------------------------------------------------------------|
| TLC version                   | 2.19 (2024-08-08, rev 5a47802)                                                            |
| Host                          | Mac OS X 26.4.1 aarch64, Amazon Corretto 11.0.31                                          |
| Workers                       | 12 (12 cores)                                                                             |
| Heap                          | 12 743 MB + 64 MB offheap                                                                 |
| Search                        | breadth-first, fingerprint 25                                                             |
| Started                       | 2026-05-04 15:56:49                                                                       |
| Latest progress snapshot      | 2026-05-12 10:02:46 (≈ 7 days 18 hours, ~186 h elapsed)                                   |
| Last checkpoint resumed from  | `states/26-05-11-15-56-49/` (11 855 407 states examined, 10 741 053 on queue at recovery) |
| BFS depth reached             | 9                                                                                         |
| States generated (cumulative) | 74 772 689                                                                                |
| Distinct states found         | 20 789 904                                                                                |
| States left on queue          | 19 004 942                                                                                |
| Throughput (post-recovery)    | ~3.3 – 3.8 M states/min generated, ~400 – 550 K new distinct states/min                   |
| Safety invariant violations   | **0**                                                                                     |
| Liveness/temporal violations  | not applicable (`SafetySpec`, no fairness)                                                |

After roughly 7 days and 18 hours of wall-clock time — spanning
multiple checkpoint/resume cycles, with the final resume picking up
from `states/26-05-11-15-56-49/` (11.86 M states already examined
and 10.74 M still on the queue) — TLC has reported **20.8 million
distinct states** discovered against **74.8 million states
generated** in the combined Byzantine + drop configuration, without
finding a single violation of any of the invariants listed in
`simplex_combined.cfg`:

- `TypeOK`
- `FinalSkipExclusion` (Lemma 2.1)
- `UniqueNotarization` (Lemma 2.2)
- `FinalizationImpliesNotarization` (Lemma 2.3)
- `UniqueFinalization` (Lemma 2.4)
- `HonestVoteConsistency`, `HonestNotarUniqueness`,
  `HonestLeaderNoEquivocation` (§1.4 local rules)
- `SafetyInvariant`, `ChainPrefixSafety` (Theorem 2.6)
- `ByzantineFaultTolerance` (safety conditioned on `3f < W`)
- `OutputLogConsistency` (Corollary 2.7)

The constant-level `QuorumIntersectionLemma` is checked once at
startup; the run would have rejected the configuration immediately if
it failed.

The search is not yet exhausted — about 19 million states remain on
the queue and new distinct states are still being added at roughly
half a million per minute — so the combined-config state graph is
still larger than the cumulative explored set. The fact that no
invariant has fired through this much adversarial exploration
(20.8 million distinct states across roughly 7 days 18 hours of BFS,
including a checkpoint/resume cycle) is strong empirical evidence
that the protocol is safe under the modelled fault model. The
deductive argument in `simplex_safety.pdf` carries the rest: every
invariant tracks a lemma whose proof only depends on quorum
intersection and the rule guards in the spec.
