# Single-node jetton TPS benchmark — results

Date: 2026-06-12. Branch `bench-jetton-tps`. See DESIGN.md for architecture.

## Setup

- **Hardware**: i9-13900H (20 threads, laptop), 125 GiB RAM, Samsung 990 PRO 4 TB NVMe
  (ext4, noatime) dedicated to the benchmark. Linux 7.0.2-arch1.
- **State** (`bench-state-gen`, 17 min to generate): 192M accounts = 80M wallet-v5 +
  80M jetton wallets + 32M ballast (~2.2 KB each) + 1 minter; 1.28B cells; **208 GB
  celldb** (> RAM, page cache can never hold it; bottom ~13 levels of the accounts
  dictionary are effectively always cold).
- **Network**: 1 validator, simplex consensus, target block rate 400 ms, max_split=0
  (single basechain shard), `--disable-state-serializer`, celldb v2.
- **Workload**: wallet-v5 externals doing TEP-74 jetton transfers
  (`forward_ton_amount=0`, `response_destination=null`) → exactly **3 transactions
  and 2 internal messages per transfer**, all landing in the same block under normal
  conditions. Each wallet used once (seqno 0); recipients pseudorandom across all 80M.
  jetton TPS = transfers/s; raw TPS = 3 × jetton TPS.
- "Mainnet-like limits" = tontester defaults (block soft 512 KB / hard 1 MB,
  gas soft+hard 100M); "unleashed" = block_limit_mul=40, gas_limit_mul=10.
- Cold runs drop OS page caches first. Latency = send → block observed locally.

## Headline numbers

| configuration | jetton TPS (steady) | raw TPS | limiting factor |
|---|---|---|---|
| 208 GB state, mainnet limits, baseline | **84–105** | 250–320 | serial celldb reads on collator thread |
| 208 GB state + account prefetch (this branch) | **132–140** | 395–420 | ~190 ms effective collation budget |
| 208 GB state + prefetch + 32 GB celldb cache | 145 | 436 | same (cache adds ~3%) |
| RAM-resident state, mainnet limits | **236–241** | 709–721 | block soft byte limit (512 KB ≈ 320 tx) |
| RAM-resident state, unleashed | collapses (bug, see below); healthy blocks imply **~900–1850** | | per-transfer CPU (~0.54 ms) × collation budget |
| 208 GB state, unleashed + prefetch + cascade fix | 147 | 441 | same I/O budget as mainnet limits — raising limits doesn't help when reads dominate |

Latency at sub-saturation (small RAM-resident state, rate ≪ ceiling): inclusion
p50 ≈ **0.82 s**, ≈ 2 block cycles (queue → next collation → finalize+observe).
The theoretical ideal block_delay/2 + processing ≈ 0.4–0.6 s; the extra cycle comes
from externals having to be in the mempool before a collation snapshot picks them up.
At any rate above the ceiling the mempool backlog dominates latency (tens of seconds
to minutes) — the system saturates, it does not degrade block production.

## The bottleneck chain (what limits single-shard TPS, in order)

1. **Serial cold reads on the collator thread** (baseline). Every account fetched
   during collation costs a ~13-level descent of the 192M-account dictionary; each
   level is a synchronous ~8 KB RocksDB read (~70–100 µs). Per transfer the collator
   touched ~58 cold cells ≈ 8 ms wall time — all sequential, ~9k IOPS at **~1% of the
   NVMe's capability**. Collation burned 0.31–0.39 s of its 0.4 s slot in I/O waits.
   The wallet's path is pre-warmed by the mempool admission check, but the two jetton
   wallets (sender's and recipient's) are only discovered mid-execution.

2. **Fix: background account-path prefetch** (commits `23cf5fb71`, `63905ee77`).
   When the collator enqueues a newly-generated internal message (or drains pending
   externals), a background pool walks the accounts dictionary for the destination on
   a snapshot of the prev-state root. Because `ExtCell` loads are atomic and the
   snapshot shares the cell instances the collator later traverses, prefetched paths
   cost the collator thread zero reads. The collator's external→internal batch
   structure provides exactly the lookahead needed (jw1 prefetches while wallet txs
   execute, jw2 while jw1 txs execute). Result: per-transfer collator time 8 ms →
   ~3.5 ms, **+51% TPS** on the 208 GB state. Mean wc0 collation time dropped from
   ~0.35 s to ~0.19 s — collation is no longer I/O-saturated.

