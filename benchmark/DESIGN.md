# Single-node jetton TPS benchmark — design

Goal: benchmark collation/validation of a single-node TON network under realistic
mainnet-like conditions: ~150GB basechain state (does not fit in 125GB RAM, saturates
disk cache), block rate 400ms, max_split=0 (single basechain shard), spam = jetton
transfers from tens of millions of distinct wallet-v5 wallets. Measure TPS + latency.

Hardware: /mnt/bench = 4TB Samsung 990 PRO (ext4, noatime). All big artifacts live there.

## Components

1. **`bench-state-gen`** (C++, `benchmark/state-gen.cpp` + `benchmark/common.*`):
   offline generator that synthesizes the basechain zerostate (workchain 0, shard
   8000000000000000, seqno 0) and writes its cells DIRECTLY into a celldb-format
   RocksDB, streaming, with O(tree-depth) RAM. Outputs `manifest.json`.
2. **Validator patch**: `WaitBlockState` probes celldb for `root_hash` before trying
   static file / network for seqno-0 states. Lets the node boot a zerostate whose BoC
   file never existed.
3. **tontester extensions**: `create_zerostate(..., external_basechain=...)` injects the
   generator's root/file hash into config param 12 instead of building basestate0.boc;
   `NetworkConfig.gas_limit_mul`; node runs with `--disable-state-serializer`.
4. **`bench-spam`** (C++, `benchmark/spam.cpp`): pre-signs wallet-v5 externals doing
   jetton transfers, sends via liteserver at a configured rate, tracks inclusion by
   scanning new blocks, reports TPS/latency/block stats (JSON + CSV).
5. **Orchestration**: `benchmark/bench_jetton.py` — builds network on /mnt/bench,
   checkpoints the master celldb into the node dir (RocksDB Checkpoint = hardlinks),
   starts node, runs bench-spam, collects results.

## State composition (defaults, CLI-overridable)

- `N` = 80M wallet-v5 wallets, each with a paired jetton wallet (160M accounts).
- 1 jetton master (minter) account.
- 32M ballast accounts × ~17 chained data cells (~2.2KB each) — inert, mimic mainnet
  cold data; interleave with hot cells in RocksDB blocks to defeat page-cache.
- Total ≈ 150-170GB on disk.

## Deterministic derivation (seed = 32 bytes, hex CLI arg)

- v5 wallet i: ed25519 seed = SHA256(seed || "w5" || u64le(i)). Data cell =
  `1(bit) | seqno=0:u32 | wallet_id=0:u32 | pubkey:256 | 0(bit, empty ext dict)` (322 bits).
  Address = SHA256(StateInit{code=W5, data}); StateInit = `00 1 ref(code) 1 ref(data) 0` (5 bits, 2 refs).
- jetton wallet of owner O: stored data = `coins(JBAL) | addr_std(0,O) | addr_std(0,M) | ref(JW_CODE)`.
  Its ADDRESS is SHA256(StateInit{JW_CODE, data with coins(0)}) — i.e. derived from the
  zero-balance initial data, matching the jetton contracts'
  calculate_user_jetton_wallet_address(); otherwise transfers would target a different
  (uninitialized) account.
- master M: minter data = `coins(total_supply = N*JBAL) | addr_none | ref(empty content) | ref(JW_CODE)`
  (per in-repo jetton-minter.fc layout), addr = SHA256(StateInit). Balance 1 TON.
- ballast i: addr = SHA256(seed || "bl" || u64le(i)); active account, shared trivial code
  cell (bytes "tonbench-ballast"), data = chain of cells, 127 bytes splitmix64 filler each,
  rng state per cell k seeded from (low 8 bytes of addr) ^ (0xB10A57C811 * (k+1))
  (addr already encodes (seed, i)).
- Balances: v5 = 100 TON, jw = 1 TON + 1e15 jetton units, ballast = 1 TON. last_paid = gen_utime.

## Account/state TLB (must byte-match what the validator expects)

- Leaf value: `account_descr$_ account:^Account last_trans_hash:0x00*32 last_trans_lt:0`.
- `account$1 addr:addr_std{wc=0} storage_stat:StorageInfo storage:AccountStorage`;
  StorageInfo = `used:StorageUsed storage_extra:storage_extra_none last_paid:gen_utime due_payment:none`;
  StorageUsed counted over the serialized AccountStorage cell tree (dedup within account),
  identical for all accounts of one shape — compute once per shape.
