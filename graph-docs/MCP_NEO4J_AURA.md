# Подключение MCP-сервера Neo4j

`mcp-server-neo4j` — MCP-обёртка над Neo4j Aura. Стартер читает `.env`, маппит
`AURA_NEO4J_*` → `NEO4J_*` и запускает `uvx mcp-server-neo4j --uri ... --username ... --password ... --database ...`.

> **Важно**: `mcp-server-neo4j` читает credentials **только из CLI-флагов** — переменные окружения игнорирует.

---

## Схема `.env`

```env
AURA_NEO4J_URI=neo4j+s://<your-aura-id>.databases.neo4j.io
AURA_NEO4J_USER=neo4j
AURA_NEO4J_PASSWORD=<your-password>
AURA_NEO4J_DATABASE=neo4j
GRAPH_LOGGING_ENABLED=1
GRAPH_LOG_FILE=simulation/trace.ndjson
```

Стартер также поддерживает старые имена `NEO4J_URI`, `NEO4J_USER`, `NEO4J_PASSWORD`.

---

## VS Code

**Конфиг:** `.vscode/mcp.json` (переменная пути — `${workspaceFolder}`)

**Стартер:** `scripts/start-mcp-neo4j.ps1`

После открытия репозитория VS Code поднимает MCP-сервер автоматически — агент видит `neo4j-aura` как tool.

> **Статус:** проверено 2026-03-16. `uvx` v0.9.7, `.env` заполнен, handshake успешен.

---

## Visual Studio Community

**Конфиг:** `.vs/mcp.json` (переменная пути — `${workspaceRoot}`)

**Стартер:** `scripts/start-mcp-neo4j.ps1` (тот же, что у VS Code)

После открытия репозитория VS Community поднимает MCP-сервер — агент видит `neo4j-aura` как tool.

> **Статус:** проверено 2026-03-16. `uvx` v0.9.7, `.env` заполнен, handshake успешен.

---

## Claude Code CLI

IDE-конфиги (`.vscode/mcp.json`, `.vs/mcp.json`) Claude Code CLI **не читает**.
Создание `~/.claude/mcp.json` вручную тоже **не работает** — файл игнорируется.

Единственный рабочий способ — CLI-команда:

```bash
claude mcp add neo4j-aura -- uvx mcp-server-neo4j \
  --uri "neo4j+s://9d0c0b8b.databases.neo4j.io" \
  --username "neo4j" \
  --password "<password>" \
  --database "neo4j"
```

Команда добавляет запись в `~/.claude.json`. Проверка:

```bash
claude mcp list
# neo4j-aura: uvx mcp-server-neo4j ... - ✓ Connected
```

> **После регистрации нужна новая сессия.** `/mcp` в текущей сессии показывает диалог, но tools (`mcp__neo4j-aura__run_query` и др.) появляются только в новом чате.

**Использование в чате агента:**

- `Выполни в Neo4j запрос #last-session`
- `Проверь #equivocation после relay`
- `Очисти граф запросом #clean`

---

## Доступные tools (`mcp-server-neo4j` v1.26.0)

Одинаково для VS Code, VS Community и Claude Code CLI.

| Tool | Описание |
|------|----------|
| `run_query` | Выполнить Cypher-запрос |
| `get_database_info` | Версия БД, количество узлов/рёбер |
| `get_node_labels` | Все метки узлов |
| `get_relationship_types` | Все типы рёбер |
| `get_node_properties` | Свойства узлов по метке |

> **Важно**: tool называется `run_query`, не `query`. Обращение к `query` возвращает `"未知工具: query"`.

---

## Проверка подключения через pipe (без IDE)

> Протокол: сервер использует **NDJSON** (одна JSON-строка на сообщение), не LSP Content-Length фреймирование.

```bash
printf '%s\n%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
| uvx mcp-server-neo4j \
    --uri "neo4j+s://9d0c0b8b.databases.neo4j.io" \
    --username "neo4j" \
    --password "<password>" \
    --database "neo4j" 2>/dev/null
```

Ожидаемый первый ответ:
```json
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"experimental":{},"tools":{"listChanged":false}},"serverInfo":{"name":"mcp-neo4j","version":"1.26.0"}}}
```

---

## Известные проблемы

### `.env` с Windows line endings (CRLF) — оба IDE + relay.mjs

`relay.mjs` читает `.env` через regex `/^\s*([^#][^=]+)=(.*)$/`. При CRLF строки заканчиваются на `\r`, `$` не матчит → `[relay] Missing NEO4J credentials`.

Фикс — конвертировать один раз:
```bash
sed -i 's/\r//' .env
```

### PowerShell 5 — стартер-скрипт (оба IDE)

`Join-Path` с тремя аргументами — не работает в PS 5, только в PS 6+:
```powershell
# PS 5 — падает:
$envFile = Join-Path $PSScriptRoot ".." ".env"
# Правильно:
$envFile = Join-Path (Split-Path $PSScriptRoot -Parent) ".env"
```

Оператор `??` (null-coalescing) — не работает в PS 5:
```powershell
# PS 5 — падает:
$uri = $env:AURA_NEO4J_URI ?? $env:NEO4J_URI
# Правильно:
function Coalesce { foreach ($v in $args) { if ($v) { return $v } } }
$uri = Coalesce $env:AURA_NEO4J_URI $env:NEO4J_URI
```

`Process.ReadToEnd()` deadlock — `_test-mcp-init.ps1` зависает при попытке читать stdout/stderr после `Kill()` через `RedirectStandardOutput`. Правильный подход — pipe через stdin напрямую:
```powershell
$messages | uvx mcp-server-neo4j --uri $uri ...
```

Все три фикса уже внесены в `scripts/`.

### `~/.claude/mcp.json` не читается — только Claude Code CLI

Claude Code CLI не использует этот файл. Нужен `claude mcp add` (пишет в `~/.claude.json`).

### `/mcp` не перезагружает tools — только Claude Code CLI

Диалог открывается, но новые MCP tools доступны только в новой сессии.

---

*(Примечание: `relay.mjs` пишет узлы напрямую в Neo4j через драйвер `neo4j-driver`, не используя MCP.
 MCP нужен исключительно для удобства разработки — «общение» с графом через Cypher прямо из агента.)*
