# Phase 2: push single-shard toward 10⁴ jetton TPS

Context: read benchmark/DESIGN.md + benchmark/RESULTS.md first. Phase-1 found single-shard
ceilings of 132–145 jTPS (208GB state) / 236–241 (RAM state, byte-limit) with the collator
bottlenecked on serial celldb reads, an effective ~190ms collation budget, and ~0.54ms CPU
per transfer. Phase-2 goal: 10⁴ jetton TPS single shard (= 30k tx/s = ~12k txs per 400ms
block), per explicit direction. Constraints: (1) NO correctness compromises; (2) ABI: the
BLOCK format is public ABI — minimal changes only if unavoidable; COLLATED DATA is exchanged
between validators only — fine to modify arbitrarily; (3) prefer eliminating the phase-1
prefetcher (no lingering threads) once proper I/O multiplexing exists; (4) the primary regime
is the 208GB state that does NOT fit in RAM (disk-bound); RAM-resident numbers are secondary.
Ordering note: C0 lands BEFORE P0 — config parity (full collated data) changes what profiling
measures.

## Machine-sharing protocol (MANDATORY for every agent)

- Any activity that launches validator-engine / runs nets / benchmarks MUST hold the lock:
  `flock /mnt/bench/work/bench.lock <cmd>` (wait with `flock -w 7200` if needed).
- Heavy processes: prefix `choom -n 1000 --`.
- NEVER rebuild binaries in the MAIN build dir (`src/build`) while not holding the lock —
  benchmarks run from it. Agents working in worktrees use their own build dir
  (cmake with `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`,
  same flags as src/build/CMakeCache.txt: clang21, RelWithDebInfo, Ninja).
- Scratch space: /mnt/bench/work/<agent-name>/.

## Workstreams

- **P0 profile**: where do 400ms of wc0 collation real time go when trx_* ≈ 60ms?
  (consensus explorer over session-logs + collator stats + perf). Output drives W1/W2 design.
- **C0 config parity**: enable full collated data (validation must not read celldb), sync
  shard block limits + global version with current mainnet values in the benchmark config.
- **W1/W2 collator multiplexing & parallelism** (after P0): first saturate ONE collation
  thread by interleaving I/O waits with execution (stackful coroutines or async dict/account
  loads); then parallelize execution across independent accountchains. Replaces the
  account-prefetch thread pool.
- **W3 simplex-aware ext pool**: continue the user's WIP (branch `mempool` + a stash on top;
  reconstruct intent, reimplement cleanly on HEAD). Core idea: candidates remove externals
  from the mempool as today, but when a final certificate collapses histories, walk the
  REJECTED blocks and re-add their externals to the mempool. (Writing rejected externals
  into collated data: out of scope.) Fixes the dead-candidate-loses-externals collapse
  (RESULTS.md §4b) properly.
- **W4 liteserver tip desync**: unify/fix the multiple independently-tracked mc tips so
  "block not found (possibly out of sync)" stops happening (ltdb vs shard client vs ls).
- **W5 celldb layout** (after P0): (a) bundle ~5 levels of the accounts dict per IO op
  (one ~4KB read serves a whole descent segment; relates to celldb-compress-depth);
  (b) [explore only] single materialized state + Merkle updates for the rest.

## C0 findings (mainnet config parity, 2026-06-12)

Mechanism: "full collated data" is carried by capability bit `capFullCollatedData = 512`
in **ConfigParam 8** (`GlobalVersion.capabilities`, ton/ton-types.h). The collator turns it
on from config (`validator/impl/collator.cpp:947`) and emits Merkle proofs of every
accessed prev-state/out-queue cell into the collated data; ValidateQuery does not read
the config bit at all — it detects the proof roots while unpacking collated data
(`validate-query.cpp:681/711`), sets `full_collated_data_`, and then skips
`load_prev_states()` entirely (line 393): prev state, neighbor out-queues and account
storage dicts are reconstructed from `virt_roots_`, so wc0 validation performs no celldb
state reads. Upstream's separate CollatorConfig param (41) is EMPTY on mainnet — the
param-8 capability bit is the live mechanism.

Current mainnet values (toncenter `getConfigParam`, fetched 2026-06-12):

- **Param 8**: version=14, capabilities=0x3EE=1006
  (2|4|8|32|64|128|256|512 = createstats, bouncemsgbody, reportversion, shortdequeue,
  storeoutmsgqueuesize, msgmetadata, **defermessages**, **fullcollateddata**;
  no capIhr, no capSplitMergeTransactions).
- **Param 22** (mc block limits, underload/soft/hard): bytes 131072/524288/1048576,
  gas 200000/1000000/2500000, lt_delta 1000/5000/10000.
- **Param 23** (basechain block limits): bytes 262144/1048576/2097152,
  gas 2000000/10000000/20000000, lt_delta 1000/5000/10000.