- `AccountStorage = last_trans_lt:0 balance:CC state:account_active(StateInit)`.
- ShardAccounts = HashmapAugE 256 ShardAccount DepthBalanceInfo; augmentation must
  byte-match block::tlb::aug_ShardAccounts (crypto/block/block.cpp) — self-test against
  vm::AugmentedDictionary for small N is mandatory.
- ShardStateUnsplit header mirrors tontester zerostate.py `base_state` exactly
  (global_id=-777, min_ref_mc_seqno=0xFFFFFFFF, empty OutMsgQueueInfo with extra=None,
  overload/underload=0, total_validator_fees=0, libraries empty, master_ref/custom none),
  with accounts dict + total_balance from generation, gen_utime = generation time.
- Cross-check: with N=0/ballast=0 and the same gen_utime, root hash MUST equal the
  python-built `basestate0.boc` root hash.

## CellDB output format

- Key = 32-byte cell hash; value = `vm::CellStorer::serialize_value(refcnt=1<<30, cell, as_boc=false)`.
  Inflated refcnt makes GC harmless (dec never reaches 0, never traverses).
- Meta entries (see Plan in repo history / celldb.cpp:878-896):
  - `"desczero"` → boxed TL `db.celldb.value{block_id=empty(workchain=0x80000000,...zeros), prev=z, next=z, root_hash=0}`
  - `"desc" + hex_upper(z)` where `z = sha256(boxed tonNode.blockIdExt(B0))`,
    `B0 = {wc=0, shard=0x8000000000000000, seqno=0, root_hash=R, file_hash=F}` →
    `db.celldb.value{block_id=B0, prev=0, next=0, root_hash=R}`.
  Build these with `create_serialize_tl_object<ton_api::db_celldb_value>` +
  `get_tl_object_sha_bits256(create_tl_block_id(B0))` — never hand-roll.
- F (fake file hash) = SHA256("tonbench-fake-file" || R).
- Pipeline: phase 1 parallel address derivation → 256 bucket files by addr[0];
  phase 2 per-bucket sorted streaming subtree build (stand-in cells for finished
  children, e.g. vm::ExtCell or custom hash-carrying Cell subclass; verify hashes match
  all-real-cell construction in tests), emit (hash,value) records to sorted run files;
  phase 3 k-way merge (dedup identical hashes; values must be byte-identical) →
  rocksdb::SstFileWriter chunks → IngestExternalFile into fresh DB → write meta entries.
- `--checkpoint <src> <dst>` subcommand: RocksDB Checkpoint (hardlink copy) so each
  network run gets a disposable celldb.

### Dictionary-layer bundles (`--bundle-depth B`, default 5, 0 = off)

Problem: at 208GB every accounts-dict descent costs ~13 serial ~8KB reads (one per
dict level) although the device reads >=4KB anyway. Fix: group ~B key-bit levels of
the dict into one record. New celldb value variant "bundle"
(`vm::CellStorer::kBundleTag = -2`, exact layout in crypto/vm/db/CellStorage.cpp
`parse_bundle`): under the bundle-root's hash we store the root plus all inlined
descendants as a flat slab (children-before-parents, internal refs by index,
external refs as level_mask+hash+depth exactly like plain-record children). One read
materializes the slab as real DataCells; ext cells are created only at the cut.

- Bundle roots: dict nodes whose edge start bit crosses a `B`-bit window boundary
  (the dict root always is one). Slab = all dict nodes in the same window, plus at
  each dict leaf the ShardAccount -> Account -> data-root chain. Shared cells
  (contract code, empty cell, ballast chain cells past the head) stay external.
- Bundles are ADDITIVE: every cell still has its plain record (loadable by hash);
  only the bundle root's key carries the bundle value instead of a plain one
  (the merge phase prefers the bundle on duplicate hashes).
- GC/refcnt: generator writes refcnt 1<<30 in bundle records too. The validator
  NEVER writes bundles (only the generator does); a block that modifies a dict node
  produces a NEW hash -> a plain record; the stale bundle under the old root hash
  just lingers with its huge refcnt (refcnt merges handle the -2 tag, see
  CellStorer::merge_value_and_refcnt_diff). Cells referenced by new blocks get
  their refcnt bumped on their standalone plain records as before. celldb's
  compress-depth migration explicitly skips bundle records (validator/db/celldb.cpp).
