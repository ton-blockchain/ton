# Cypher-запросы для граф-логирования TON Simplex consensus

Логирующий C++-код (для понимания схемы) — `simulation/GraphLogger.h`.

---

## #edge-types — Типы рёбер

| Ребро | Источник → цель | Условие |
|---|---|---|
| `[:propose]` | `Validator` → `Candidate` | лидер эмитит кандидата через `OurLeaderWindowStarted` |
| `[:receive]` | `Candidate` → `Validator` | валидатор получил кандидата (`CandidateReceived`) |
| `[:notarize]` | `Validator` → `Candidate` | валидатор эмитит `NotarizeVote` после успешной валидации |
| `[:skip]` | `Validator` → `SkipEvent` | валидатор эмитит `SkipVote` по таймауту |
| `[:cert]` | `Candidate` → `Cert` | набрана нотаризационная коллекция (`NotarizationObserved`) |
| `[:finalize]` | `Validator` → `Cert` | валидатор эмитит `FinalizeVote` |
| `[:accepted]` | `Cert` → `Block` | блок принят менеджером (`FinalizeBlock`) |
| `[:parent]` | `Candidate` → `Block` | ссылка кандидата на родительский блок |
| `[:misbehavior]` | `Candidate` → `Validator` | зафиксировано Byzantine поведение (`MisbehaviorReport`) |

Свойства рёбер:
- `tsMs` — момент события на эмитирующей стороне
- `slot` — номер слота Simplex
- `weight` — суммарный вес для `[:cert]` / `[:accepted]`

---

## #serialization — Сериализация

- `tsMs` — всегда в миллисекундах (`td::Timestamp::now().at() * 1000`).
- `candidateId` — hex-строка из `CandidateId`.
- `validatorIdx` — `PeerValidatorId::value()` (size_t → int64).
- Объекты → JSON-строка до 500 символов.
- Примитивы → как есть.

---

## #last-session — Дерево последнего прогона

Находит корневой узел с наибольшим `tsMs` и обходит всё дерево вниз.

```cypher
MATCH (root {depth: 0})
WITH root ORDER BY root.tsMs DESC LIMIT 1
MATCH p = (root)-[:propose|receive|notarize|skip|cert|finalize|accepted|parent*0..]->(n)
RETURN
  labels(n)[0]   AS type,
  n.slot         AS slot,
  n.tsMs         AS ts,
  n.validatorIdx AS validator
ORDER BY n.slot, n.tsMs
```

---

## #by-session — Дерево по sessionId

```cypher
MATCH (root {depth: 0, sessionId: $sid})
MATCH p = (root)-[:propose|receive|notarize|skip|cert|finalize|accepted|parent*0..]->(n)
RETURN
  labels(n)[0]   AS type,
  n.slot         AS slot,
  n.tsMs         AS ts,
  n.validatorIdx AS validator
ORDER BY n.slot, n.tsMs
```

> `$sid` — значение `bus.session_id` из конфига или из свойства любого узла в Neo4j Browser.

---

## #frontier — Листья дерева (кандидаты для следующей инструментации)

Возвращает все узлы последнего прогона без исходящих рёбер трассировки —
«граница» для расширения на следующей итерации. Сортировка по `depth`.

```cypher
MATCH (root {depth: 0})
WITH root ORDER BY root.tsMs DESC LIMIT 1
MATCH (root)-[:propose|receive|notarize|skip|cert|finalize|accepted|parent*0..]->(n)
WHERE NOT (n)-[:propose|receive|notarize|skip|cert|finalize|accepted|parent]->()
RETURN
  labels(n)[0] AS type,
  n.slot       AS slot,
  n.depth      AS d,
  n.nodeId     AS id
ORDER BY n.depth, n.slot
```

---

## #equivocation — Equivocation: два vote за разные candidateId в одном slot

BFT-аномалия: один валидатор подписал два `NotarizeVote` с разными `candidateId` в одном `slot`.

```cypher
MATCH (v:Validator)-[r1:notarize]->(c1:Candidate),
      (v)-[r2:notarize]->(c2:Candidate)
WHERE r1.slot = r2.slot
  AND c1.candidateId <> c2.candidateId
  AND r1.sessionId = r2.sessionId
RETURN
  v.validatorIdx     AS validator,
  r1.slot            AS slot,
  c1.candidateId     AS candidate1,
  c2.candidateId     AS candidate2,
  r1.tsMs            AS ts1,
  r2.tsMs            AS ts2
ORDER BY slot
```

