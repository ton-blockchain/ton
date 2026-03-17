# Уязвимости TON Consensus — соответствие contest.com/docs/TonConsensusChallenge

Источник идей: `firstChat.md` (архив первого чата).
Скоуп контеста: баги в `validator/consensus/` (кроме `validator/consensus/null`),
а также QUIC, TwoStep broadcast и ресурсное истощение.

---

## ✅ В скоупе контеста

### 1. Equivocation (двойное голосование)

**Описание:** Byzantine валидатор подписывает два разных `NotarizeVote` за один и тот же `slot`
с разными `candidateId`.

**Сценарий:** `ConsensusHarness --scenario equivocation`

**Детектирование (Cypher):**
```cypher
MATCH (v:Validator)-[:notarize {slot: $slot}]->(c1:Candidate),
      (v)-[:notarize {slot: $slot}]->(c2:Candidate)
WHERE c1.candidateId <> c2.candidateId
RETURN v, c1, c2
```

**Классификация контеста:** Consensus implementation bug — safety violation.

---

### 2. Liveness attack / Message withholding

**Описание:** Лидер генерирует кандидата, но не доставляет `propose` валидаторам.
Все валидаторы по таймауту шлют `SkipVote`, слот пропускается — прогресс заблокирован.

**Сценарий:** `ConsensusHarness --scenario message_withholding`

**Детектирование (Cypher):**
```cypher
// Слоты без NotarizeCert — только SkipEvent
MATCH (sk:SkipEvent)
WHERE NOT (:Cert {slot: sk.slot, sessionId: sk.sessionId})
RETURN sk.slot ORDER BY sk.slot
```

**Классификация контеста:** Consensus bug — liveness violation.

---

### 3. Byzantine leader / Split propose

**Описание:** Лидер шлёт разным группам валидаторов разные кандидаты (`cand_A`, `cand_B`)
за один `slot`. Ни одна группа не набирает кворум → слот пропускается.

**Сценарий:** `ConsensusHarness --scenario byzantine_leader`

**Детектирование (Cypher):**
```cypher
MATCH (v:Validator)-[:propose]->(c1:Candidate),
      (v)-[:propose]->(c2:Candidate)
WHERE c1.slot = c2.slot AND c1.candidateId <> c2.candidateId
RETURN v, c1, c2
```

**Классификация контеста:** Consensus bug — safety + liveness violation.

---

### 4. State divergence

**Описание:** Два валидатора финализируют разные блоки в одном `slot` — нарушение safety.
Возникает при Byzantine quorum или ошибке в логике сертификации.

**Детектирование (Cypher):**
```cypher
MATCH (b1:Block {slot: $slot}), (b2:Block {slot: $slot})
WHERE b1.candidateId <> b2.candidateId
RETURN b1, b2
```

**Классификация контеста:** Consensus bug — critical safety violation.

---

### 5. Amnesia attack

**Описание:** Валидатор «забывает» ранее выданный `NotarizeVote` (локed кандидат)
и голосует за другой кандидат в том же `slot`. Аналог surround vote в Ethereum.

**Детектирование (Cypher):**
```cypher
// Validator голосовал notarize за cand_A, потом за cand_B в том же slot
MATCH (v:Validator)-[n1:notarize]->(c1:Candidate),
      (v)-[n2:notarize]->(c2:Candidate)
WHERE n1.slot = n2.slot AND c1 <> c2 AND n1.tsMs < n2.tsMs
RETURN v, c1, c2, n1.tsMs AS first_vote, n2.tsMs AS second_vote
```

**Классификация контеста:** Consensus bug — safety violation (lock bypass).

---

### 6. Message reordering

**Описание:** `NotarizeVote` доставляется до `Propose` в одном `slot`.
Если реализация не защищена от out-of-order, возможна некорректная обработка.

**Детектирование (Cypher):**
```cypher
MATCH (v:Validator)-[p:propose]->(c:Candidate),
      (v2:Validator)-[n:notarize]->(c)
WHERE n.tsMs < p.tsMs
RETURN c.slot, p.tsMs AS propose_ts, n.tsMs AS vote_ts
```

**Классификация контеста:** Consensus bug — liveness / incorrect state handling.

---

### 7. Resource exhaustion — linear message flood

**Описание:** Byzantine валидатор шлёт каждому честному узлу линейное по времени число
сообщений (напр. дублированные `BroadcastVote`). Суммарно: O(N·t) сообщений → перегрузка
bandwidth (~1 Gbps на типичном железе).

**Проверка:** Измерить число входящих сообщений на честный узел при Byzantine отправителе
за N слотов. Должно быть O(1) на слот, не O(слоты).

**Классификация контеста:** Resource-exhaustion bug — явно в скоупе (`linear-in-time messages`).

---

### 8. Resource exhaustion — superlinear processing

**Описание:** Обработка одного входящего сообщения занимает O(N²) по числу валидаторов.
При N=100 (лимит репро) это 10 000 операций на сообщение — реалистичный DoS.

**Классификация контеста:** Resource-exhaustion bug (superlinear resource growth).

---

## ❌ Вне скоупа контеста

| Идея из firstChat.md | Причина исключения |
|---|---|
| Memory corruption при десериализации `td::BufferSlice` | Отклонено как «local environment attack» |
| Dead code / unreachable states в state machine | Не воспроизводимо, спекулятивно |
| Статический анализ AST через граф | Unverifiable без конкретного exploit |
| Timing attack ровно в окно `[timeout-ε, timeout+ε]` | Требует доступа к сети, вне attack model |
| Дифференциальное тестирование против LibraBFT | Результат — расхождение реализаций, не баг TON |

---

## Приоритет прогонов

```
1. [CRITICAL] State divergence         — нарушение safety, максимальный импакт
2. [HIGH]     Equivocation             — уже воспроизведён в simulation
3. [HIGH]     Byzantine leader         — уже воспроизведён в simulation
4. [HIGH]     Resource exhaustion      — linear message flood, измеримо
5. [MEDIUM]   Message withholding      — уже воспроизведён, liveness
6. [MEDIUM]   Amnesia attack           — нужна модификация сценария
7. [LOW]      Message reordering       — зависит от реализации буфера
```

---

## Лимиты воспроизведения (из условий контеста)

- Consensus bugs: **< 10 000 слотов**, **≤ 100 валидаторов**
- Resource exhaustion: должен быть реалистичный DoS на стандартном железе
- Submission: скрипт или архив + title + impact + description + reproduction
