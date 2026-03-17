# Алгоритм проверки MCP

1. **Установка и проверка наличия (`uvx mcp-server-neo4j`)**
   Убедиться, что Python-утилита `uv` установлена. Пакет `mcp-server-neo4j` должен успешно скачиваться и запускаться через `uvx`.

2. **Подключение как MCP tool**

   В репозитории лежат конфиги и стартер `scripts/start-mcp-neo4j.ps1`.

   | IDE | Конфиг | Переменная пути |
   |-----|--------|-----------------|
   | VS Code | `.vscode/mcp.json` | `${workspaceFolder}` |
   | Visual Studio 2022/2026 | `.vs/mcp.json` | `${workspaceRoot}` |

   Стартер:
   - читает `.env` из корня репозитория,
   - маппит `AURA_NEO4J_*` → `NEO4J_*`,
   - запускает `uvx mcp-server-neo4j --uri ... --username ... --password ... --database ...`.

   > **Важно**: `mcp-server-neo4j` читает credentials **только из CLI-флагов** — переменные окружения игнорирует. Стартер явно передаёт их через `--uri/--username/--password/--database`.

   После открытия репозитория в IDE агент должен увидеть MCP-сервер `neo4j-aura` как tool.

   > **Статус**: проверено 2026-03-16. `uvx` v0.9.7, `.env` заполнен, оба конфига валидны, handshake успешен.

3. **Регистрация в Claude Code (CLI)**

   IDE-конфиги (`.vscode/mcp.json`, `.vs/mcp.json`) **не читаются** Claude Code CLI.
   Создание `~/.claude/mcp.json` вручную тоже **не работает** — этот файл игнорируется.

   Единственный рабочий способ — CLI-команда:

   ```bash
   claude mcp add neo4j-aura -- uvx mcp-server-neo4j \
     --uri "neo4j+s://9d0c0b8b.databases.neo4j.io" \
     --username "neo4j" \
     --password "<password>" \
     --database "neo4j"
   ```

   Команда добавляет запись в `~/.claude.json` (project-scoped local config).
   Проверка что сервер подключился:

   ```bash
   claude mcp list
   # neo4j-aura: uvx mcp-server-neo4j ... - ✓ Connected
   ```

   > **После регистрации нужна новая сессия.** `/mcp` в текущей сессии показывает диалог, но tools (`mcp__neo4j-aura__run_query` и др.) появляются только в новом чате.

4. **Доступные tools (`mcp-server-neo4j` v1.26.0)**

   | Tool | Описание |
   |------|----------|
   | `run_query` | Выполнить Cypher-запрос |
   | `get_database_info` | Версия БД, количество узлов/рёбер |
   | `get_node_labels` | Все метки узлов |
   | `get_relationship_types` | Все типы рёбер |
   | `get_node_properties` | Свойства узлов по метке |

   > **Важно**: tool называется `run_query`, не `query`. Обращение к `query` возвращает `"未知工具: query"`.

5. **Проверка подключения через pipe (без IDE/Claude Code)**

   > **Протокол**: сервер использует **NDJSON** (одна JSON-строка на сообщение), не LSP Content-Length фреймирование.

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

6. **Известные проблемы**

   **`.env` с Windows line endings (CRLF)** — `relay.mjs` читает `.env` через regex `/^\s*([^#][^=]+)=(.*)$/`. При CRLF строки заканчиваются на `\r`, `$` не матчит, все переменные игнорируются → `[relay] Missing NEO4J credentials`.

   Фикс — конвертировать `.env` в Unix endings один раз:
   ```bash
   sed -i 's/\r//' .env
   ```

   **`~/.claude/mcp.json` не читается** — Claude Code CLI не использует этот файл. Нужен `claude mcp add` (пишет в `~/.claude.json`).

   **`/mcp` не перезагружает tools в текущей сессии** — диалог открывается, но новые MCP tools доступны только в новой сессии.

   **Известные проблемы скриптов (Windows PowerShell 5)**

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
   Оба фикса уже внесены в `scripts/`.

7. **Схема `.env`**

   ```env
   AURA_NEO4J_URI=neo4j+s://<your-aura-id>.databases.neo4j.io
   AURA_NEO4J_USER=neo4j
   AURA_NEO4J_PASSWORD=<your-password>
   AURA_NEO4J_DATABASE=neo4j
   GRAPH_LOGGING_ENABLED=1
   GRAPH_LOG_FILE=simulation/trace.ndjson
   ```

   Стартер также поддерживает старые имена `NEO4J_URI`, `NEO4J_USER`, `NEO4J_PASSWORD`.

8. **Как использовать в чате агента**

   После регистрации через `claude mcp add` и старта новой сессии:

   - `Выполни в Neo4j запрос #last-session`
   - `Проверь #equivocation после relay`
   - `Очисти граф запросом #clean`

   Это и есть автоматизированная проверка графа через MCP, без ручного терминала.

---

*(Примечание: `relay.mjs` пишет узлы напрямую в Neo4j через драйвер `neo4j-driver`, не используя MCP.
 MCP нужен исключительно для удобства разработки — «общение» с графом через Cypher прямо из агента.)*