---

## #dual-cert — Dual-cert: два quorum-сертификата за один slot

```cypher
MATCH (c1:Candidate)-[r1:cert]->(cert1:Cert),
      (c2:Candidate)-[r2:cert]->(cert2:Cert)
WHERE r1.slot = r2.slot
  AND c1.candidateId <> c2.candidateId
  AND cert1.sessionId = cert2.sessionId
RETURN
  r1.slot        AS slot,
  c1.candidateId AS candidate1,
  c2.candidateId AS candidate2,
  r1.tsMs        AS ts1,
  r2.tsMs        AS ts2
```

---

## #withholding — Message withholding: SkipVote без предшествующего кандидата

Валидатор сделал skip, не получив кандидата — признак удержания сообщений лидером.

```cypher
MATCH (root {depth: 0})
WITH root ORDER BY root.tsMs DESC LIMIT 1

MATCH (v:Validator)-[r:skip]->(s:SkipEvent)
WHERE s.sessionId = root.sessionId
  AND NOT EXISTS {
    MATCH (c:Candidate)
    WHERE c.slot = r.slot AND c.sessionId = root.sessionId
  }
RETURN
  v.validatorIdx AS validator,
  r.slot         AS slot,
  r.tsMs         AS ts
ORDER BY slot
```

---

## #amnesia — Amnesia (post-crash equivocation): повторный vote после перезапуска

Валидатор проголосовал за `c2` позже чем за `c1` в том же `slot` — признак того, что WAL не сохранил первый голос до краша.

```cypher
MATCH (v:Validator)-[n1:notarize]->(c1:Candidate),
      (v)-[n2:notarize]->(c2:Candidate)
WHERE n1.slot = n2.slot
  AND c1 <> c2
  AND n1.tsMs < n2.tsMs
RETURN
  v.validatorIdx AS validator,
  n1.slot        AS slot,
  c1.candidateId AS first,
  c2.candidateId AS second,
  n1.tsMs        AS ts1,
  n2.tsMs        AS ts2
ORDER BY slot
```

---

## #out-of-order — Out-of-order: notarize раньше propose

Голос `notarize` зафиксирован раньше, чем `propose` от лидера — признак некорректной буферизации или message reordering.

```cypher
MATCH (v:Validator)-[p:propose]->(c:Candidate),
      (v2:Validator)-[n:notarize]->(c)
WHERE n.tsMs < p.tsMs
RETURN
  c.slot    AS slot,
  p.tsMs    AS proposeTs,
  n.tsMs    AS notarizeTs
ORDER BY slot
```

---

## #byzantine-leader — Byzantine leader: разные кандидаты разным валидаторам в одном slot

```cypher
MATCH (c1:Candidate)<-[r1:receive]-(v1:Validator),
      (c2:Candidate)<-[r2:receive]-(v2:Validator)
WHERE c1.leaderIdx = c2.leaderIdx
  AND c1.slot = c2.slot
  AND c1.candidateId <> c2.candidateId
  AND c1.sessionId = c2.sessionId
RETURN
  c1.leaderIdx   AS leader,
  c1.slot        AS slot,
  c1.candidateId AS sentTo1,
  c2.candidateId AS sentTo2,
  v1.validatorIdx AS validator1,
  v2.validatorIdx AS validator2
ORDER BY slot
```

---

## #skip-rate — Доля слотов с SkipVote (liveness metric)

```cypher
MATCH (root {depth: 0})
WITH root ORDER BY root.tsMs DESC LIMIT 1

OPTIONAL MATCH (v:Validator)-[rs:skip]->(s:SkipEvent)
WHERE s.sessionId = root.sessionId

OPTIONAL MATCH (v2:Validator)-[rn:notarize]->(c:Candidate)
WHERE c.sessionId = root.sessionId

RETURN
  count(DISTINCT rs.slot) AS skippedSlots,
  count(DISTINCT rn.slot) AS notarizedSlots
```

---

## #latency — Latency от collate до accept (по slot)

```cypher
MATCH (root {depth: 0})
WITH root ORDER BY root.tsMs DESC LIMIT 1

MATCH (b:Block)<-[:accepted]-(cert:Cert)<-[:cert]-(c:Candidate)
WHERE b.sessionId = root.sessionId

MATCH (v:Validator)-[rp:propose]->(c)

RETURN
  c.slot                       AS slot,
  rp.tsMs                      AS proposeTs,
  b.tsMs                       AS acceptTs,
  (b.tsMs - rp.tsMs)           AS latencyMs
ORDER BY slot
```

