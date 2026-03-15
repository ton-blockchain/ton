# План граф-логирования консенсуса TON Simplex

## Правила (инварианты)

0. **Каждому консенсусному событию:**
   - должен быть создан уникальный `nodeId` (UUID или хэш-производное).
   - должна быть создана своя вершина в графе.
   - события связываются рёбрами согласно каузальной цепочке протокола:
     `Candidate → NotarizeVote → Cert → FinalizeVote → Block`.
   - `sessionId` = `ValidatorSessionId` из `Bus::session_id` — изолирует один прогон консенсуса.
   - Все события пишутся **fire-and-forget** через `GraphLogger` (NDJSON в файл), не блокируя consensus-поток.

1. **Расширять, не менять.**
   - Инструментация добавляется внутрь оригинального обработчика, не меняя логики.
   - Не создавать обёрток и дублёров событийных типов.
   - Если хендлер неизменяем — объяснить, прежде чем делать исключение.

2. **В граф записывается:**
   - `nodeId` — UUID (по умолчанию) или детерминированный из `{sessionId, slot, validatorIdx, voteType}`.
   - `sessionId` — `bus.session_id` (изолирует прогон).
   - `slot` — номер слота Simplex-консенсуса.
   - `tsMs` — `td::Timestamp::now().at() * 1000` (миллисекунды).
   - `validator` — `PeerValidatorId::value()` (индекс в наборе валидаторов).
   - `voteType` — `notarize | finalize | skip`.
   - `candidateId` — хэш кандидата (из `CandidateId`).
   - `outcome` — записывается только при отклонении (`CandidateReject`, misbehavior).

3. **Типы рёбер и сериализация.**
   Подробно: **[CYPHER_QUERIES.md#edge-types](CYPHER_QUERIES.md#edge-types)**.

4. **Graф-архитектура (`simulation/`).**

   ```
   validator/consensus/simplex/consensus.cpp    ← точки инструментации
   validator/consensus/block-producer.cpp       ←
   validator/consensus/block-accepter.cpp       ←
   simulation/
     GraphLogger.h / GraphLogger.cpp            ← C++ emit → NDJSON
     relay.mjs                                  ← NDJSON → AuraGraphReporter → Neo4j Aura
     ConsensusHarness.cpp                       ← мок actor runtime + Byzantine scenarios
     scenarios/
       equivocation.cpp
       message_withholding.cpp
       byzantine_leader.cpp
     CMakeLists.txt
   ```

   `GraphLogger` не обращается к Neo4j напрямую. Он пишет NDJSON-строки в файл (или stdout).  
   `relay.mjs` читает этот файл и вызывает `AuraGraphReporter.writeEntries()` из teleGraph.  
   Формат совместим с `AuraLogEvent`.

5. **Логировать fire-and-forget.** Consensus-поток не ждёт записи.

6. **Глобальный флаг отключения.**
   - `GRAPH_LOGGING_ENABLED=0` в `.env` или переменная окружения — запись в файл отключена.
   - Проверяется один раз при инициализации `GraphLogger`.

---

## Точки инструментации (приоритет)

| Файл | Место | Событие | Уязвимость/аномалия |
|---|---|---|---|
| `simplex/consensus.cpp` | `handle(CandidateReceived)` | `Candidate` → `ReceivedByValidator` | Byzantine leader — разные кандидаты разным валидаторам |
| `simplex/consensus.cpp` | `try_notarize()` — после validate | `NotarizeVote` emitted | Equivocation: два notarize-vote за разные candidateId в одном slot |
| `simplex/consensus.cpp` | `handle(NotarizationObserved)` | `Cert` observed | Dual-cert: два quorum-сертификата за один slot |
| `simplex/consensus.cpp` | `alarm()` — SkipVote emit | `SkipVote` emitted | Message withholding: propose без vote дольше timeout |
| `simplex/consensus.cpp` | `handle(FinalizationObserved)` | `Block` finalized | State divergence: разные блоки финализированы в одном slot |
| `block-producer.cpp` | propose emitted | `Propose` event | Forking: несколько propose в одном window |
| `block-accepter.cpp` | block accepted | `Accept` event | End-to-end latency от collate до accept |

---

## Схема узлов и рёбер

```
(Validator)-[:propose {slot, tsMs}]->(Candidate)
(Candidate)<-[:notarize {slot, tsMs}]-(Validator)
(Candidate)-[:cert {slot, weight, tsMs}]->(Cert)
(Cert)<-[:finalize {slot, tsMs}]-(Validator)
(Cert)-[:accepted {slot, tsMs}]->(Block)
(Candidate)-[:parent {parentSlot}]->(PrevBlock)
(Validator)-[:skip {slot, tsMs}]->(SkipEvent)
```

---

## Шаги (прогон инструментации)

1. Определить актуальную точку инструментации:
   - Сначала смотрим в контексте чата.
   - Если не найдено — запрашиваем граф через MCP: [#frontier](CYPHER_QUERIES.md#frontier).
     Берём узел с наименьшей `depth`, у которого нет исходящих рёбер нужного типа.

2. Модифицировать обработчик в `simplex/consensus.cpp`:
   - Вызвать `GraphLogger::emit(eventType, props)` в нужном месте.
   - Props включают минимальный набор полей из п. 2.

3. Проверить компиляцию: `cmake --build ./build --target simulation -j4`.

4. Запустить сценарий: **[SIMULATION.md](SIMULATION.md)**.

5. Запустить `node simulation/relay.mjs` — подробнее: **[CPP_LOGGING.md](CPP_LOGGING.md)**.

6. Агент запрашивает граф через MCP (см. [MCP_NEO4J_AURA.md](MCP_NEO4J_AURA.md)) и проверяет нужные рёбра/узлы:
   - Изолировать последний прогон: [#last-session](CYPHER_QUERIES.md#last-session).
   - Проверить аномалии: [#equivocation](CYPHER_QUERIES.md#equivocation), [#dual-cert](CYPHER_QUERIES.md#dual-cert).
   - Очистить граф при необходимости: [#clean](CYPHER_QUERIES.md#clean).

7. Если сценарий не воспроизводит аномалию — доработать Byzantine injection в [SIMULATION.md](SIMULATION.md)
   и добавить проверку в Cypher.
