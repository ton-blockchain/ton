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
   - запускает `uvx mcp-server-neo4j`.

   После открытия репозитория в IDE агент должен увидеть MCP-сервер `neo4j-aura` как tool.

   > **Статус**: `uvx` v0.9.7 доступен, `.env` заполнен, оба конфига валидны.
   >
   > **Важно**: `mcp-server-neo4j` читает только CLI-флаги (`--uri`, `--username`, `--password`, `--database`), переменные окружения игнорирует. Стартер `start-mcp-neo4j.ps1` маппит их и передаёт явно.

3. **Проверка подключения с кредами из `.env`**
   Запустить сервер, передав переменные из `.env` (`NEO4J_URI`, `NEO4J_USER`, `NEO4J_PASSWORD`),
   после чего отправить тестовый `initialize` запрос по протоколу MCP.

   Ожидаемый ответ:
   ```json
   {
     "jsonrpc": "2.0",
     "id": 1,
     "result": {
       "protocolVersion": "2024-11-05",
       "capabilities": {
         "experimental": {},
         "tools": { "listChanged": false }
       },
       "serverInfo": {
         "name": "mcp-neo4j",
         "version": "1.26.0"
       }
     }
   }
   ```
   В логах stderr также может быть зафиксировано успешное подключение.

4. **Схема `.env`**

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
