# Инварианты TON Simplex Consensus (∀/∃/⇒/⊥/◇)

---

## #equivocation — Equivocation

```
∀ v ∈ Validators, ∀ s ∈ Slots, ∀ T ∈ {notarize, finalize}:
  |{c | voted_T(v, c, s)}| ≤ 1

∀ v, s:
  voted_notarize(v, c, s) ∧ voted_skip(v, s) ⇒ ⊥

Нарушение ⇒ MisbehaviorReport(v) обязателен.
```

---

## #withholding — Liveness attack / Message withholding

```
∀ s ∈ Slots, L = leader(s):
  Correct(L) ⇒ ◇ Propose(L, s)           [liveness]

∀ s: alarm(s) fires ⇒ ¬voted_notar(s)    [корректность таймаута]

¬Propose(L, s) ∧ t > T_timeout ⇒
  ∀ v: SkipVote(v, s) eventually
```

---

## #byzantine-leader — Byzantine leader / Split propose

```
∀ L = leader(s), ∀ s ∈ Slots:
  |{c | Propose(L, c, s)}| ≤ 1

∃ c₁ ≠ c₂: Propose(L, c₁, s) ∧ Propose(L, c₂, s)
  ⇒ MisbehaviorReport(L) обязателен
```

---

## #state-divergence — State divergence

```
∀ s ∈ Slots:
  |{c | FinalizeCert(c, s)}| ≤ 1          [safety]

FinalizeCert(c₁, s) ∧ FinalizeCert(c₂, s)
  ⇒ c₁ = c₂

SkipCert(s) ∧ FinalizeCert(c, s) ⇒ ⊥
```

---

## #amnesia — Amnesia attack

```
∀ v ∈ Validators, ∀ s ∈ Slots, ∀ c:
  broadcast(vote(v, c, s))
    ⇒ persisted_to_db(v, c, s)            [до broadcast]

restart(v) ⇒ state(v) = load_from_db(v)

¬persisted(v, c, s) ∧ restart(v)
  ⇒ ¬voted_notarize(v, c, s) after restart
```

---

## #out-of-order — Message reordering

```
∀ v, s:
  recv(vote, s) before recv(propose, s)
    ⇒ vote deferred or rejected          [no out-of-order accept]

bootstrap_replay(votes) with conflict(v, s)
  ⇒ MisbehaviorReport(v) обязателен

tolerate_conflicts(v) = true ⇒ log_only ≠ suppress
```

---

## #linear-flood — Resource exhaustion — linear flood

```
∀ t ∈ Time:
  |requests_| = O(|Validators|)           [bounded queue]

∀ v_byzantine, ∀ honest h:
  msgs_received(h, t) = O(1) per slot    [per-validator, per-slot]

retries(candidateId) ≤ R_max = const
```

---

## #superlinear — Resource exhaustion — superlinear

```
∀ s ∈ Slots:
  |notarize_weight[s]| = O(1)             [один candidateId per slot]

cert_creation_cost(s) = O(|Validators|)

total_processing_cost(msgs)
  = O(|msgs| · |Validators|)             [линейно, не O(N²)]
```
