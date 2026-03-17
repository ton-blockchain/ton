// relay.mjs — reads simulation/trace.ndjson line-by-line and pushes events
// into Neo4j Aura using the schema defined in GRAPH_LOGGING_PLAN.md.
//
// Schema written to Neo4j:
//   (Validator)-[:propose {slot,tsMs}]->(Candidate)
//   (Candidate)<-[:notarize {slot,tsMs}]-(Validator)
//   (Candidate)-[:cert {slot,weight,tsMs}]->(Cert)
//   (Cert)<-[:finalize {slot,tsMs}]-(Validator)
//   (Cert)-[:accepted {slot,tsMs}]->(Block)
//   (Validator)-[:skip {slot,tsMs}]->(SkipEvent)
//
// Usage: node simulation/relay.mjs [path/to/trace.ndjson]
//
// Reads credentials from env (or ../.env):
//   AURA_NEO4J_URI, AURA_NEO4J_USER, AURA_NEO4J_PASSWORD, AURA_NEO4J_DATABASE

import fs from 'fs';
import path from 'path';
import readline from 'readline';
import { fileURLToPath } from 'url';
import neo4j from 'neo4j-driver';

// ── Env loading ─────────────────────────────────────────────────────────────

const __dir = path.dirname(fileURLToPath(import.meta.url));

