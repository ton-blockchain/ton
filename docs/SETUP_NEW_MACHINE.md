# Установка tonGraph на новой машине (Windows 10/11/Server x86-64)

Этот документ описывает полный путь от клонирования репозитория до запуска симуляции.  
Все исправления сборки уже включены в `master` — дополнительных патчей не требуется.

---

## 1. Требования

| Инструмент | Минимальная версия | Где взять |
|---|---|---|
| Visual Studio 2022 | Community или выше | https://visualstudio.microsoft.com/downloads/ |
| Node.js | 18 LTS или выше | https://nodejs.org/ |
| Git | любая | https://git-scm.com/ |

При установке Visual Studio выбери компонент **"Desktop development with C++"**.

прочитать https://github.com/ton-blockchain/ton#windows-10-11-server-x86-64

---

## 2. Добавить CMake в PATH

После установки VS добавь в системный `PATH` (Пуск → «Изменить переменные среды» → PATH):

```
C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
```

> Путь может отличаться — скорректируй под свою установку.

---

## 3. Клонировать репозиторий

```cmd
git clone https://github.com/a1oleg/tonGraph.git
cd tonGraph
```

---

## 4. Сборка TON-бинарников

Открой **от имени Администратора**:  
**Пуск → "x64 Native Tools Command Prompt for VS 2022" → ПКМ → Запустить от имени администратора**

```cmd
cd C:\GitHub\tonGraph
copy assembly\native\build-windows-2022.bat .
build-windows-2022.bat
```

Скрипт автоматически установит Chocolatey, ninja, nasm и соберёт все бинарники.  
Результат — в папке `artifacts\`.

> **Время сборки:** 15–40 минут.

---

## 5. Сборка модуля симуляции

В той же консоли (x64 Native Tools, от Администратора):

```cmd
cd C:\GitHub\tonGraph\build
cmake -DWITH_SIMULATION=ON .
cmake --build . --target ConsensusHarness
```

Результат: `build\simulation\ConsensusHarness.exe`

---

## 6. Настройка Neo4j Aura (для relay)

Создай файл `.env` в корне репозитория:

```env
NEO4J_URI=neo4j+s://<your-aura-id>.databases.neo4j.io
NEO4J_USER=neo4j
NEO4J_PASSWORD=<your-password>
GRAPH_LOGGING_ENABLED=1
GRAPH_LOG_FILE=simulation/trace.ndjson
```

> Без `.env` симуляция всё равно работает — просто relay в Neo4j не отправит данные.

---

## 7. Запуск симуляции

Из корня репозитория (обычный cmd или PowerShell):

```cmd
cd C:\GitHub\tonGraph
build\simulation\ConsensusHarness.exe --scenario equivocation
```

Доступные сценарии:

| Сценарий | Описание |
|---|---|
| `equivocation` | Byzantine валидатор голосует дважды за один slot |
| `message_withholding` | лидер не доставляет propose валидаторам |
| `byzantine_leader` | лидер шлёт разные кандидаты разным группам |

После запуска создаётся файл `simulation\trace.ndjson`.

---

## 8. Отправка трейса в Neo4j

```cmd
node simulation\relay.mjs
```

Для непрерывного режима (tail):

```cmd
set RELAY_WATCH=1
node simulation\relay.mjs
```

---

## 9. Проверка графа

Запросы к Neo4j — см. [CYPHER_QUERIES.md](CYPHER_QUERIES.md).  
MCP-подключение — см. [MCP_NEO4J_AURA.md](MCP_NEO4J_AURA.md).

---

## Структура после клонирования и сборки

```
tonGraph/
├── artifacts/                  ← TON-бинарники (validator-engine, lite-client, …)
├── build/
│   └── simulation/
│       └── ConsensusHarness.exe
├── simulation/
│   ├── GraphLogger.h / .cpp    ← C++ NDJSON-эмиттер
│   ├── ConsensusHarness.cpp    ← точка входа симуляции
│   ├── relay.mjs               ← NDJSON → Neo4j Aura
│   ├── trace.ndjson            ← создаётся при запуске
│   └── scenarios/
├── docs/
│   ├── SETUP_NEW_MACHINE.md    ← этот файл
│   ├── BUILD_AND_RUN.md
│   ├── SIMULATION.md
│   ├── GRAPH_LOGGING_PLAN.md
│   ├── CYPHER_QUERIES.md
│   └── MCP_NEO4J_AURA.md
└── .env                        ← создать вручную (не в git)
```

---

## Решение типичных проблем

| Ошибка | Причина | Решение |
|---|---|---|
| `Zlib build failed` | VS 2019 toolset вместо VS 2022 | убедись что используешь **x64 Native Tools for VS 2022** |
| `LNK2019 __std_find_not_ch_1` | смешение компиляторов VS 2019/2022 | удали `build\` и пересобери |
| `ninja: unknown target ConsensusHarness` | не передан флаг `-DWITH_SIMULATION=ON` | выполни `cmake -DWITH_SIMULATION=ON .` в папке `build\` |
| `ConsensusHarness: trace written` но файл пуст | нет прав на запись в `simulation\` | запусти из корня репозитория |
