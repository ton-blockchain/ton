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

3. **Проверка подключения с кредами из `.env`**

   Запустить `scripts/_test-mcp-init.ps1` — он читает `.env`, стартует сервер с явными флагами и отправляет `initialize` + `notifications/initialized`.

   > **Протокол**: сервер использует **NDJSON** (одна JSON-строка на сообщение), не LSP Content-Length фреймирование.

   > **Альтернатива (bash/Git Bash)** — прямой pipe без скрипта:
   > ```bash
   > echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}
   > {"jsonrpc":"2.0","method":"notifications/initialized","params":{}}' \
   >   | uvx mcp-server-neo4j \
   >       --uri "neo4j+s://9d0c0b8b.databases.neo4j.io" \
   >       --username "neo4j" \
   >       --password "<password>" \
   >       --database "neo4j"
   > ```

   Ожидаемый ответ в stdout:
   ```json
   {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"experimental":{},"tools":{"listChanged":false}},"serverInfo":{"name":"mcp-neo4j","version":"1.26.0"}}}
   ```

   Ожидаемые строки в stderr:
   ```
   INFO - mcp-neo4j - 正在启动Neo4j MCP服务器
   INFO - mcp-neo4j - Neo4j MCP服务器启动成功
   ```

4. **Известные проблемы скриптов (Windows PowerShell 5)**

   **`Join-Path` с тремя аргументами** — не работает в PS 5, только в PS 6+:
   ```powershell
   # PS 5 — падает:
   $envFile = Join-Path $PSScriptRoot ".." ".env"
   # Правильно:
   $envFile = Join-Path (Split-Path $PSScriptRoot -Parent) ".env"
   ```

   **Оператор `??` (null-coalescing)** — не работает в PS 5:
   ```powershell
   # PS 5 — падает:
   $uri = $env:AURA_NEO4J_URI ?? $env:NEO4J_URI
   # Правильно:
   function Coalesce { foreach ($v in $args) { if ($v) { return $v } } }
   $uri = Coalesce $env:AURA_NEO4J_URI $env:NEO4J_URI
   ```

   **`Process.ReadToEnd()` deadlock** — `_test-mcp-init.ps1` зависает при попытке читать stdout/stderr после `Kill()` через `RedirectStandardOutput`. Правильный подход — pipe через stdin напрямую:
   ```powershell
   $messages | uvx mcp-server-neo4j --uri $uri ...
   ```
   Оба фикса уже внесены в `scripts/`.

5. **Схема `.env`**

   ```env
   AURA_NEO4J_URI=neo4j+s://<your-aura-id>.databases.neo4j.io
   AURA_NEO4J_USER=neo4j
   AURA_NEO4J_PASSWORD=<your-password>
   AURA_NEO4J_DATABASE=neo4j
   GRAPH_LOGGING_ENABLED=1
   GRAPH_LOG_FILE=simulation/trace.ndjson
   ```

   Стартер также поддерживает старые имена `NEO4J_URI`, `NEO4J_USER`, `NEO4J_PASSWORD`.

5. **Как использовать в чате агента**

   После того как IDE подняла MCP tool, можно писать в чат:

   - `Выполни в Neo4j запрос #last-session`
   - `Проверь #equivocation после relay`
   - `Очисти граф запросом #clean`

   Это и есть автоматизированная проверка графа через MCP, без ручного терминала.

---

*(Примечание: `relay.mjs` пишет узлы напрямую в Neo4j через `AuraGraphReporter`, не используя MCP.
 MCP нужен исключительно для удобства разработки — «общение» с графом через Cypher прямо из агента.)*