- **Param 41** (upstream CollatorConfig): empty.

tontester (`zerostate.py`): param 8 now mainnet 0x3EE with
`NetworkConfig.full_collated_data` (default True) gating bit 512; params 22/23 use the
mainnet baselines above, `block_limit_mul` scaling the bytes+lt soft/hard limits and
`gas_limit_mul` the gas soft/hard limits. NOTE vs phase-1 configs at mul=1: basechain
gas soft limit dropped 100M→10M and lt_delta soft 500k→5k, so phase-1 numbers are not
directly comparable; basechain bytes soft limit rose 512K→1M (mainnet).

Verification (208GB state, rate 300, 60s, mul=1, `/mnt/bench/results/c0-parity-r300`):
included=13276, tps_included=586, jetton_tps=195. wc0 ValidateQuery perf logs contain NO
`wait_block_state #i` action (the prev-state celldb load) anywhere in the run, and wc0
validation cpu time ≈ real time (e.g. 60.2ms cpu / 60.4ms real at block 199) — validation
is pure CPU over collated-data proofs. Timings: validation n=387 mean 28.5ms max 118ms;
collation n=387 mean 175ms max 491ms (collation still I/O-bound; that is P0/W1 territory).

## P0 findings (collation profiling, 208GB state, mainnet-parity config, 2026-06-12)

Runs (rate 600, warmup 15, wallet offsets 3.0–3.2M, cold caches, single shard,
400ms slots): `p0-r600` (120s, node-verbosity 3, dwarf perf), `p0-r600-v1` (120s,
verbosity 1 — canonical), `p0-r600-lbr` (60s, verbosity 1, LBR perf). Raw artifacts in
/mnt/bench/work/p0/ (session-logs{,2,3}, node1-log*.zst, perf{,2,3}.data, diskstats*.csv,
analyze_p0.py + per-block CSVs, classify_perf.py + perf*-classified.txt) and
/mnt/bench/results/p0-r600*/. Background contention: two agents compiled in bursts;
perf windows were taken in a quiet window (load1 ≈ 6 during the canonical run,
20 hw threads); the engine itself accounts for most of that load.

**Headline**: verbosity-1 run included 14,823 transfers in 105s ⇒ **118.6 jetton TPS**
(356 raw). Saturated blocks (n=186): 71.2 externals, 146 txs, 444KB block,
**366KB collated data**, cycle p50 405ms. NOTE: node-verbosity 3 costs ~34% goodput
(78.5 jTPS in the otherwise identical run — logging serializes on hot actor threads);
and rate 600 yields *lower* goodput than rate 300 (118 vs 195 jTPS in c0-parity-r300):
over-saturation overhead (admission + pool + internals backlog), relevant to W3.

### 1. Slot timeline (saturated, per ~405ms cycle, session-log events + collator stats)

| segment | mean | notes |
|---|---|---|
| prev candidate ready → collate request | 21ms | p50 0.3ms; **92ms at every 4th slot** (leader-window restart, single validator) |
| request → Collator ctor | 0.0ms | parent state handed over in-memory: producer does MerkleUpdate::apply itself (chain-state.cpp) and passes `prev_block_state_roots` — **the phase-1 "parent-resolve eats half the slot" no longer exists** |
| ctor → do_collate (preinit: mc state, neighbors, queues) | 1.9ms | |
| **do_collate → externals cutoff (intake)** | **333ms** | ends at `wait_externals_until = slot_start`; split measured in the verbose run: **dispatch+inbound internals 153ms, externals 167ms**; ext-queue idle wait ≈ 0 (mempool always backlogged) |
| post-externals (finalize+serialize, "post_ext") | 39ms | runs *past* slot_start; delays the next request |
| candidate handoff to producer | 10ms | clone/copy of 444KB block + 366KB collated data |
| total collation (ctor → stats) | 374ms | occupies the full slot; the c0 "mean 175ms" was an artifact of averaging over non-saturated warmup blocks |

Post-collation pipeline (overlapped with the next collation, does not consume slot
budget): candidate received +0.1ms → validation start +18ms → validation 44ms
(pure CPU, no celldb reads — full collated data works) → notarize cert +12ms →
finalize cert +14ms. Finalization lands ~90ms after the candidate; only gates
empty-block fallbacks, never block production here (0 empty slots, all attempts==0).

So the next block's intake window ≈ 400 − post_ext(39) − handoff(10) − gap(21) −
preinit(2) ≈ 330ms ✓. The old "collation gets only ~190ms" is dead; its successor:
**externals get only ~167ms** because the first ~153ms of every intake go to the
previous blocks' deferred internal cascades (out-queue grew +32/block at rate 600 —
the system was not even at internals equilibrium; longer runs would shift the split
further toward internals).

