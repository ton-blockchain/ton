# C++ GraphLogger — граф-логирование из консенсус-потока

`GraphLogger` работает в consensus-потоке (tdactor). `relay.mjs` читает файл и пишет в Neo4j Aura.

---

## Поток данных

```
consensus-поток (C++)
  │
  ├─ GraphLogger::emit(type, props)
  │     └─ сериализует в NDJSON-строку
  │     └─ append в файл  simulation/trace.ndjson
  │
relay.mjs (Node.js, отдельный процесс)
  │
  ├─ tail -f simulation/trace.ndjson
  │     └─ AuraGraphReporter.writeEntries(entries)
  │           └─ Neo4j Aura (bolt://)
```

Consensus-поток **не ждёт** записи в файл. `GraphLogger` использует неблокирующий буферизованный `ofstream`.

---

## Архитектура `GraphLogger`

**`simulation/GraphLogger.h`**

```cpp
namespace ton::simulation {

struct GraphEntry {
  std::string nodeId;      // UUID
  std::string sessionId;   // bus.session_id (hex)
  std::string type;        // "Candidate" | "Vote" | "Cert" | "Block" | "SkipEvent" | "Validator"
  int64_t tsMs;            // td::Timestamp::now().at() * 1000
  uint32_t slot;
  std::optional<std::string> candidateId;
  std::optional<int64_t> validatorIdx;
  std::optional<std::string> voteType;     // "notarize" | "finalize" | "skip"
  std::optional<std::string> outcome;      // "reject" | "misbehavior"

  // Ребро к родительскому узлу
  std::optional<std::string> parentNodeId;
  std::optional<std::string> edgeType;     // из #edge-types в CYPHER_QUERIES.md
  std::map<std::string, std::string> edgeProps;  // slot, tsMs, weight, …
};

class GraphLogger {
 public:
  static GraphLogger& instance();

  void init(std::string session_id, std::string output_path = "simulation/trace.ndjson");
  void emit(const GraphEntry& entry);
  bool is_enabled() const;

 private:
  std::ofstream file_;
  std::string session_id_;
  bool enabled_ = true;
};

}  // namespace ton::simulation
```

---

## Пример инструментации (`simplex/consensus.cpp`)

```cpp
// В handle(BusHandle, std::shared_ptr<const CandidateReceived> event):
if (GraphLogger::instance().is_enabled()) {
  GraphEntry e;
  e.type        = "Candidate";
  e.sessionId   = owning_bus()->session_id_hex();
  e.slot        = event->candidate->id.slot;
  e.candidateId = event->candidate->id.to_hex();
  e.tsMs        = static_cast<int64_t>(td::Timestamp::now().at() * 1000);
  e.nodeId      = make_uuid();
  // Ребро receive: Validator → Candidate
  e.parentNodeId = local_validator_node_id_;
  e.edgeType     = "receive";
  e.edgeProps["slot"]  = std::to_string(event->candidate->id.slot);
  e.edgeProps["tsMs"]  = std::to_string(e.tsMs);
  GraphLogger::instance().emit(e);
}
```

---

## NDJSON-формат (одна строка = одно событие)

```json
{"nodeId":"uuid-…","sessionId":"deadbeef…","type":"Candidate","tsMs":1741700000000,"slot":42,"candidateId":"abc123…","parentNodeId":"uuid-validator-…","edgeType":"receive","edgeProps":{"slot":"42","tsMs":"1741700000000"}}
```

Каждая JSON-строка — самодостаточна. `relay.mjs` конвертирует её в `AuraLogEvent` и вызывает
`AuraGraphReporter.writeEntries([entry])`.

---

## relay.mjs

**`simulation/relay.mjs`**

```js
import { createInterface } from 'node:readline';
import { createReadStream } from 'node:fs';
import { AuraGraphReporter } from './AuraGraphReporter.js';

const rl = createInterface({ input: createReadStream('simulation/trace.ndjson') });

for await (const line of rl) {
  if (!line.trim()) continue;
  const entry = JSON.parse(line);
  await AuraGraphReporter.writeEntries([entry]);
}
```

### Режим «живого слежения» (tail -f)

```js
import { watchFile } from 'node:fs';
// ... буферизованное чтение с readline при каждом изменении файла
```

Детали реализации: `simulation/relay.mjs` в репозитории.

---

## API GraphLogger — методы

| Метод | Описание |
|---|---|
| `init(session_id, path)` | Открывает файл, записывает session-заголовок |
| `emit(entry)` | Сериализует `GraphEntry` → NDJSON, flush |
| `is_enabled()` | Проверяет `GRAPH_LOGGING_ENABLED` env-переменную |
| `make_uuid()` (free fn) | Генерирует UUID v4 без зависимостей (random + format) |

---

## Переменные окружения

| Переменная | По умолчанию | Описание |
|---|---|---|
| `GRAPH_LOGGING_ENABLED` | `1` | `0` — отключает emit |
| `GRAPH_LOG_FILE` | `simulation/trace.ndjson` | Путь к выходному файлу |
| `NEO4J_URI` | — | bolt-URI для relay.mjs (из `.env`) |
| `NEO4J_USER` | — | пользователь Neo4j |
| `NEO4J_PASSWORD` | — | пароль Neo4j |
