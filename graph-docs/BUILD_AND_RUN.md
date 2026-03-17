# Сборка и запуск симуляции

> Официальные инструкции для Windows: [ton-blockchain/ton — Windows 10/11](https://github.com/ton-blockchain/ton#windows-10-11-server-x86-64).

```
build/
├── simulation            ← основной бинарник (ConsensusHarness)
└── …
```

> Первичная настройка (cmake configure, Windows fixes) — **[SETUP.md](SETUP.md)**.

---

## Пересборка

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

## Известные проблемы сборки Windows (MSVC)

### C1128 — `number of sections exceeds object file format limit`

Затронутые файлы: `tl/generate/auto/tl/ton_api.cpp`, `validator/manager.cpp` (и другие крупные TU).

```
error C1128: число секций превышает предел формата объектного файла:
             компилировать с /bigobj
```

**Причина:** автогенерированные TL-файлы и `manager.cpp` слишком велики для MSVC без `/bigobj`.
**Не наш код** — ошибка существовала до инструментации GraphLogger.
**Обходной путь:** добавить `/bigobj` в CMakeLists для затронутых таргетов (`tl_api`, `validator`).

> Пока `/bigobj` не добавлен — `cmake --build ... --target validator` завершается с ошибкой.
> Это **не мешает** проверить компиляцию отдельных файлов (см. ниже).

---

### Изолированная проверка одного .cpp (pool.cpp / consensus.cpp)

Работает через MSBuild с `SelectedFiles` — минует проблемные TU:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  "C:\GitHub\tonGraph\build\validator\validator.vcxproj" `
  /t:ClCompile `
  /p:Configuration=Debug `
  /p:Platform=x64 `
  "/p:SelectedFiles=consensus\simplex\pool.cpp" `
  /v:n
```

Если компиляция успешна — появится:
```
build\validator\validator.dir\Debug\pool.obj
```

Можно указать несколько файлов через `;`:
```
"/p:SelectedFiles=consensus\simplex\pool.cpp;../simulation/GraphLogger.cpp"
```

---

### Отладка сборки (общее)

```powershell
# Найти только error-строки
cmake --build build --target simulation 2>&1 | Select-String " error "

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