### 2. Collation internal breakdown (collator work_time stats, saturated means, run v1)

| phase | real ms | cpu ms | iowait ms |
|---|---|---|---|
| trx_tvm (VM execution) | 33.9 | 25.2 | 8.7 |
| trx_storage_stat + trx_other | 13.6 | 13.3 | 0.3 |
| combine_account_transactions | 8.3 | 8.2 | 0.1 |
| create_shard_state (new state + Merkle update) | 8.9 | 8.9 | 0.0 |
| create_block | 2.2 | 2.2 | 0.0 |
| **create_collated_data (proofs of accessed cells — NEW)** | **13.8** | 13.7 | 0.1 |
| create_block_candidate (BoC serialization) | 5.9 | 5.8 | 0.0 |
| queue_cleanup + preinit + final_storage_stat | 0.4 | 0.4 | 0.0 |
| **unattributed (account loads, dict descents, msg routing)** | **283.8** | 100.8 | **183.0** |
| **TOTAL work_time** | **370.7** | **178.5** | **192.2** |

**52% of collation real time is I/O wait**, all of it inside the intake loop
(the named phases are CPU-bound). The user's "trx_* ≈ 60ms" observation is the
trx_tvm+trx_other+trx_storage_stat rows (~48ms here): the trx timers start *after*
`make_account` — the per-message account-path descent and cell loads land in
"unattributed". Full-collated-data cost: create_collated_data 13.8ms + bigger
create_block_candidate (5.9ms vs ~2.7 pre-C0) ≈ **+17ms/block ≈ 4% of the slot**,
plus 366KB/block of extra candidate bytes. Validation: 44ms pure CPU.
Account-path prefetch is active (issued ≈ 330/block, dropped 0) and yet 183ms of
serial read waits remain: prefetch covers the destination-account paths it can see,
but storage-stat cells, dict-update paths and second-hop loads still miss.

### 3. CPU profile (perf, LBR call graphs, whole process, saturated)

Process CPU ≈ 3 cores of 20. Subsystems: collator-tagged 10.7%, network 13.5%,
liteserver (serving bench-spam) 13.2%, account-prefetch pool 7.0%, validation 3.8%,
mempool admission 2.2%, celldb commit 1.6%, archive 2.7%, rest deep-stack/other.
By component across subsystems: **ed25519 ≈ 25%** (mempool admission VM checks +
network channel crypto + signing), **rocksdb Get path ≈ 18%**, cell-slice ops ≈ 10%,
BoC serialize ≈ 7.5%, allocator ≈ 6.5%, sha256 ≈ 5.5%, TVM dispatch ≈ 1%.
The collation-critical thread runs at only ~44% CPU duty (178ms/405ms) — CPU is not
the binding constraint; the I/O wait is.

perf notes: `--call-graph dwarf` unwinds only ~14% of samples on this RelWithDebInfo
clang build (fat/deep actor+TVM stacks exceed the dump; libdw gives up) — **use
`--call-graph lbr`** (works perfectly, 32-frame depth) or add
`-fno-omit-frame-pointer` (-1–2% perf) for profiling builds. sched_switch off-cpu
stacks are kernel-only for the same reason (captured, not usable).

### 4. Disk picture (/proc/diskstats, steady state, canonical run)

16.5k read IOPS, 127MB/s, avg read 7.7KB, avg latency 0.10ms, device util 56%
(NVMe nowhere near saturated — the serial chain is). Writes 377 IOPS / 55MB/s.
Per transfer (~170/s): **~94 device reads total** (collator + prefetch pool +
admission + liteserver), of which **~27 serial reads on the collator critical path**
(192ms iowait ÷ 0.10ms ≈ 1,900 reads/block ÷ 71 transfers).

### 5. Ranked verdict (per 405ms cycle) and implications

1. **Serial celldb I/O wait in the intake loop: 183–192ms (47%)** — W1's target.
   Multiplexing I/O within one collation thread (async account loads / stackful
   coros) converts ~insert-latency×count into max(CPU, IO-depth-limited time):
   intake could roughly double at util ≪ 100%. W1 is the single biggest lever.
2. **Intake CPU: ~128ms** (TVM 25ms + tx scaffolding ~14ms + account
   unpack/dict/routing ~89ms). W2 (parallel execution across accountchains) attacks
   this *after* W1; alone it saves at most ~30% of the slot. Per-transfer collator
   CPU ≈ 2.5ms (3 txs incl. next-block internals).
3. **Post-externals serialization: 39ms (10%)** — pure CPU after the externals
   cutoff; it directly shortens the *next* intake window. Proof building (14ms) +
   candidate BoC (6ms) + state/block (11ms) + combine (8ms). Could overlap with the
   next collation's intake (pipeline the serialize stage) for a free ~10% window gain.