3. **Effective collation budget ≈ 190 ms of the 400 ms slot.** For shard blocks the
   producer sets `soft_timeout = wait_externals_until = slot_start` and nominally
   starts collating one slot early (`start_collate_before = target_rate`,
   block-producer.cpp:100-126), but collation cannot start until the parent block's
   state is resolved, which lands ~halfway through the window. Observed externals
   budget ≈ 186–190 ms/block. This now binds: TPS ≈ budget ÷ per-transfer-cost × 2.5.
   Every measured config fits this model (8 ms → ~25 ext/block; 3.5 ms → ~53).

4. **Per-transfer CPU ≈ 0.54 ms** (RAM-resident state: 3 VM executions, dict
   updates, message routing; measured from over-budget collations packing ~830 mixed
   txs with gas 9–10M in ~0.45 s). With mainnet byte limits the 512 KB soft limit
   binds first (320 tx ≈ 236 jetton TPS); unleashed, the budget×CPU ceiling binds.

4b. **Robustness bug found & half-fixed: untime-bounded in-block work.** With
   raised limits a collation could legally pack ~800 externals plus their ~1600
   cascading internal txs ≈ 0.45 s of work — beyond the producer's
   `slot_start + 400 ms` candidate deadline. The producer then finalizes an empty
   fallback (attempt-1) block, and the dead candidate's externals are already
   deleted from the mempool ⇒ goodput collapses to ~0 instead of degrading.
   Fix (committed): `process_new_messages` now defers the cascade to the out queue
   once the soft timeout passes, like BLOCK FULL does. This cured the 208 GB
   unleashed runs (147 jetton TPS, stable). A second instance of the same disease
   remains: on a RAM-resident state a single block can ingest ~870 externals,
   whose ~1740 deferred internal messages then make the NEXT collation overrun the
   deadline in inbound-internal-queue processing — the queue is never dropped, so
   the system livelocks producing empty blocks (mid4-unl runs). Needs the same
   time-bounding treatment in the inbound-internal/dispatch-queue loops.

5. **Ingestion**: external-message admission (one VM check per message in the
   mempool actor) plus the liteserver path saturates around ~4–6k ext/s; at 10k/s
   sends time out (95% rejected). Would need parallel admission checks for more.

## Run matrix (details)

| run | state | limits | rate | duration | jetton TPS | incl p50 | notes |
|---|---|---|---|---|---|---|---|
| full-mul1-r100 | 208GB cold | mul=1 | 100 | 60s | 60 | 11.3 s | warming 40→150 tx/blk |
| full-mul1-r300-diag | 208GB cold | mul=1 | 300 | 180s | 84 | 35.6 s | plateau 105–115 tx/blk; collation 0.31–0.39 s |
| full-mul1-r60-cold | 208GB cold | mul=1 | 60 | 120s | 64 (all incl.) | 3.3 s | early-cold backlog drains |
| full-mul1-r400-warm | 208GB | mul=1 | 400 | 600s | 100 | (saturated) | 100→160 tx/blk over 10 min |
| full-pf-r300 | 208GB cold | mul=1 | 300 | 180s | **132** | (saturated) | prefetch; collation ~0.19 s |
| full-pf-r600 | 208GB cold | mul=1 | 600 | 180s | **140** | (saturated) | prefetch ceiling |
| full-pf-cache32-r600 | 208GB cold | mul=1 | 600 | 180s | 145 | (saturated) | +32 GB block cache: +3% |
| mid2-mul1-r1000 | 2M RAM | mul=1 | 1000 | 60s | **236** | (saturated) | 320 tx/blk = byte-limit |
| mid3/mid4-unl-r2000/r4000 | 2M RAM | unleashed | 2–4k | 60–90s | ~0 | — | collapse: one ~870-tx block, then empty (bug 4b, second instance) |
| full-pf-unl-r2000 (pre-fix) | 208GB cold | unleashed | 2000 | 180s | ~0 | — | collapse (bug 4b, first instance) |
| full-pf2-unl-r1000 (cascade fix) | 208GB cold | unleashed | 1000 | 180s | **147** | (saturated) | stable; same ceiling as mainnet limits (I/O budget binds) |
| mid4-mul1-r1000 (cascade fix) | 2M RAM | mul=1 | 1000 | 60s | 241 | (saturated) | no regression from fix |
| full-pf2-r600 (cascade fix) | 208GB cold | mul=1 | 600 | 180s | 144 | (saturated) | no regression from fix |