---

## #all-nodes — Все узлы (отладка)

```cypher
MATCH (n)
RETURN
  labels(n)[0]   AS type,
  n.slot         AS slot,
  n.sessionId    AS sid,
  n.tsMs         AS ts
ORDER BY n.slot, n.tsMs
```

---

## #clean — Очистка графа

```cypher
MATCH (n) DETACH DELETE n
```

> Используй с осторожностью. Для изоляции конкретного прогона лучше фильтруй по `sessionId`.

---

## Аномалии ConsensusHarness — верифицированные запросы

Запросы проверены на данных `ConsensusHarness --scenario all` (4 сессии, 10 слотов каждая).
Трасса: `simulation/trace.ndjson` → `relay.mjs --clear`.

| Сессия | Сценарий | finalizedBlocks | skippedSlots |
|--------|----------|-----------------|--------------|
| `dc1938bb` | equivocation | 10 | 0 |
| `33355d2e` | message_withholding | 9 | 1 |
| `57e62334` | byzantine_leader | 7 | 3 |
| `bb1a73fc` | notarize_skip_split | 10 | 0 |

---

### #notarize-skip-split — Notarize+Skip от одного валидатора в одном слоте

Детектирует дыру в `pool.cpp check_invariants()`: пара Notarize+Skip не проверяется.
Ожидаемый результат: только сессия `notarize_skip_split`, validator 0, слоты 0–9.

```cypher
MATCH (v:Validator)-[n:notarize]->(c:Candidate)
MATCH (v)-[sk:skip]->(se:SkipEvent)
WHERE n.slot = sk.slot
RETURN v.sessionId AS session, v.validatorIdx AS validator, n.slot AS slot
ORDER BY session, validator, slot
```

**Результат (2026-03-17):**

| session | validator | slot |
|---------|-----------|------|
| `bb1a73fc-edac-43b5-a476-eced1b5bed57` | 0 | 0 |
| `bb1a73fc-edac-43b5-a476-eced1b5bed57` | 0 | 1 |
| … | 0 | 2–9 |

Только сессия `notarize_skip_split` — шума нет.

---

### #double-notarize — Double-notarize: два голоса за разные candidateId в одном слоте

Ожидаемый результат: только сессия `equivocation`, validator 0, слоты 0–9.

```cypher
MATCH (v:Validator)-[n1:notarize]->(c1:Candidate)
MATCH (v)-[n2:notarize]->(c2:Candidate)
WHERE n1.slot = n2.slot AND c1.nodeId < c2.nodeId
RETURN v.sessionId AS session, v.validatorIdx AS validator,
       n1.slot AS slot, c1.candidateId AS cand1, c2.candidateId AS cand2
ORDER BY session, validator, slot
```

**Результат (2026-03-17):**

| session | validator | slot | cand1 | cand2 |
|---------|-----------|------|-------|-------|
| `dc1938bb-2a75-4eaf-aeb8-81c237eb95c3` | 0 | 0 | `cand:0` | `cand:0:B` |
| `dc1938bb-2a75-4eaf-aeb8-81c237eb95c3` | 0 | 1 | `cand:1` | `cand:1:B` |
| … | 0 | 2–9 | `cand:N` | `cand:N:B` |

---

### #cert-with-skip — Safety violation: NotarizeCert сформирован, но участник кворума голосовал Skip

Показывает случаи, где один из нотаризирующих валидаторов одновременно слал SkipVote на тот же слот.
Ожидаемый результат: только сессия `notarize_skip_split`, validator 0, слоты 0–9.

```cypher
MATCH (cert:Cert)<-[:cert]-(:Candidate)<-[:notarize {slot: cert.slot}]-(v:Validator)
MATCH (v)-[:skip {slot: cert.slot}]->(:SkipEvent)
RETURN cert.sessionId AS session, v.validatorIdx AS validator,
       cert.slot AS slot, cert.candidateId AS certifiedCand
ORDER BY session, slot
```

**Результат (2026-03-17):**

| session | validator | slot | certifiedCand |
|---------|-----------|------|---------------|
| `bb1a73fc-edac-43b5-a476-eced1b5bed57` | 0 | 0 | `cand:0` |
| `bb1a73fc-edac-43b5-a476-eced1b5bed57` | 0 | 1 | `cand:1` |
| … | 0 | 2–9 | `cand:N` |
