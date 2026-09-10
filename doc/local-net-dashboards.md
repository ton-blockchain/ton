# Local validator net with Grafana dashboards

How to run a local multi-validator net (idle or under jetton-transfer load) and watch it on
the dashboards from `metrics/grafana/dashboards/`.

## 1. Build

```bash
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=OFF -DTON_USE_JEMALLOC=ON -GNinja
ninja -C cmake-build-relwithdebinfo validator-engine dht-server generate-random-id \
      validator-engine-console create-state tonlibjson bench-state-gen bench-spam
```

Debug builds work too but are ~7x slower in collation; block-timing numbers will not be
representative.

## 2. Generate the bench state (once)

Deterministic basechain state with N wallet-v5 accounts (each with a paired jetton wallet)
plus ballast accounts. 500k wallets ≈ 4M cells, a few minutes to generate:

```bash
cmake-build-relwithdebinfo/benchmark/bench-state-gen gen \
  --seed-hex 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff \
  --v5-count 500000 --ballast-count 10000 --ballast-cells 4 \
  --contracts-dir benchmark/contracts \
  --out-dir "$HOME/ton-localnet/state"
```

The output dir holds `celldb/` + `manifest.json`; every validator gets a hardlink checkpoint
of it at net boot, so one state serves any number of runs. See `benchmark/DESIGN.md`.

Do NOT keep the state (or anything below) under `/tmp` — macOS reaps old files there and the
stack dies in confusing ways days later.

## 3. Run the net

```bash
# idle, 8 validators on both masterchain and the shard group, mainnet-like consensus:
uv run python benchmark/run_local_net.py \
  --state "$HOME/ton-localnet/state/manifest.json" \
  --net-dir "$HOME/ton-localnet/net" \
  --validators 8 --shard-validators 8 --rate 0

# same topology under sustained 40 external tx/s:
uv run python benchmark/run_local_net.py \
  --state "$HOME/ton-localnet/state/manifest.json" \
  --net-dir "$HOME/ton-localnet/net" \
  --validators 8 --shard-validators 8 --rate 40

# smallest delegated-collation topology (no generated state, idle):
uv run python benchmark/run_local_net.py \
  --net-dir "$HOME/ton-localnet/one-validator-net" \
  --validators 1 --collators 1 --rate 0

# Spam runs in 600s chunks; every successful chunk spends fresh wallets.
# mainnet-parity consensus is the default: MC protocol v0, shard v1,
# target 400ms, min block interval 300ms. Override with
#   --mc-protocol / --shard-protocol / --target-ms / --min-block-interval-ms
```

Notes:
- `--shard-validators` defaults to `min(8, --validators)`, so `--validators 1` is a complete
  one-validator configuration without another flag. Up to eight dedicated collators can be
  added with `--collators`; validators occupy exporter ports first, then collators.
- The Python process is the supervisor. `Ctrl-C`, `SIGTERM`, and `SIGHUP` cancel an active
  checkpoint/spam process and stop every node (bounded `SIGTERM`, then `SIGKILL`). `SIGKILL`
  cannot run cleanup, but its stale PID lock is recovered on the next invocation.
- `--net-dir` is deliberately strict because its contents are erased at startup. The first run
  must use a nonexistent path. The harness creates `.ton-local-net.json`; later runs accept only
  that marked directory and refuse a live `.ton-local-net.pid` lock. It also rejects paths that
  overlap the repository, build tree, or bench state. For an old unmarked net directory, move or
  remove it manually once, then let the harness create a fresh path.
- Run one net at a time per port range. `--exporter-base` defaults to 9101 and consumes one port
  per validator and collator. `--port-base` is the allocation floor; the first node port is
  `--port-base + 1`.
- A failed checkpoint or message-load chunk terminates the run after cleaning up nodes. Wallet
  offsets advance only after a successful chunk, so failed load is never reported as completed.
- Chain counters restart from zero on every net restart; rate panels show a 2-4 minute hole
  around a restart. That is honest, not a dashboard bug.
- Machine sizing: a 16-core machine handles 8 RelWithDebInfo validators; 8 *debug*
  validators under load oversubscribe it and all timing numbers become scheduling noise.

## 4. Prometheus

`prometheus.yml` (scrape interval 5s; `node` is relabeled into `instance` so dashboard
legends show `validator-N` instead of `host:port`):