- Aging: as blocks rewrite the hot dict paths, descents increasingly hit per-cell
  plain records for the rewritten top levels and bundles only below the rewrite
  frontier; long-running networks gradually lose the read advantage on hot paths
  (fine for benchmark runs measured in minutes).

## Manifest (manifest.json, written by generator, read by python + bench-spam)

```json
{"version":1, "seed_hex":"…64 hex…", "global_id":-777, "gen_utime":1234567890,
 "root_hash_hex":"…", "file_hash_hex":"…", "total_balance":"8160000000000000000",
 "num_v5":80000000, "num_ballast":32000000, "ballast_cells":17, "wallet_id":0,
 "w5_code_hash_hex":"…", "jw_code_hash_hex":"…", "minter_addr_hex":"…",
 "v5_balance":"100000000000", "jw_balance":"1000000000", "jw_jetton_balance":"1000000000000000",
 "celldb_path":"/mnt/bench/state/celldb"}
```

## Spam transaction shape

External to wallet i (seqno 0, each wallet used once per run):
body = `0x7369676E | wallet_id:32=0 | valid_until:32=0xFFFFFFFE | seqno:32=0 | 1(maybe actions ref) | ref(c5) | 0(no other actions)` + 512-bit sig appended; sig = ed25519 over cell-hash of the signed slice (slice_hash semantics: hash of cell with those bits+ref).
c5 = `cell{0x0ec3c86d | mode:u8=3, refs:[empty_cell, ^MessageRelaxed]}` (mode must include +2).
MessageRelaxed: int_msg to wallet-i's jetton wallet, value 0.05 TON, body =
`op=0xf8a7ea5 transfer | query_id:u64=i | amount:coins | destination=addr_std(0, owner_j) | response_destination=addr_none | custom_payload=0 | forward_ton_amount=0 | forward_payload=0(either inline, empty)`.
Result: 3 txs (wallet, sender jw, recipient jw), 2 internal msgs, no notification/excess.
Recipient j = pseudorandom other wallet. Gas ≈ 20-25k total per transfer.

## Measurement

- bench-spam sends at rate R (token bucket), records (msg_hash, t_send).
- Watches wc0 blocks via liteserver (lookupBlock seqno++ / getBlock), parses InMsgDescr:
  ext msg hash → inclusion latency = t_block_seen - t_send; counts txs per block → TPS.
- Sample (1%) full-chain tracing: follow out-msg hashes through subsequent blocks'
  InMsgDescr to timestamp the recipient-jw tx (transfer completion latency).
- Also scrape validator log collation/validation timings ("collation took") + block gas.
- Expected mainnet-like ceiling ≈ 500-1000 jetton TPS (bytes soft limit 512KB);
  "unleashed" config (block_limit_mul≈40, gas_limit_mul≈10) probes the 1e4 goal.

## bench-spam CLI contract (agreed between orchestrator and tool)

```
bench-spam --manifest <manifest.json> --contracts-dir <benchmark/contracts> \
  --liteserver <ip>:<port> --liteserver-pubkey-b64 <base64 ed25519 pubkey> \
  --rate <ext msgs/s> --duration <s> --warmup <s> --wallet-offset <first wallet idx> \
  --track-sample 0.01 --out <results.json> --blocks-csv <blocks.csv>
```

Each run consumes wallets [offset, offset + rate*duration) — one external per wallet
(seqno 0). Orchestrator advances offset between runs so seqnos stay valid without
re-reading state. results.json schema (approximate, tool may extend):

```json
{"sent":N, "send_errors":N, "included":N, "duration_s":…, "rate_target":…,
 "tps_included":…, "jetton_tps":…,
 "inclusion_latency_ms":{"p50":…,"p90":…,"p99":…,"mean":…},
 "chain_latency_ms":{"p50":…,"p90":…,"p99":…,"samples":N},
 "blocks":[{"seqno":…,"utime":…,"observed_at_unix_ms":…,"n_txs":…}, …]}
```

The orchestrator (`benchmark/bench_jetton.py`) needs from tontester:
`FullNode.liteserver_endpoint()` → (host, port, pubkey_b64).

## Build

New top-level `benchmark/` dir, added via add_subdirectory in root CMakeLists (pattern:
test-ton-collator). Targets: bench-state-gen, bench-spam; link ton_crypto, ton_block,
ton_db, tddb, tl_api, lite-client-common (for spam). Build dir: src/build (Ninja).
Heavy processes run with `choom -n 1000 --`.