4. **Producer/window overhead: 21ms mean + 10ms handoff** — 92ms stall at every
   leader-window boundary (4 slots) even with a single validator; cheap fix candidate
   (pre-arm the next window's first collation).
5. **Internals-vs-externals scheduling**: deferred cascades consume the first ~46%
   of every intake window and the backlog still grows (+32 msgs/block) — at true
   equilibrium externals/block would be *lower*. Any intake speedup helps both; W3's
   pool work should also re-meter externals admission to internals capacity.
6. **W5 (celldb layout)**: ~13-level dict descents at ~1 cell/IO today; 5-level
   bundling would cut the critical path ~27 → ~6 reads/transfer and total IOPS ~4×,
   multiplying W1's headroom (more in-flight reads per NVMe roundtrip budget).

### Consensus-pipeline answer to the driving question

Collation seemingly "occupies its full 400ms" because it *does*: the producer
requests the next collation immediately after the previous candidate (no parent-state
stall at parity config), and the collator spends the whole slot — but only ~48ms of
it is transaction work (trx_*); ~190ms is serial cold-read wait, ~80ms is per-message
CPU outside trx timers, ~39ms is end-of-block serialization+proofs, and ~153ms of the
"transaction" budget is really the previous block's deferred internal cascade.
Validation/notarization/finalization are fully pipelined and irrelevant to the slot
budget at parity config.

## Status log (append-only; agents update their line on completion)

- P0: done — slot timeline + collation phase decomposition + LBR CPU profile + disk
  profile on 208GB/parity (runs p0-r600*); verdict: 52% of collation is serial celldb
  I/O wait (W1), intake CPU ~32% (W2), collated-data proofs cost ~4% of slot, no
  parent-resolve gap remains; externals get ~167ms of the slot, internals backlog the
  first ~153ms. W1 → W5 → post-ext pipelining is the recommended order.
- C0: done — full collated data on by default (param-8 cap 0x3EE, mainnet parity) +
  mainnet 22/23 block limits as mul baselines; verified wc0 validation reads no celldb
  state (r300 run: 586 tps included, validation pure-CPU ~28ms mean).
- W1: done (merged) — io-mux: awaited account-path walkers (8 coroutine walkers, window 64) + shadow inbound-queue walker + out-queue insert warming; replaces AccountPrefetchPool (removed). Eliminates all account/queue celldb io-wait (cold-collator A/B +75%; vs prefetch baseline +7.7%). KEY FINDING: residual ~175ms/block real-vs-cpu gap is page-cache reclaim (folio_wait) + fsync/jbd2 pressure, NOT celldb reads -> W8. W2 (cross-account parallel execution): design note in W1 report, not implemented
- W3: done (merged) — simplex-aware ext pool: candidates hold externals, history collapse re-adds rejected blocks' externals; 8.7x goodput on the unleashed collapse scenario; unit tests in test/validator/test-ext-message-pool.cpp
- W4: done (merged) — liteserver advertised tip never outruns the shard client; inverted "possibly out of sync" diagnostic fixed; 0 desync errors under load
- W5: done (merged) — celldb 'bundle' records (tag -2): 5-level dict slabs + leaf+account+data bundles; 93->30 reads/transfer, 124->209.5 jTPS on bundled 256GB state (/mnt/bench/state-full-b5, root hash identical to state-full)
- W7: done (merged) — ExtMessageChecker worker pool (24 workers) off the pool actor; admission ~8k/s with ~0 errors (was ~hundreds/s collapse), backpressure with fast rejects; found ~1ms VM floor per check + recurring ~30s engine-wide contention bursts (suspect state GC, future work)
- W6: done (merged) — speculative cross-window collation (self-parent only, adoption gated on base match at real window start; foreign-parent validation wait untouched; 4-node adversarial test PASS). Boundary collate-gap 92-180ms -> <=9ms, unimodal 400ms cadence. Serialize-tail overlap (~50ms/slot) documented, not implemented.
- W8: done (merged) — fsync map: archive index/pack 55 sync/s, simplex DB 38/s, 366 fsync-ms/s on the read device. Best config: DEVICE SPLIT (node dir on second SSD, celldb checkpoint symlinked; orchestrator --celldb-checkpoint-dir), +15-18% with NO durability relaxation. Default-off flags --celldb-relaxed-sync / --consensus-db-relaxed-sync (DANGEROUS) exist for measurement. Reclaim regime absent in 2-min cold runs (corrects W1 attribution); residual ~120-130ms gap = genuine cold-read latency.
- FINAL (W6+W8+everything, state-full-b5, mainnet limits, cold, device split): 313 jTPS @r600, 330 jTPS @r1200 steady (~990 raw tx/s), 402ms cadence. Phase-1 start was 84.
