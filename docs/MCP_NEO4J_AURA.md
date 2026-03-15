# Алгоритм проверки MCP

1. **Установка и проверка наличия (`uvx mcp-server-neo4j`)**
   Убедиться, что Python-утилита `uv` установлена. Пакет `mcp-server-neo4j` должен успешно скачиваться и запускаться через `uvx`.

2. **Проверка подключения с кредами из `.env`**
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

3. **Схема `.env`**

   ```env
   NEO4J_URI=neo4j+s://<your-aura-id>.databases.neo4j.io
   NEO4J_USER=neo4j
   NEO4J_PASSWORD=<your-password>
   GRAPH_LOGGING_ENABLED=1
   GRAPH_LOG_FILE=simulation/trace.ndjson
   ```

   В корне репозитория лежит `mcp.neo4j.example.json` как памятка для заполнения.

---

*(Примечание: `relay.mjs` пишет узлы напрямую в Neo4j через `AuraGraphReporter`, не используя MCP.  
 MCP нужен исключительно для удобства разработки — «общение» с графом через Cypher прямо из агента.)*