## The 10⁴ jetton TPS question

10,000 jetton transfers/s = 30,000 transactions/s = ~12,000 transactions per 400 ms
block. Single-shard, this is out of reach **regardless of I/O**: at the measured
~0.54 ms CPU per transfer (3 txs), even a full 400 ms budget yields ~740
transfers/block ≈ **1,850 jetton TPS absolute single-thread ceiling**; with the
current ~190 ms effective budget, ~880. Healthy over-budget collations observed
during the bug hunt (~830 mixed txs / 9–10M gas per block) confirm the model.

Paths to 10⁴:
- **Sharding (the intended one)**: at ~140 (realistic, 208 GB state) to ~800
  (RAM-state) jetton TPS per shard, 10⁴ needs roughly 12–64 shards — well within
  max_split capabilities, and shards parallelize both CPU and I/O cleanly.
- Single-shard would require parallel intra-block transaction execution
  (account-disjoint speculative execution) plus recovering the full slot budget
  (overlap collation with parent-state resolution) plus parallel mempool admission.

## Incidental findings

- The liteserver's block index (ltdb) transiently lags the shard client when blocks
  are large and fast; clients must treat "block not found (possibly out of sync)" as
  retryable (bench-spam fix in `spam.cpp`).
- `--celldb-cache-size` (default 1 GB) is nearly irrelevant for this access pattern —
  the OS page cache already backs the same reads; 32 GB bought ~3%.
- The mempool's per-address limits (30 msgs / 10 s) and MAX_WALLET_SEQNO_DIFF are
  benchmark-relevant constraints when designing spam (one wallet ⇒ one external).
- Account-path prefetch stats appear in the collation log as
  `Account path prefetch: issued=N dropped=M` (verbosity ≥ 2).

## Reproduce

```sh
# one-time: generate the 208 GB state (~17 min)
build/benchmark/bench-state-gen gen --v5-count 80000000 --ballast-count 32000000 \
  --seed-hex abab…ab --out-dir /mnt/bench/state-full

# a benchmark run
uv run python test/integration/bench_jetton.py \
  --manifest /mnt/bench/state-full/manifest.json \
  --rate 600 --duration 180 --warmup 20 \
  --net-dir /mnt/bench/net-bench --out-dir /mnt/bench/results/my-run
```

---

# Phase 2: single-shard optimization toward 10⁴ jetton TPS (2026-06-12)

Goal: push the single-shard, disk-bound (state ≫ RAM) regime as far toward 10⁴ jetton
TPS as possible without compromising correctness or (more than minimally) public ABI.
All work on branch `bench-jetton-tps`; plan + per-workstream findings in PHASE2.md.

## The ladder (208 GB-class state, mainnet-parity limits, 400 ms blocks, cold caches)

| step | steady jetton TPS | what changed |
|---|---|---|
| phase-1 baseline | 84 | — |
| + phase-1 prefetch | 132 | fire-and-forget account prefetch pool |
| + C0 mainnet parity | 195 | full collated data (param-8 cap 0x3EE), real mainnet block limits (1 MB soft bytes); validation = pure CPU ~28 ms, zero celldb reads |
| + W5 celldb bundling | 210–219 | 'bundle' records (tag −2): 5-level dict slabs + leaf/account/data bundles; 93→30 device reads per transfer; bundled state /mnt/bench/state-full-b5 (256 GB, root hash identical) |
| + W1 io-mux (+W3/W4/W7) | 221–238 | awaited account-path walkers replace the prefetch pool; account/queue I/O wait → 0 |
| + W6 window-cadence fix | (in next row) | speculative cross-window collation on self-parents; boundary stall 92–180 ms → ≤9 ms |
| + W8 device split | **313–330** | fsync-heavy DBs (consensus, statedb, archive) moved off the read device; no durability relaxation |

