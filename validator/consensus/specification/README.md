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
| `simplex_normal.cfg`    | Normal operation: all honest, reliable network (safety + liveness)            |
| `simplex_byzantine.cfg` | Byzantine fault tolerance: 1 of 4 validators is Byzantine                     |
| `simplex_failure.cfg`   | Network failures: message drops enabled                                       |
| `simplex_combined.cfg`  | Combined: Byzantine + network failures (worst case)                           |
| `simplex_weighted.cfg`  | Non-uniform validator weights with Byzantine                                  |
| `Simplex.md`            | Protocol specification document                                               |

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
