/**
 * simulation/relay.mjs
 *
 * Reads simulation/trace.ndjson (one JSON object per line) and writes
 * each entry into Neo4j Aura directly via neo4j-driver (no TypeScript build needed).
 *
 * Usage:
 *   node simulation/relay.mjs
 *
 * Env vars (required):
 *   NEO4J_URI       bolt-URI, e.g. neo4j+s://xxxx.databases.neo4j.io
 *   NEO4J_USER      neo4j
 *   NEO4J_PASSWORD  <your-password>
 *
 * Optional:
 *   GRAPH_LOG_FILE  path to the NDJSON trace file (default: simulation/trace.ndjson)
 *   RELAY_WATCH     set to "1" to keep watching the file for new lines (tail mode)
 *   NEO4J_DATABASE  target database name (default: neo4j)
 */

import { createReadStream, statSync, watchFile } from 'node:fs';
import { createInterface } from 'node:readline';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

// ── Resolve paths ─────────────────────────────────────────────────────────
const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot  = resolve(__dirname, '..');

// neo4j-driver lives in teleGraph's node_modules (sibling repo)
const require = createRequire(import.meta.url);
const neo4j = require(resolve(repoRoot, '../teleGraph/node_modules/neo4j-driver'));

// ── Load .env from repo root (if present) ────────────────────────────────
try {
  const envPath = resolve(repoRoot, '.env');
  const envText = require('node:fs').readFileSync(envPath, 'utf8');
  for (const line of envText.split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
  console.log('[relay] loaded .env');
} catch {
  // .env is optional
}

// ── Config ────────────────────────────────────────────────────────────────
const NEO4J_URI      = process.env.AURA_NEO4J_URI      ?? process.env.NEO4J_URI      ?? '';
const NEO4J_USER     = process.env.AURA_NEO4J_USER     ?? process.env.NEO4J_USER     ?? 'neo4j';
const NEO4J_PASSWORD = process.env.AURA_NEO4J_PASSWORD ?? process.env.NEO4J_PASSWORD ?? '';
const NEO4J_DB       = process.env.AURA_NEO4J_DATABASE ?? process.env.NEO4J_DATABASE ?? 'neo4j';
const TRACE_FILE     = process.env.GRAPH_LOG_FILE ?? resolve(__dirname, 'trace.ndjson');
const WATCH_MODE     = process.env.RELAY_WATCH === '1';

if (!NEO4J_URI || !NEO4J_PASSWORD) {
  console.error('[relay] ERROR: NEO4J_URI and NEO4J_PASSWORD must be set in .env or environment.');
  console.error('[relay] Supported variable names: AURA_NEO4J_URI / NEO4J_URI, AURA_NEO4J_PASSWORD / NEO4J_PASSWORD');
  process.exit(1);
}

// ── Neo4j driver ──────────────────────────────────────────────────────────
const driver = neo4j.driver(
  NEO4J_URI,
  neo4j.auth.basic(NEO4J_USER, NEO4J_PASSWORD),
  { disableLosslessIntegers: true },
);

try {
  await driver.verifyConnectivity();
  console.log(`[relay] connected to ${NEO4J_URI}`);
} catch (e) {
  console.error('[relay] cannot connect to Neo4j:', e.message);
  await driver.close();
  process.exit(1);
}

// ── Cypher: upsert one GraphEntry as a node (+ optional edge to parent) ──
const UPSERT_CYPHER = `
MERGE (n:GraphNode {nodeId: $nodeId})
ON CREATE SET
  n.sessionId    = $sessionId,
  n.type         = $type,
  n.tsMs         = $tsMs,
  n.slot         = $slot,
  n.candidateId  = $candidateId,
  n.validatorIdx = $validatorIdx,
  n.voteType     = $voteType,
  n.outcome      = $outcome
WITH n
CALL {
  WITH n
  MATCH (p:GraphNode {nodeId: $parentNodeId})
  WHERE $parentNodeId IS NOT NULL
  MERGE (p)-[e:EDGE {edgeType: $edgeType}]->(n)
  ON CREATE SET e.slot = $edgeSlot, e.weight = $edgeWeight
  RETURN count(e) AS edgeCount
}
RETURN n.nodeId AS id
`;

async function writeEntries(entries) {
  if (entries.length === 0) return;
  const session = driver.session({ database: NEO4J_DB });
  try {
    const tx = session.beginTransaction();
    for (const e of entries) {
      tx.run(UPSERT_CYPHER, {
        nodeId:       e.nodeId       ?? null,
        sessionId:    e.sessionId    ?? null,
        type:         e.type         ?? null,
        tsMs:         e.tsMs         ?? 0,
        slot:         e.slot         ?? 0,
        candidateId:  e.candidateId  ?? null,
        validatorIdx: e.validatorIdx ?? null,
        voteType:     e.voteType     ?? null,
        outcome:      e.outcome      ?? null,
        parentNodeId: e.parentNodeId ?? null,
        edgeType:     e.edgeType     ?? null,
        edgeSlot:     e.edgeSlot     ?? e.slot ?? 0,
        edgeWeight:   e.edgeWeight   ?? null,
      });
    }
    await tx.commit();
    console.log(`[relay] wrote ${entries.length} node(s) to Neo4j`);
  } catch (e) {
    console.error('[relay] write failed:', e.message);
  } finally {
    await session.close();
  }
}

// ── Stream processor ──────────────────────────────────────────────────────
async function processStream(stream) {
  const rl = createInterface({ input: stream, crlfDelay: Infinity });
  const batch = [];

  for await (const line of rl) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    try {
      batch.push(JSON.parse(trimmed));
    } catch {
      console.warn('[relay] malformed line, skipping:', trimmed.slice(0, 80));
    }
  }

  await writeEntries(batch);
}

// ── Main ──────────────────────────────────────────────────────────────────
console.log(`[relay] reading ${TRACE_FILE}`);

try {
  statSync(TRACE_FILE);
} catch {
  console.error(`[relay] file not found: ${TRACE_FILE}`);
  await driver.close();
  process.exit(1);
}

await processStream(createReadStream(TRACE_FILE, { encoding: 'utf8' }));

if (WATCH_MODE) {
  console.log('[relay] watch mode — following new lines (Ctrl+C to stop)');
  let offset = statSync(TRACE_FILE).size;

  watchFile(TRACE_FILE, { interval: 300 }, async () => {
    const newSize = statSync(TRACE_FILE).size;
    if (newSize <= offset) return;
    const stream = createReadStream(TRACE_FILE, { encoding: 'utf8', start: offset });
    offset = newSize;
    await processStream(stream);
  });
} else {
  await driver.close();
  console.log('[relay] done');
  process.exit(0);
}