```yaml
global:
  scrape_interval: 5s
  scrape_timeout: 4s
scrape_configs:
  - job_name: ton            # the dashboards' single-select `job` picker: one job is one network
    relabel_configs:
      - source_labels: [node]
        target_label: instance
    static_configs:
      - { targets: ["127.0.0.1:9101"], labels: { node: validator-0 } }
      - { targets: ["127.0.0.1:9102"], labels: { node: validator-1 } }
      - { targets: ["127.0.0.1:9103"], labels: { node: validator-2 } }
      - { targets: ["127.0.0.1:9104"], labels: { node: validator-3 } }
      - { targets: ["127.0.0.1:9105"], labels: { node: validator-4 } }
      - { targets: ["127.0.0.1:9106"], labels: { node: validator-5 } }
      - { targets: ["127.0.0.1:9107"], labels: { node: validator-6 } }
      - { targets: ["127.0.0.1:9108"], labels: { node: validator-7 } }
      # With --validators 8 --collators 2, dedicated collators follow validators:
      - { targets: ["127.0.0.1:9109"], labels: { node: collator-0 } }
      - { targets: ["127.0.0.1:9110"], labels: { node: collator-1 } }
```

```bash
prometheus --config.file="$HOME/ton-localnet/prometheus.yml" \
           --storage.tsdb.path="$HOME/ton-localnet/prom-data"
```

For the one-validator/one-collator command above, use only `9101` as `validator-0` and `9102`
as `collator-0`. Extra targets beyond the running validator + collator count show as down
("Targets down" stat), so remove the optional collator lines when no collators are running.

## 5. Grafana

Provisioning (`$HOME/ton-localnet/grafana/provisioning/`):

`datasources/datasources.yaml` — **`timeInterval` MUST equal the real scrape interval.**
If it is smaller, `$__rate_interval` windows can miss samples and every rate panel shows
pseudo-random gaps that move on each refresh (this cost us an evening):

```yaml
apiVersion: 1
datasources:
  - name: Prometheus (TON)
    uid: prom-ton
    type: prometheus
    access: proxy
    url: http://127.0.0.1:9090
    isDefault: true
    jsonData:
      timeInterval: 5s
```

`dashboards/dashboards.yaml` — serve the dashboards straight from the repo; edits to the
JSONs are picked up within ~10s, no restart needed:

```yaml
apiVersion: 1
providers:
  - name: ton-dashboards
    type: file
    updateIntervalSeconds: 10
    allowUiUpdates: false
    options:
      path: /ABSOLUTE/PATH/TO/REPO/metrics/grafana/dashboards
```

Launch (anonymous admin, local only):

```bash
GF_PATHS_PROVISIONING="$HOME/ton-localnet/grafana/provisioning" \
GF_PATHS_DATA="$HOME/ton-localnet/grafana/data" \
GF_SERVER_HTTP_ADDR=127.0.0.1 GF_SERVER_HTTP_PORT=3000 \
GF_AUTH_ANONYMOUS_ENABLED=true GF_AUTH_ANONYMOUS_ORG_ROLE=Admin \
GF_ANALYTICS_REPORTING_ENABLED=false GF_NEWS_NEWS_FEED_ENABLED=false \
grafana server --homepath /opt/homebrew/opt/grafana/share/grafana
```

Open http://127.0.0.1:3000 — dashboards: **TON Overview** (is anything wrong), **TON
Blockchain** (why: block budget, phases, externals funnel), **TON Actor Framework**, **TON
Network**. A restarted Grafana severs open tabs: reload them once.

## Reading the dashboards, quick contract

- Titles carry their scope: "(chain)", "(per node)", "(worst node)"; a "▾" means the panel
  follows the "Node aggregation" switch, no "▾" means the aggregation is fixed by design.
- TON Blockchain → "Collation time per block vs slot": the solid "any collator (chain)"
  line ignores the instance filter — if it stops while Block flow shows blocks, something is
  actually wrong; per-validator dots show rotation and respect the filter.
- "Collation event tails (5m)" compares elapsed, real and CPU median/p95 without stacking.
  The exact recent-max panels keep a single pathological event visible for roughly 10–20 minutes
  and name its node; phase outliers say where it went.
- "Collator-view timeline" uses the target slot start as real zero. Negative marks are pre-roll;
  positive marks are late. It is only the rotating collator's local path for winning full-block
  candidates. The p95 before/after lines are separate zero-padded sides and must not be added.
- CPU load panels are CPU seconds per wall second (1 = one continuously busy core). On macOS,
  elapsed time rising without CPU commonly points to filesystem sync rather than computation;
  correlate with the `fd_sync`/RocksDB perf lines.
- Budget/vs-slot panels exclude `wait_externals` (deliberate idle waiting for messages); on
  a healthy fast net expect a low budget %, not ~100%.
- Mempool evictions are batch events (TTL sweep every 250s) — the panel averages over 5m
  on purpose; a sustained nonzero `expired` series means input exceeds chain capacity for
  longer than the 600s TTL, i.e. the network is losing messages.
