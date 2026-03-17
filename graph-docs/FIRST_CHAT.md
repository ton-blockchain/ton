# Первый чат: графовое логирование для поиска уязвимостей TON Consensus

### Что уже есть и что применимо

Инфраструктура (GRAPH_LOGGING_PLAN.md, WORKER_LOGGING.md, CYPHER_QUERIES.md) даёт:

| Возможность | Применимость к consensus |
|---|---|
| **Дерево вызовов** с `depth`, `parentNodeId` | Отслеживание цепочки `validate_block → check_signatures → apply_state` |
| **Edge types** (`async`, `postMessage`, `event`) | Моделирование message passing между валидаторами (`[:vote]`, `[:propose]`, `[:commit]`) |
| **Store-узлы** (`[:init]`, `[:set]`, `[:get]`) | Отслеживание мутаций состояния консенсуса (кто и когда менял `validator_set`, `block_candidate`) |
| **sessionId изоляция** | Изоляция отдельных раундов консенсуса |
| **Cypher-запросы** (`#frontier`, `#store-history`) | Поиск аномалий в графе: необычные пути, отсутствующие рёбра, циклы |
| **Lane/thread разметка** | Визуализация параллельных validator threads |


### Практический план адаптации

Код TON consensus написан на C++ (`ton/validator/consensus/`). Три подхода:

**A. Инструментация через обёртки (рекомендуется для контеста)**
- Написать тонкий C++ logger по аналогии с `WorkerGraphReporter` — буферизует события, сериализует в JSON
- Отправлять буфер в Neo4j через HTTP API (или через существующий `AuraGraphReporter` как relay)
- Cypher-запросы из CYPHER_QUERIES.md работают as-is после адаптации лейблов



**C. Статический анализ кода через граф**
- Парсить C++ AST → граф вызовов в Neo4j
- Искать паттерны уязвимостей: unchecked return values, missing validations, race conditions
- Это ближе к CodeQL, но с Neo4j инфраструктурой

### Ограничения

- **Текущий `AuraGraphReporter`** привязан к browser runtime (`self.constructor.name`, webpack `APP_REVISION`) — нужна адаптация под C++/standalone
- **Масштаб**: consensus fuzzing генерирует миллионы событий — нужен batch-optimized pipeline (существующий `flushLoop` с batch size 500 — хороший старт, но может потребовать увеличения)
- **Детерминизм**: для воспроизведения бага нужно логировать seed рандома и порядок сообщений

### Вывод

Архитектура графового логирования — готовый фреймворк для **динамического анализа протоколов**. Для TON Consensus Challenge наиболее практичный путь — **вариант B** (симуляция + fuzzing + Cypher-анализ аномалий), потому что:
1. Вся Neo4j инфра уже работает
2. Cypher-запросы для поиска аномалий пишутся за минуты
3. Визуализация в Neo4j Browser даёт мгновенный insight в topology атаки

---


### 1. Готовые тест-кейсы в самом репозитории TON

```
ton/
  validator/tests/
  validator/consensus/  ← state machine, message handlers
  crypto/test/
```

В репозитории есть `ton/validator/tests/test-validator.cpp` и аналоги — первый источник: уже описанные сценарии, которые надо прогнать **с инструментацией**, чтобы получить baseline-граф нормального исполнения. Baseline — точка отсчёта. Любое отклонение от него в последующих прогонах — кандидат на уязвимость.

### 2. Fuzzing с libFuzzer / AFL++ (главный источник)

TON использует C++ — идеально ложится на coverage-guided fuzzing:

```cpp
// Обёртка для libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // Десериализуем данные как ValidatorSessionMessage
  // → прогоняем через state machine
  // → граф-логгер пишет trace в Neo4j
  return 0;
}
```

**Что фаззить конкретно:**

| Точка входа | Класс уязвимостей |
|---|---|
| `ValidatorSessionImpl::process_message()` | Неверная обработка неизвестных типов сообщений |
| `SentBlocks::approve_block()` | Race / double-approve |
| `RoundAttempt::make_vote()` | Equivocation detection bypass |
| Десериализация `td::BufferSlice` | Memory corruption при malformed input |

Corpus для fuzzer берётся из:
- Перехваченных реальных сообщений с testnet (через ADNL-трейс)
- Сгенерированных мутаций из существующих unit-тестов


### 4. Дифференциальное тестирование против эталонной реализации

HotStuff (на котором основан TON consensus) имеет несколько реализаций:
- **LibraBFT / DiemBFT** (Rust) — эталон
- **Tendermint** (Go) — другая BFT-семья, для сравнения поведения corner cases

Взять одинаковый входной сценарий → прогнать через TON и через эталон → сравнить графы в Neo4j:

```cypher
// Найти состояния, достигнутые в TON но не в эталоне
MATCH (n:ConsensusState {implementation: 'ton'})
WHERE NOT EXISTS {
  MATCH (m:ConsensusState {implementation: 'reference', stateHash: n.stateHash})
}
RETURN n
```

