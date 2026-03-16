# ConsensusHarness — Byzantine сценарии симуляции

## Архитектура (`simulation/ConsensusHarness.cpp`)

`ConsensusHarness` — мок tdactor runtime, запускающий несколько экземпляров `ConsensusImpl`
в одном процессе с управляемым network layer.

```
ConsensusHarness
├── N × ValidatorNode (оборачивает Bus + ConsensusImpl)
│     └── Bus::publish<BroadcastVote> → перехватывается NetworkMock
├── NetworkMock
│     ├── честная доставка (shuffle + latency)
│     └── Byzantine injection (drop, duplicate, substitute)
└── GraphLogger::instance() — общий для всех узлов
```

**Параметры по умолчанию:**
- `N = 4` валидатора (f=1, порог нотаризации ≥ 3)
- `slots_per_leader_window = 4`
- `first_block_timeout_ms = 1000`

---

## Сценарий 1 — Equivocation (`scenarios/equivocation.cpp`)

**Условие:** Byzantine валидатор (индекс 0) подписывает два `NotarizeVote` с разными `candidateId`
за один и тот же `slot`.

**Настройка:**
```cpp
harness.set_byzantine(0, ByzantineMode::DoubleVote);
harness.run_slots(10);
```

**Ожидаемые узлы в графе:**
- Два `Candidate` с одинаковым `slot`, разными `candidateId`.
- Два ребра `[:notarize]` от одного `Validator` (idx=0) к разным `Candidate`.

**Проверка:** запрос [#equivocation](CYPHER_QUERIES.md#equivocation) должен вернуть ≥ 1 строку.

---

## Сценарий 2 — Message withholding (`scenarios/message_withholding.cpp`)

**Условие:** лидер генерирует кандидата, но `NetworkMock` не доставляет его ни одному валидатору.
Все валидаторы по таймауту шлют `SkipVote`.

**Настройка:**
```cpp
harness.set_network_rule(0 /* leader_slot */, NetworkRule::DropPropose);
harness.run_slots(5);
```

**Ожидаемые узлы в графе:**
- `Candidate` с `slot=0` есть (propose зафиксирован у лидера).
- Нет рёбер `[:receive]` или `[:notarize]` от других валидаторов.
- Есть рёбра `[:skip]` от валидаторов 1–3.

**Проверка:** запрос [#withholding](CYPHER_QUERIES.md#withholding) должен вернуть ≥ 1 строку.

---

## Сценарий 3 — Byzantine leader (`scenarios/byzantine_leader.cpp`)

**Условие:** лидер (idx=0) эмитит два разных кандидата (`cand_A` и `cand_B`) для разных
подмножеств валидаторов в одном `slot`.

**Настройка:**
```cpp
harness.set_byzantine(0, ByzantineMode::SplitPropose{.group_a = {1, 2}, .group_b = {3}});
harness.run_slots(8);
```

**Ожидаемые узлы в графе:**
- Два `Candidate` с одинаковым `leaderIdx=0` и одинаковым `slot`, разными `candidateId`.
- Рёбра `[:receive]` ведут к разным `Candidate` для разных валидаторов.

**Проверка:** запрос [#byzantine-leader](CYPHER_QUERIES.md#byzantine-leader) должен вернуть ≥ 1 строку.

---

## Запуск сценария

```bash
# Из корня tonGraph/
./build/simulation/ConsensusHarness --scenario equivocation
./build/simulation/ConsensusHarness --scenario message_withholding
./build/simulation/ConsensusHarness --scenario byzantine_leader

# С расширенным логированием:
./build/simulation/ConsensusHarness --scenario equivocation --log-level DEBUG
```

Бинарь записывает `simulation/trace.ndjson`.  
После завершения запусти `node simulation/relay.mjs` для отправки в Neo4j.

---

## Полный workflow прогона

```
1. cmake --build build --target simulation -j4
2. rm simulation/trace.ndjson             # очистка предыдущего трейса
3. ./build/simulation/ConsensusHarness --scenario equivocation
4. node simulation/relay.mjs              # отправка трейса в Neo4j
5. MCP-запрос через агента:
     [#last-session](CYPHER_QUERIES.md#last-session)   — весь граф прогона
     [#equivocation](CYPHER_QUERIES.md#equivocation)   — поиск аномалии
```

---

## CI

Отдельного CI-шага для simulation нет.  
Для локальной регрессии используй:

```bash
cmake --build build --target simulation -j4
./build/simulation/ConsensusHarness --scenario all --assert-anomalies
# Возвращает non-zero exit code, если аномалии не найдены
```
