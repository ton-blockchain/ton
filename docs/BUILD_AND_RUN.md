# Сборка и запуск симуляции

```
build/
├── simulation            ← основной бинарник (ConsensusHarness)
└── …
```

---

## Первичная настройка (один раз)

```bash
# Из корня tonGraph/
cmake -B build -DWITH_SIMULATION=ON
cmake --build build --target simulation -j$(nproc)
```

**Флаги**

| Флаг CMake | Назначение |
|---|---|
| `-DWITH_SIMULATION=ON` | Включает цель `simulation/CMakeLists.txt` |
| `-DGRAPH_LOGGING_ENABLED=1` | Компилирует `GraphLogger` в режиме записи |
| `-DCMAKE_BUILD_TYPE=Debug` | Включает символы для lldb/gdb |

---

## Пересборка без полного cmake

```bash
# Только simulation-цель (быстро):
cmake --build build --target simulation -j4

# Только ConsensusHarness:
cmake --build build --target ConsensusHarness -j4
```

**Когда нужен полный cmake-reconfigure** (аналог рестарта dev-сервера):

```
изменения, требующие cmake -B build
├── simulation/CMakeLists.txt
├── CMakeLists.txt (корневой)
├── добавление новых .cpp файлов
└── изменение флагов компиляции
```

> Для правок внутри существующих `.cpp`/`.h` достаточно `cmake --build`.

---

## Запуск сценария

```bash
# Из корня tonGraph/
GRAPH_LOGGING_ENABLED=1 \
GRAPH_LOG_FILE=simulation/trace.ndjson \
./build/simulation/ConsensusHarness --scenario equivocation

# Доступные сценарии:
#   equivocation        — Byzantine валидатор голосует дважды
#   message_withholding — лидер удерживает propose
#   byzantine_leader    — лидер шлёт разные кандидаты
```

Бинарь записывает `simulation/trace.ndjson`.

---

## Запуск relay.mjs (NDJSON → Neo4j)

```bash
# Из корня tonGraph/
node simulation/relay.mjs
```

Читает `simulation/trace.ndjson` построчно и отправляет события через `AuraGraphReporter`.
Требует заполненного `.env` (см. [MCP_NEO4J_AURA.md](MCP_NEO4J_AURA.md)).

---

## Отладка сборки (Windows, PowerShell)

```powershell
# Найти ошибки компиляции
cmake --build build --target simulation 2>&1 | Select-String "error:"

# Проверить, что бинарь создан
Test-Path build/simulation/ConsensusHarness.exe
```

---

## Очистка трейс-файла перед новым прогоном

```bash
rm simulation/trace.ndjson
# или в PowerShell:
Remove-Item simulation/trace.ndjson -ErrorAction SilentlyContinue
```

> Не забудь также очистить граф в Neo4j: **[CYPHER_QUERIES.md#clean](CYPHER_QUERIES.md#clean)**.