### 5. Академические CVE-паттерны (конкретные баги в BFT)

| CVE / Баг | Протокол | Применимость к TON |
|---|---|---|
| **Tendermint liveness bug** (2019) | Tendermint | Высокая — схожая round-change логика |
| **HotStuff safety violation under network delay** | HotStuff | Прямая — TON основан на HotStuff |
| **PBFT view-change amplification** | PBFT | Средняя — другой механизм смены лидера |
| **Casper FFG surround vote** | Ethereum | Низкая — разная модель finality |

### 6. TLA+ / Alloy спецификации

Если у TON есть формальная спецификация (или если написать её по коду), model checker генерирует **все достижимые состояния** автоматически. Каждая контрпримерная трасса из TLA+ — один прогон для граф-логгера.

### Итоговая приоритизация

```
1. [БЫСТРО] Прогнать существующие unit-тесты TON с инструментацией → baseline граф
2. [БЫСТРО] Вручную воспроизвести 3-4 BFT-сценария из таблицы выше → аномалии
3. [СРЕДНЕ] Настроить libFuzzer на process_message() → корпус интересных входов
4. [ДОЛГО]  Дифференциальное тестирование против LibraBFT
```

Самый быстрый способ найти что-то конкретное — **шаг 2**: написать adversarial harness на C++, где один из `f` валидаторов ведёт себя по-Byzantine согласно сценарию из таблицы, прогнать, посмотреть граф в Neo4j Browser.

---

## Архитектура: форк + simulation/ модуль

### Структура репозитория

```
ton/ (форк)
  validator/consensus/     ← инструментируем здесь
  simulation/              ← новая папка
    ConsensusHarness.cpp   ← мок actor runtime + Byzantine scenarios
    GraphLogger.cpp        ← C++ аналог WorkerGraphReporter
    GraphLogger.h
    scenarios/
      equivocation.cpp
      message_withholding.cpp
      byzantine_leader.cpp
    CMakeLists.txt
```

### GraphLogger.h

```cpp
#pragma once
#include <string>
#include <vector>

struct GraphContext {
  std::string nodeid;
  std::string parentNodeid;  // "" если root
  int depth;
};

class GraphLogger {
public:
  static GraphContext logCall(
    const GraphContext& parent,
    const std::string& fnName,
    const std::string& argsJson = "{}"
  );

  static void logResponse(
    const GraphContext& ctx,
    const std::string& resultJson = "{}"
  );

  static void logError(
    const GraphContext& ctx,
    const std::string& error
  );

  static void flush(const std::string& outPath = "graph_trace.ndjson");

private:
  static std::string generateUuid();
  static std::vector<std::string> events_;
};
```

### GraphLogger.cpp (ключевой метод)

```cpp
GraphContext GraphLogger::logCall(
  const GraphContext& parent,
  const std::string& fnName,
  const std::string& argsJson
) {
  GraphContext ctx;
  ctx.nodeid = generateUuid();
  ctx.parentNodeid = parent.nodeid;
  ctx.depth = parent.depth + 1;

  // NDJSON — тот же формат что AuraLogEvent
  std::string event = R"({"type":"call","nodeid":")" + ctx.nodeid + R"(",)"
    + R"("parentNodeid":")" + ctx.parentNodeid + R"(",)"
    + R"("fnName":")" + fnName + R"(",)"
    + R"("depth":)" + std::to_string(ctx.depth) + ","
    + R"("args":)" + argsJson + ","
    + R"("tsMs":)" + std::to_string(nowMs()) + "}";

  events_.push_back(event);
  return ctx;
}
```

### relay.mjs: NDJSON → Neo4j Aura

```js
// simulation/relay.mjs — читает graph_trace.ndjson, пишет в Aura
import { AuraGraphReporter } from '../src/util/graphLogger/AuraGraphReporter.js';

const lines = fs.readFileSync('graph_trace.ndjson', 'utf8').split('\n');
const entries = lines.filter(Boolean).map(JSON.parse);
await AuraGraphReporter.writeEntries(entries);
```

### Что инструментировать первым

Три файла дают 80% coverage консенсусной логики:

| Файл | Ключевые функции | Уязвимость |
|---|---|---|
| `validator-session-round-attempt.cpp` | `make_vote()`, `make_precommit()` | Equivocation |
| `validator-session-state.cpp` | `apply_action()`, `try_approve_block()` | State divergence |
| `validator-session.cpp` | `process_message()`, `on_new_round()` | Message reordering, liveness |

### Итого

```
1. fork ton-blockchain/ton → свой репо
2. создать simulation/ с GraphLogger.cpp (~100 строк)
3. написать ConsensusHarness.cpp — мок actor runtime (без реального ADNL)
4. инструментировать 3 файла выше вызовами GraphLogger::logCall()
5. relay.mjs → AuraGraphReporter → Neo4j
6. Cypher-запросы из CYPHER_QUERIES.md — без изменений
```
