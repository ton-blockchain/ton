/**
 * simulation/relay.mjs
 *
 * Reads simulation/trace.ndjson (one JSON object per line) and forwards
 * each entry to Neo4j Aura via AuraGraphReporter.writeEntries().
 *
 * Usage:
 *   node simulation/relay.mjs
 *
 * Env vars (required unless GRAPH_LOGGING_ENABLED=0):
 *   NEO4J_URI       bolt-URI, e.g. neo4j+s://xxxx.databases.neo4j.io
 *   NEO4J_USER      neo4j
 *   NEO4J_PASSWORD  <your-password>
 *
 * Optional:
 *   GRAPH_LOG_FILE  path to the NDJSON trace file (default: simulation/trace.ndjson)
 *   RELAY_WATCH     set to "1" to keep watching the file for new lines (tail mode)
 */

import { createReadStream, statSync, watchFile } from 'node:fs';
import { createInterface } from 'node:readline';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Resolve path relative to repo root (one level up from simulation/)
const __dirname = fileURLToPath(new URL('.', import.meta.url));
const repoRoot = resolve(__dirname, '..');

// ── Import AuraGraphReporter from teleGraph ──────────────────────────────
// Adjust this path if teleGraph is installed differently (npm workspace, etc.)
const { AuraGraphReporter } = await import(
  resolve(repoRoot, '../teleGraph/src/util/graphLogger/AuraGraphReporter.ts')
).catch(() =>
  // Fallback: try compiled JS output
  import(resolve(repoRoot, '../teleGraph/dist/util/graphLogger/AuraGraphReporter.js'))
);

// ── Config ────────────────────────────────────────────────────────────────
const NEO4J_URI      = process.env.NEO4J_URI      ?? '';
const NEO4J_USER     = process.env.NEO4J_USER     ?? 'neo4j';
const NEO4J_PASSWORD = process.env.NEO4J_PASSWORD ?? '';
const TRACE_FILE     = process.env.GRAPH_LOG_FILE ?? resolve(__dirname, 'trace.ndjson');
const WATCH_MODE     = process.env.RELAY_WATCH === '1';

if (!NEO4J_URI || !NEO4J_PASSWORD) {
  console.error('[relay] Missing NEO4J_URI or NEO4J_PASSWORD. Set them in .env or as env vars.');
  process.exit(1);
}

// ── Init reporter ─────────────────────────────────────────────────────────
AuraGraphReporter.init({
  uri:      NEO4J_URI,
  user:     NEO4J_USER,
  password: NEO4J_PASSWORD,
  database: 'neo4j',
  isEnabled: true,
  batchSize: 200,
  flushIntervalMs: 500,
});

// ── Convert a GraphEntry (C++ NDJSON line) to AuraLogEvent ───────────────
function toAuraLogEvent(entry) {
  const {
    nodeId,
    sessionId,
    type,
    tsMs,
    slot,
    candidateId,
    validatorIdx,
    voteType,
    outcome,
    parentNodeId,
    edgeType,
    edgeSlot,
    edgeWeight,
  } = entry;

  // Build edge props (carried on the relationship, not the node)
  const requestProps = { tsMs, slot: edgeSlot ?? slot };
  if (edgeWeight != null) requestProps.weight = edgeWeight;

  // Build node props
  const nodeProps = {
    fnName: type,      // AuraGraphReporter uses fnName as the primary display name
    sessionId,
    type,
    slot,
    tsMs,
    depth: 0,          // depth is not tracked here — queries use tsMs ordering
  };
  if (candidateId  != null) nodeProps.candidateId  = candidateId;
  if (validatorIdx != null) nodeProps.validatorIdx  = validatorIdx;
  if (voteType     != null) nodeProps.voteType      = voteType;
  if (outcome      != null) nodeProps.outcome       = outcome;

  return {
    nodeid:       nodeId,
    parentNodeid: parentNodeId,
    edgeType:     edgeType ?? 'call',
    requestProps,
    responseProps: {},
    nodeProps,
    labels: [type],
  };
}

// ── Stream processor ─────────────────────────────────────────────────────
async function processStream(stream) {
  const rl = createInterface({ input: stream, crlfDelay: Infinity });
  const batch = [];

  for await (const line of rl) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    let entry;
    try {
      entry = JSON.parse(trimmed);
    } catch (e) {
      console.warn('[relay] malformed line, skipping:', trimmed.slice(0, 80));
      continue;
    }
    batch.push(toAuraLogEvent(entry));
  }

  if (batch.length > 0) {
    AuraGraphReporter.writeEntries(batch);
    console.log(`[relay] queued ${batch.length} entries`);
  }
}

// ── Main ──────────────────────────────────────────────────────────────────
console.log(`[relay] reading ${TRACE_FILE}`);

try {
  statSync(TRACE_FILE);
} catch {
  console.error(`[relay] file not found: ${TRACE_FILE}`);
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
  // Give the reporter time to flush before exiting
  await new Promise((r) => setTimeout(r, 3000));
  console.log('[relay] done');
  process.exit(0);
}