Raw TPS at the final config: ~940–990 tx/s sustained, cadence steady at 402 ms.
Latency: saturated runs are backlog-dominated (p50 tens of seconds); sub-saturation
inclusion p50 remains ≈ 2 block cycles.

## What we learned (bottleneck chain, in discovery order)

1. Serial celldb descents on the collator thread (phase 1) — fixed by W5 (fewer reads)
   × W1 (overlapped reads, no lingering threads: bounded awaited walkers).
2. Block limits were never the binder at mainnet values once validation went
   collated-data-only (C0); ×10 limits change nothing on the disk-bound state.
3. Candidate-loss collapse under overload — W3's simplex-aware mempool (hold per
   candidate, re-add on history collapse) = 8.7× goodput in the collapse regime; plus
   the phase-1 soft-timeout cascade deferral.
4. Admission ceiling ~4 k ext/s — W7's worker-pool checker sustains ~8 k/s admitted
   (the VM check ~1 ms/msg is the remaining floor).
5. Leader-window boundary stall (P0: 92 ms, grown to 160–180 ms with fatter blocks) —
   W6 speculation on self-produced parents only; foreign parents still require local
   validation before collation (load-bearing safety property, per review).
6. fsync/jbd2 interference: ~122 syncs/s (archive index 55/s, simplex DB 38/s) forcing
   ~114 journal commits/s on the read device — W8 device split: +15–18%, no durability
   loss. Default-off relaxed-sync engine flags exist for measurement only.
7. Liteserver advertised-tip desync (W4) and an inverted "out of sync" diagnostic —
   fixed; advertised tips are now always servable.

## Where the next factor of ~30 lives (to reach 10⁴)

Measured residuals at the final config:
- ~120–130 ms/block of genuine cold-read latency inside intake that io-mux depth
  doesn't yet hide (deeper windows + W5 bundle aging + admission/collation I/O
  interference at high rates are the levers; archive temp-index write coalescing is a
  cheap +).
- Serialize/proof tail ~78–89 ms/block — W6 produced a safe overlap design
  (block-id/file-hash ordering constraint), not yet implemented.
- Per-transfer CPU ≈ 0.5 ms single-threaded — the hard wall. **W2 cross-account
  parallel execution** (design note in W1's report: per-account lanes, commit-ordered
  lt assignment, usage-tree replay at commit; blocks valid but not bit-identical —
  acceptable since the collator defines the block) is the only path to 10⁴ on one
  shard: ~12 k tx per 400 ms needs ~15–20 effective cores in execution plus
  admission scaled to ~10 k VM checks/s (~12 cores) — i.e. 10⁴ single-shard is a
  full-machine parallel-execution project, not a tuning exercise. Realistic stacked
  estimate short of W2: intake-bound ~500–700 jTPS.

## Reproducing the final number

```sh
uv run python test/integration/bench_jetton.py \
  --manifest /mnt/bench/state-full-b5/manifest.json \
  --net-dir <dir-on-other-ssd> --celldb-checkpoint-dir /mnt/bench/work/ckpt \
  --rate 1200 --duration 180 --warmup 15 --spam-arg=--connections=8
```
Results: /mnt/bench/results/p2-FINAL-r1200 (330 jTPS steady).

### Final-binary RAM-resident measurements (addendum)

| config | steady jetton TPS | notes |
|---|---|---|
| state-mid (RAM), mainnet limits, r2000 | **476** | 1427 tx/s; BYTE-BOUND on the mainnet 1MB soft limit (saturated blocks ~576 txs = ~192 transfers ≈ 1MB size estimate, ~1.42MB fetched BoC; gas only 4.9M of 10M). Theoretical ceiling at these limits ≈ 500 jTPS — this run is at ~96% of it. (Earlier "per-transfer CPU ~1ms binds" attribution was wrong.) |
| state-mid (RAM), ×10 limits, r3000 | **406** | mega-block (933 tx) + empty-block cycling — the unfixed inbound-internal/dispatch-queue time-bounding bug (RESULTS.md §4b, second instance); W3 prevents external loss so it recovers per cycle instead of collapsing |

Implication: on RAM-resident state the next lever is per-transfer collation CPU (profile
the proof-building and io-mux overhead at RAM speeds) and the §4b-second-instance fix;
limits bind nowhere on the final binary.
