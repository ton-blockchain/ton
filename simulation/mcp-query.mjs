/**
 * simulation/mcp-query.mjs
 *
 * One-shot MCP query tool: sends a Cypher query to mcp-server-neo4j
 * and prints the result, then exits.
 *
 * Usage:
 *   node simulation/mcp-query.mjs "MATCH (n:GraphNode) RETURN n LIMIT 5"
 *   node simulation/mcp-query.mjs last-session
 *   node simulation/mcp-query.mjs equivocation
 *
 * Reads .env from repo root for NEO4J_* / AURA_NEO4J_* credentials.
 */

import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const req = createRequire(import.meta.url);
const fs  = req('node:fs');

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot  = resolve(__dirname, '..');

// ── Load .env ─────────────────────────────────────────────────────────────
try {
  for (const line of fs.readFileSync(resolve(repoRoot, '.env'), 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
} catch { /* .env optional */ }

const NEO4J_URI      = process.env.AURA_NEO4J_URI      ?? process.env.NEO4J_URI      ?? '';
const NEO4J_USER     = process.env.AURA_NEO4J_USER     ?? process.env.NEO4J_USER     ?? 'neo4j';
const NEO4J_PASSWORD = process.env.AURA_NEO4J_PASSWORD ?? process.env.NEO4J_PASSWORD ?? '';

if (!NEO4J_URI || !NEO4J_PASSWORD) {
  console.error('ERROR: set NEO4J_URI / NEO4J_PASSWORD in .env');
  process.exit(1);
}

// ── Named queries ─────────────────────────────────────────────────────────
const NAMED = {
  'last-session': `
    MATCH (n:GraphNode)
    RETURN n.nodeId AS id, n.type AS type, n.sessionId AS session,
           n.slot AS slot, n.outcome AS outcome
    ORDER BY n.tsMs DESC LIMIT 20`,

  'equivocation': `
    MATCH (v:GraphNode)-[r1:EDGE {edgeType:'notarize'}]->(c1:GraphNode),
          (v)-[r2:EDGE {edgeType:'notarize'}]->(c2:GraphNode)
    WHERE r1.slot = r2.slot AND c1.candidateId <> c2.candidateId
    RETURN v.validatorIdx AS validator, r1.slot AS slot,
           c1.candidateId AS candidate1, c2.candidateId AS candidate2`,

  'frontier': `
    MATCH (n:GraphNode)
    WHERE NOT (n)-[:EDGE]->()
    RETURN n.type AS type, n.slot AS slot, n.nodeId AS id
    ORDER BY n.slot`,

  'clean': `MATCH (n:GraphNode) DETACH DELETE n`,

  'count': `MATCH (n:GraphNode) RETURN count(n) AS total`,
};

const arg = process.argv[2];
if (!arg) {
  console.log('Available named queries:', Object.keys(NAMED).join(', '));
  console.log('Or pass a raw Cypher string.');
  process.exit(0);
}

const cypher = NAMED[arg] ?? arg;

// ── Spawn mcp-server-neo4j ────────────────────────────────────────────────
const env = { ...process.env, NEO4J_URI, NEO4J_USER, NEO4J_PASSWORD };

const proc = spawn('uvx', ['mcp-server-neo4j'], {
  env,
  stdio: ['pipe', 'pipe', 'pipe'],
});

let stdout = '';
proc.stdout.on('data', d => { stdout += d.toString(); });
proc.stderr.on('data', d => {
  const s = d.toString().trim();
  if (s && !s.includes('INFO')) process.error('[mcp stderr]', s);
});

// Send initialize then query, then close stdin
const init = JSON.stringify({
  jsonrpc: '2.0', id: 1, method: 'initialize',
  params: { protocolVersion: '2024-11-05', capabilities: {},
            clientInfo: { name: 'mcp-query', version: '1.0' } },
}) + '\n';

const query = JSON.stringify({
  jsonrpc: '2.0', id: 2, method: 'tools/call',
  params: { name: 'query', arguments: { query: cypher.trim() } },
}) + '\n';

proc.stdin.write(init);
proc.stdin.write(query);
proc.stdin.end(); // ← closes stdin → server will exit

// Wait for response with timeout
const timeout = setTimeout(() => {
  proc.kill();
  console.error('Timeout waiting for MCP response');
  process.exit(1);
}, 10000);

proc.on('close', () => {
  clearTimeout(timeout);
  // Parse all JSONRPC responses from stdout
  for (const line of stdout.split('\n')) {
    const t = line.trim();
    if (!t) continue;
    try {
      const msg = JSON.parse(t);
      if (msg.id === 2) {
        // This is our query result
        const content = msg.result?.content;
        if (Array.isArray(content)) {
          for (const c of content) {
            if (c.type === 'text') {
              try {
                const rows = JSON.parse(c.text);
                console.log(`\nQuery: ${arg}`);
                console.log(`Rows:  ${Array.isArray(rows) ? rows.length : '?'}\n`);
                if (Array.isArray(rows) && rows.length > 0) {
                  console.table(rows);
                } else {
                  console.log('(no results)');
                }
              } catch {
                console.log(c.text);
              }
            }
          }
        } else if (msg.error) {
          console.error('Query error:', msg.error.message);
        }
      }
    } catch { /* skip non-JSON lines */ }
  }
});