function loadEnv(envPath) {
  if (!fs.existsSync(envPath)) return;
  const lines = fs.readFileSync(envPath, 'utf8').split('\n');
  for (const line of lines) {
    const m = line.match(/^\s*([^#][^=]+)=(.*)$/);
    if (m) process.env[m[1].trim()] = m[2].trim();
  }
}

loadEnv(path.resolve(__dir, '..', '.env'));

const URI      = process.env.AURA_NEO4J_URI      || process.env.NEO4J_URI;
const USER     = process.env.AURA_NEO4J_USER     || process.env.NEO4J_USERNAME || process.env.NEO4J_USER;
const PASSWORD = process.env.AURA_NEO4J_PASSWORD || process.env.NEO4J_PASSWORD;
const DATABASE = process.env.AURA_NEO4J_DATABASE || process.env.NEO4J_DATABASE || 'neo4j';

if (!URI || !PASSWORD) {
  console.error('[relay] Missing NEO4J credentials. Fill .env in repo root.');
  process.exit(1);
}

// ── Neo4j driver ────────────────────────────────────────────────────────────

const driver = neo4j.driver(URI, neo4j.auth.basic(USER, PASSWORD));

async function runQuery(session, query, params = {}) {
  try {
    await session.run(query, params);
  } catch (e) {
    console.error('[relay] Cypher error:', e.message, '\nQuery:', query);
  }
}

// ── Node ID helpers (stable, deterministic) ─────────────────────────────────

const validatorNodeId = (sessionId, idx) => `validator:${sessionId}:${idx}`;
const candidateNodeId = (sessionId, candidateId) => `candidate:${sessionId}:${candidateId}`;
const certNodeId      = (sessionId, candidateId) => `cert:${sessionId}:${candidateId}`;
const blockNodeId     = (sessionId, candidateId) => `block:${sessionId}:${candidateId}`;
const skipNodeId      = (sessionId, slot)        => `skip:${sessionId}:${slot}`;

// ── Ensure Validator nodes exist (created lazily) ───────────────────────────

const createdValidators = new Set();

async function ensureValidator(session, sessionId, idx) {
  const nid = validatorNodeId(sessionId, idx);
  if (createdValidators.has(nid)) return nid;
  await runQuery(session,
    `MERGE (v:Validator {nodeId: $nid})
     SET v.validatorIdx = $idx, v.sessionId = $sessionId`,
    { nid, idx: neo4j.int(idx), sessionId });
  createdValidators.add(nid);
  return nid;
}

// ── Event handlers ──────────────────────────────────────────────────────────

async function handlePropose(session, ev) {
  const { sessionId, slot, leaderIdx, candidateId, tsMs } = ev;
  const vNid = await ensureValidator(session, sessionId, leaderIdx);
  const cNid = candidateNodeId(sessionId, candidateId);

  await runQuery(session,
    `MERGE (c:Candidate {nodeId: $cNid})
     SET c.slot = $slot, c.candidateId = $candidateId, c.sessionId = $sessionId`,
    { cNid, slot: neo4j.int(slot), candidateId, sessionId });

  await runQuery(session,
    `MATCH (v:Validator {nodeId: $vNid}), (c:Candidate {nodeId: $cNid})
     MERGE (v)-[r:propose {slot: $slot}]->(c)
     SET r.tsMs = $tsMs`,
    { vNid, cNid, slot: neo4j.int(slot), tsMs: neo4j.int(tsMs) });
}

async function handleNotarizeVote(session, ev) {
  const { sessionId, slot, validatorIdx, candidateId, tsMs } = ev;
  const vNid = await ensureValidator(session, sessionId, validatorIdx);
  const cNid = candidateNodeId(sessionId, candidateId);

  // Candidate may already exist (from Propose); create if missing (DoubleVote scenario)
  await runQuery(session,
    `MERGE (c:Candidate {nodeId: $cNid})
     ON CREATE SET c.slot = $slot, c.candidateId = $candidateId, c.sessionId = $sessionId`,
    { cNid, slot: neo4j.int(slot), candidateId, sessionId });

  await runQuery(session,
    `MATCH (v:Validator {nodeId: $vNid}), (c:Candidate {nodeId: $cNid})
     MERGE (v)-[r:notarize {slot: $slot, validatorIdx: $validatorIdx}]->(c)
     SET r.tsMs = $tsMs`,
    { vNid, cNid, slot: neo4j.int(slot), validatorIdx: neo4j.int(validatorIdx), tsMs: neo4j.int(tsMs) });
}

async function handleNotarizeCert(session, ev) {
  const { sessionId, slot, candidateId, weight, tsMs } = ev;
  const cNid = candidateNodeId(sessionId, candidateId);
  const certNid = certNodeId(sessionId, candidateId);

  await runQuery(session,
    `MERGE (cert:Cert {nodeId: $certNid})
     SET cert.slot = $slot, cert.candidateId = $candidateId,
         cert.weight = $weight, cert.sessionId = $sessionId`,
    { certNid, slot: neo4j.int(slot), candidateId, weight: neo4j.int(weight), sessionId });

  await runQuery(session,
    `MATCH (c:Candidate {nodeId: $cNid}), (cert:Cert {nodeId: $certNid})
     MERGE (c)-[r:cert {slot: $slot}]->(cert)
     SET r.weight = $weight, r.tsMs = $tsMs`,
    { cNid, certNid, slot: neo4j.int(slot), weight: neo4j.int(weight), tsMs: neo4j.int(tsMs) });
}

async function handleFinalizeVote(session, ev) {
  const { sessionId, slot, validatorIdx, candidateId, tsMs } = ev;
  const vNid = await ensureValidator(session, sessionId, validatorIdx);
  const certNid = certNodeId(sessionId, candidateId);

  await runQuery(session,
    `MATCH (v:Validator {nodeId: $vNid}), (cert:Cert {nodeId: $certNid})
     MERGE (v)-[r:finalize {slot: $slot, validatorIdx: $validatorIdx}]->(cert)
     SET r.tsMs = $tsMs`,
    { vNid, certNid, slot: neo4j.int(slot), validatorIdx: neo4j.int(validatorIdx), tsMs: neo4j.int(tsMs) });
}

async function handleFinalizeCert(session, ev) {
  const { sessionId, slot, candidateId, weight, tsMs } = ev;
  const certNid = certNodeId(sessionId, candidateId);
  const bNid    = blockNodeId(sessionId, candidateId);

  await runQuery(session,
    `MERGE (b:Block {nodeId: $bNid})
     SET b.slot = $slot, b.candidateId = $candidateId, b.sessionId = $sessionId`,
    { bNid, slot: neo4j.int(slot), candidateId, sessionId });

  await runQuery(session,
    `MATCH (cert:Cert {nodeId: $certNid}), (b:Block {nodeId: $bNid})
     MERGE (cert)-[r:accepted {slot: $slot}]->(b)
     SET r.weight = $weight, r.tsMs = $tsMs`,
    { certNid, bNid, slot: neo4j.int(slot), weight: neo4j.int(weight), tsMs: neo4j.int(tsMs) });
}

async function handleSkipVote(session, ev) {
  const { sessionId, slot, validatorIdx, tsMs } = ev;
  const vNid  = await ensureValidator(session, sessionId, validatorIdx);
  const skNid = skipNodeId(sessionId, slot);

  await runQuery(session,
    `MERGE (sk:SkipEvent {nodeId: $skNid})
     ON CREATE SET sk.slot = $slot, sk.sessionId = $sessionId`,
    { skNid, slot: neo4j.int(slot), sessionId });

  await runQuery(session,
    `MATCH (v:Validator {nodeId: $vNid}), (sk:SkipEvent {nodeId: $skNid})
     MERGE (v)-[r:skip {slot: $slot, validatorIdx: $validatorIdx}]->(sk)
     SET r.tsMs = $tsMs`,
    { vNid, skNid, slot: neo4j.int(slot), validatorIdx: neo4j.int(validatorIdx), tsMs: neo4j.int(tsMs) });
}

// ── Dispatch ────────────────────────────────────────────────────────────────

async function dispatch(session, ev) {
  switch (ev.event) {
    case 'Propose':        return handlePropose(session, ev);
    case 'NotarizeVote':   return handleNotarizeVote(session, ev);
    case 'NotarizeCert':   return handleNotarizeCert(session, ev);
    case 'FinalizeVote':   return handleFinalizeVote(session, ev);
    case 'FinalizeCert':   return handleFinalizeCert(session, ev);
    case 'Block':          /* handled via FinalizeCert */ break;
    case 'SkipVote':       return handleSkipVote(session, ev);
    case 'SkipCert':       /* no separate node */ break;
    case 'SessionStart':   console.log(`[relay] session=${ev.sessionId} scenario=${ev.scenario}`); break;
    case 'SessionEnd':     console.log(`[relay] done: finalized=${ev.finalizedBlocks} skipped=${ev.skippedSlots}`); break;
    case 'Receive':        /* informational only */ break;
    default:               console.warn('[relay] unknown event:', ev.event);
  }
}

// ── Main ────────────────────────────────────────────────────────────────────

async function main() {
  const tracePath = process.argv[2] || path.resolve(__dir, 'trace.ndjson');

  if (!fs.existsSync(tracePath)) {
    console.error(`[relay] trace file not found: ${tracePath}`);
    console.error('[relay] Run ConsensusHarness first to generate the trace.');
    process.exit(1);
  }

  console.log(`[relay] reading ${tracePath}`);
  console.log(`[relay] connecting → ${URI}`);

  await driver.verifyConnectivity();
  const session = driver.session({ database: DATABASE });

  const rl = readline.createInterface({
    input: fs.createReadStream(tracePath),
    crlfDelay: Infinity,
  });

  let lineNo = 0;
  let errors = 0;

  for await (const line of rl) {
    lineNo++;
    const trimmed = line.trim();
    if (!trimmed) continue;
    try {
      const ev = JSON.parse(trimmed);
      await dispatch(session, ev);
    } catch (e) {
      console.error(`[relay] line ${lineNo} parse error:`, e.message);
      errors++;
    }
  }

  await session.close();
  await driver.close();
  console.log(`[relay] processed ${lineNo} lines, errors=${errors}`);
}

main().catch(e => { console.error('[relay] fatal:', e); process.exit(1); });
