# TON Grafana dashboards

The canonical dashboards live in `dashboards/`:

- `ton-overview.json` — fleet health and incident entry point
- `ton-blockchain.json` — sync, block production, split pressure, and external messages
- `ton-actors.json` — actor scheduler, workload, tails, and performance counters
- `ton-network.json` — ADNL, RLDP2, and QUIC health and traffic

There is deliberately only one copy of each dashboard. Every Prometheus query uses the
standard `instance` target label, and every dashboard starts with a `datasource` variable that
selects a Prometheus datasource by plugin type. Datasource URLs, credentials, and scrape-label
customization belong in the deployment's Prometheus/Grafana provisioning, not in copied JSON.

**One job is one network.** The `job` selector is deliberately single-select with no **All**: every
board reads chain-level series — masterchain head, block rates, collation budget — that are only
meaningful within one network, so a selection spanning two jobs would merge mainnet and testnet into
one chain that does not exist. Compare networks in two browser tabs, not in one panel. The
`instance` selector is the multi-select one, and it filters nodes *inside* the selected job.

**Attribution rows name the range owner.** Wherever a panel collapses nodes it hides a `⇒ node` row
beside each series, and that row names the node that dominated the *visible range* — the maximum
over the whole range, or the minimum where low is the bad direction. That is deliberate: one stable
owner per range rather than a name that flickers from sample to sample. It also means that under
`$agg` = median or p95 the plotted value is that aggregate while the name stays the range-dominant
node; read the name as "who this series is about", not as "who produced this exact point".

**Runs are annotated.** Every board displays the Grafana org annotations tagged `ton-run` as a
**Runs** layer. `benchmark/run_local_net.py` posts one region per run (net directory, size, message
rate, and the branch/commit it was built from) and one point per load chunk, so any panel can be
read against "which run was this". Posting is best-effort: it is configured by `GRAFANA_URL`,
`GRAFANA_AUTH` (`user:pass`) or `GRAFANA_TOKEN`, setting any of them to the empty string disables
it, and the first failure logs one warning and stops the harness from trying again.

Set the Prometheus datasource's minimum interval to the real scrape interval. In provisioning, the
setting belongs under `jsonData`; for a 5-second scrape:

```yaml
jsonData:
  timeInterval: 5s
```

This keeps Grafana's `$__rate_interval` at 20 seconds or more. Declaring a shorter interval can leave
only one or two samples in `rate()` windows, making lines appear or disappear with refresh alignment
and panel width. The fixed one-minute summary panels assume a scrape interval of 15 seconds or less;
widen those fixed windows for slower scrapes.

## Use

### Import manually

In Grafana, open **Dashboards -> New -> Import**, upload a file from `dashboards/`, and import it.
After it opens, choose the Prometheus instance from the **Prometheus** selector at the top. This
works regardless of the datasource's name or UID.

### Provision from files

Point a Grafana dashboard file provider at the `metrics/grafana/dashboards` directory and
provision the Prometheus datasource separately. The same JSON used for manual import is loaded by
the file provider; no datasource-UID substitution or generated dev copy is required.

When more than one Prometheus datasource exists, select the intended one in the dashboard or pass
it in the URL as `?var-datasource=<uid>`. Keep the variable visible so an operator can immediately
see which environment is being queried.

Older working trees used `portable/` plus an ignored `dev/` directory. Update external file
providers from `dev/` to `dashboards/`. The former `ton-quic` and `ton-network-admins` dashboards
are now the single `ton-network` dashboard. A manually imported legacy dashboard with UID
`ton-quic` must be deleted once; the existing `ton-network` UID updates in place.

## ton-network.json — "TON Network"

Works against any Prometheus scraping nodes that run the metrics branch (the `ton_quic_*`,
`ton_adnl_*`, and `ton_rldp2_*` families). The `scope` variable keys off a `ton_scope` target label
(mainnet/testnet/devnet). Without that label the selector has only **All**, and the panels still
match the unlabeled series because **All** expands to `.*`. The same expansion preserves unlabeled
targets during a mixed labeled/unlabeled rollout.

The default view is incident-oriented: outbound query/delivery health and path/packet health stay
expanded. Connection inventory, traffic efficiency, type composition, and noisy per-type failure
drill-downs are collapsed until needed. RLDP2 bulk-transfer latency and outcomes have their own
collapsed row instead of sharing a scale with low-latency ADNL/QUIC. Multi-series charts use at most
two columns so their legends remain readable on a 1280 px viewport.

`Node aggregation` drives every per-node transport panel here — failure and loss ratios, drops by
tier and reason, the two failure drill-downs, handshake attempts and completion, app payload,
datagrams per syscall, wire/app overhead, RLDP2 transfer failures. Each is computed per (node,
scope) and then collapsed, so worst (the default) is the old fixed reading and median or p95 tells
you whether a number is one node's problem or everyone's. Two of them read the other way — handshake
completion and datagrams per syscall, where high is good — so there "worst" is the fleet minimum;
the panels say so. **QUIC RTT across nodes** deliberately carries no `▾`: it already draws p50, p95
and max at once, which is the whole distribution the switch would otherwise pick one point of.

The **QUIC peer class** selector is global to peer-attributable QUIC app traffic and drops, query
roundtrips, message delivery, and ready paths. The one exception is the wire/app overhead ratio: its
app denominator remains all-peers so it matches the unsplit wire numerator. **All** compares trusted
and untrusted series side by side where useful. QUIC socket/pre-auth totals and all ADNL, RLDP2, and
overlay metrics stay all-peers because they do not carry a sound trust label; panel descriptions
call this out where split and unsplit series share a chart. During a mixed-version rollout, QUIC
series from pre-trust builds appear under **All** only; selecting **trusted** or **untrusted** excludes
them.

## ton-overview.json — "TON Overview"

One small health-first dashboard spanning the fleet. The first screen shows worst-node chain lag,
actor scheduler occupancy, live execution age, exporter reachability, and worst-node
kernel receive overflow. Throughput and the busiest-node eligible mempool backlog are a separate context row,
alongside chain-level masterchain blocks/s, shard blocks/s per active shard, and active shard count. Deep
actor throughput is normalized per reporting node, and one card shows reporting masterchain
validators, shardchain validators, and unique validator targets in the selected fleet. Masterchain
and shardchain activity use separate panels. A collapsed quick-attribution row provides incident
pointers; full network, broadcast, and performance attribution stays in the detail dashboards.
Blockchain timing, split-reason, and external-message drill-downs live in `ton-blockchain.json`;
Overview keeps only their incident-entry signals. The single-select `job` picker scopes every query
to one network. `Node aggregation` switches the per-node panels that carry a `▾` — actor handler
load, the two top-traffic charts, and transport failure ratios — between worst, p95, median, and
best across the nodes exposing each series; the stat tiles stay fixed-worst because that is their
whole semantics, and chain-scope panels have no node to pick. The `instance` selector scopes
node-local views and reachability; chain-wide
activity, production, and collation signals deliberately ignore it so rotating collators remain
complete. Its choices come from Prometheus's `up` inventory so down targets remain visible. Actor
panels need builds that expose the `ton_actor_*` metric families. Set **Slot (s)** from the selected
network's chain configuration; it supplies target context and the denominator for collation-budget
ratios, but never changes the measured block-rate series.

Triage thresholds for CPU-worker occupancy, IO-worker occupancy, and kernel RX overflow use a
fixed five-minute rate window. Zooming the dashboard therefore cannot change whether the same raw
traffic or worker load crosses a health threshold; exploratory detail panels continue to use
Grafana's scrape-safe `$__rate_interval`.

## ton-blockchain.json — "TON Blockchain"

The blockchain drill-down opens with one at-a-glance row instead of the full metric inventory.
Its collation summary, validation summary and slot timeline are **chain-paired**: masterchain on the
left, shardchain on the right, each pinned to its chain and deliberately ignoring the **Chain**
selector, because the first screen has to answer "is either chain unhealthy" without a toggle. The
selector still drives the two phase stacks in that row and every collapsed diagnostic row below it.
The compact tables put successful collation and validation p50, p95, exact recent maximum and that
maximum's owning node side by side for end-to-end, timed-wall and CPU clocks. **Collate time stats**
uses the same zero-centred grammar as the devnet log dashboard: additive outer work phases stack
above zero, while explicit external waiting and otherwise unattributed waiting stack below it.
Nested transaction/preliminary-storage timers remain outside that stack so they cannot be counted
twice. A matching CPU stack makes computation visible separately, and the slot timelines show five
median milestones, listed in the order they happen, with zero fixed at scheduled slot start. A
**Session logs (devnet)** link in the top bar opens the log-based per-run explorer for the current
time range. The collapsed diagnostics are
organized by the question being investigated: chain health history, block-flow outcomes,
collation, validation, block workload/storage, consensus/finality, propagation, external-message
flow, and mempool health. The overview row owns current chain health, so duplicate instant
chain-state cards are deliberately absent here.

A single-value **Chain** selector controls the detailed activity, production, and
external-execution panels, so masterchain and shardchain never share a graph. Masterchain rate is
derived from advance of the highest applied head observed across
the selected jobs. During a rolling upgrade that head is used only when every applied-block
reporter exports it; otherwise the dashboard falls back to the older reporter-median stream.
Shardchain has no global seqno, so its rate uses the median only to reconcile
duplicate canonical applied-block streams from healthy nodes. Both rates intentionally ignore the
instance selector and validator count. Collation is summed across rotating collators, and validation
rates are divided by active validators. Production panels expose successful-event p50/p95 over fixed
five-minute windows for elapsed, real and CPU time, exact retained total and phase maxima split
between success and failed/retry attempts with node attribution, per-node CPU s/s, an additive
outer collation work decomposition, nested transaction/storage work,
`want_split`, overload reasons, block work, and size. The external section separates global
admission from selected-chain execution and shows rejection and local-removal reasons,
eligible backlog, total storage, expiry-handler lag, oldest-entry age, and a five-minute
stock/flow reconciliation. Its stages are correlated signals, not a conservation funnel. Mempool
stock is node-local across all destination chains and priorities: it is never summed across the
fleet, and the **Chain** selector does not filter it. The primary backlog is stored `active=true`
entries; it does not claim every entry is selectable by the current collator. Total storage adds
postponed `active=false` entries, which can remain postponed after their retry deadline until a
collator snapshot revisits and reactivates them. Stateful exporters compare the five-minute delta
of total storage with the increase in new `accepted` insertions minus the increase in every removal
reason, keeping `reprioritized` correctly net-zero. The underlying identity is exact from process
start on every upgraded process—informative on storing nodes and trivially zero on relay-only
nodes—although the plotted range-function estimates can differ transiently at scrape, restart, or
rollout boundaries. Legacy `accepted` also counted successful validation on non-storing nodes;
legacy unlabeled stock therefore
remains visible as both the eligible fallback and total during rollout/history, but split-only
reconciliation stays absent rather than presenting invented values. Expiry removes entries atomically
at the 600-second TTL; `max(oldest age - 600s, 0)` is shown as expiry-handler lag and should stay zero.
Oldest-age and expiry-lag signals omit legacy exporters because their periodic expiry semantics are
not comparable and could otherwise dominate the worst-node view during a rolling upgrade.
Applied-external rates are medians of reporters whose absolute masterchain age is under 120 seconds;
shardchain observations additionally require shard-client lag of at most two blocks. This rejects
normal catch-up and future-skewed chain clocks, but the underlying counter still includes replay and
is not protocol-wide chain truth. The chain-health history row uses normal time series with a
visible Y-axis and Last/Min/Max values for diagnosis. Replicated age and lag histories combine
worst, p95, median, average, and best (minimum) because their node spread is diagnostic. Block-rate
histories instead show one chain-level series; selecting an individual reporter cannot redefine
the blockchain. The discarded-candidate estimate is fleet-wide within the selected jobs and
intentionally ignores the instance selector because collators rotate. It renders no data during a
partial metrics rollout rather than combining incompatible reporter populations. A curated block/state/storage
perf panel keeps the relevant consensus-signature, cell, CellDB, state-application, serialization,
transaction-storage, RocksDB commit, and file-sync counters visible without top-k truncation. **Slot (s)** is an explicit
deployment input used by target guides and budget ratios; block rate itself always comes from chain
state and is never normalized by that input.

The consensus/finality row intentionally has only four views. **Slot outcomes** shows terminal
cadence against the configured target and accepted/empty/skipped shares, normalized per validator
group so replica and shard counts cannot inflate it. **Finality position** shows p95 and the exact
recent latest finality-certificate and local-apply positions as a percentage of the slot, where
100% is slot end, and names the owner of the retained extreme. **Consensus stage stats** gives one
row per stage with p50, p95, the slowest node's mean and sample count, plus an exact recent maximum
and owner. **Slot handoff stats** gives the same compact median/p95/extreme grammar for dead time and
pipeline head start. Distribution columns use a fixed five-minute window; exact maxima are retained
for roughly 10–20 minutes.

Consensus stages are attribution signals, not an additive partition of a round. Collation and
publish are collator-only observations, later stages come from participating validators, and
certificate stages include signing, quorum wait, local persistence, and possible fsync. Full-block
handoff completion includes local apply/storage/fsync; empty and skipped slots complete at their
certificate. Consensus distributions pool the selected jobs rather than obeying the Instance
selector, while exact extremes preserve the responsible reporter for attribution.

The same chain-scope/per-node pairing runs through block production: successful-event quantiles and
exact recent maxima split by outcome show typical, p95, worst-success and worst-failure cases;
real/CPU phase panels explain the cause; and the
two `▾` "phases by node" panels identify which collator or validator is slow in which phase.
Collation is sparse by construction (one node holds each slot);
validation is dense (every validator validates every block), so the spread between worst and median
means more there.

The block-propagation row compares each semantic source with the applied-block rate. First-arrival
share identifies the source that usually wins the same-block race; source coverage shows which
other paths arrive for those blocks. Both ratios are calculated per node and then averaged, so the
result is stable when the validator count changes. They use Grafana's scrape-safe recent rate
window (normally about 20 seconds at a 5-second scrape and one minute at a 15-second scrape). They do not distinguish Plumtree from two-step,
FEC from simple broadcasts, or QUIC from ADNL: that method identity is not present in these metric
families. Raw overlay broadcast count and byte rates by TL type remain in `ton-network.json`.

## ton-actors.json — "TON Actor Framework"

Needs nodes running a build with the `ton_actor_*` exporter tier. On older builds the actor data
panels are empty, but **Nodes without actor stats** remains populated from target/exporter inventory
and counts reachable nodes whose actor tier is missing or disabled. The `job` and `instance`
selectors scope every actor query, and the instance choices follow the selected job roles.

The top cards are worst-target health signals rather than fleet totals: normalized CPU/IO worker
occupancy, visible local run queue, live execution age, recent queue wait/drain maxima, peak node
message rate, and missing/disabled-stat count. The `Node aggregation` selector switches actor-class and
perf-counter panels between worst, p95, median, and best across nodes exposing each series. Health
and tail panels keep their fixed worst-node semantics; low-action inventory cards are collapsed by
default, while performance counters stay expanded. The full-width perf panels show up to 50
registered operations by default; the `Perf series` selector can reduce the view when a narrower
comparison is useful. The 30-minute default range normally shows a complete two-bucket retained
maximum and its expiry cliff; exact wall duration is platform-dependent because the hot path uses
TSC buckets rather than a wall-clock call.

Panel semantics live in each panel's description (the (i) icon), sourced from
`metrics/METRICS.md`.

## dashgen — the dashboards are generated

The four dashboard JSONs are build artifacts. The source of truth is
`metrics/grafana/dashgen/`: one module per board (`ton_overview.py`, `ton_blockchain.py`,
`ton_network.py`, `ton_actors.py`) describes each dashboard as rows of archetype calls
(`worst_node_stat`, `fleet_timeseries`, `agg_timeseries`, `chain_timeseries`, ...) and is the only
file meant to be read while editing a dashboard. `lib/` is the vocabulary those calls are written
in: `lib/conventions.py` implements each house pattern exactly once (named worst-node tiles with
drill links, the `\u25be`/`$agg` node-aggregation switch, the hidden attribution rows that name the
node behind a collapsed series, threshold bands, the nav links bar) and `lib/core.py` holds the
Grafana JSON primitives. A convention is impossible to forget because the vocabulary emits it;
a panel cannot ship without a description because `panel()` rejects an empty one; a series that
collapses the fleet carries its `class \u21d2 node` attribution row because `fleet_timeseries` adds it.

```sh
python3 -m metrics.grafana.dashgen.build build   # regenerate metrics/grafana/dashboards/*.json
python3 -m metrics.grafana.dashgen.build check   # verify committed JSON == regenerated; exit 1 on drift
python3 -m metrics.grafana.dashgen.build validate # build and run semantic dashboard checks
```

Workflow: edit the board (or the convention), run `build`, review the JSON diff, commit both.
Never edit the JSON by hand — `check` is the drift gate that catches it (run it in review or
wire it into CI; nothing runs it automatically today). When a pattern needs to
appear on one more panel, it is one more archetype call in the board; when a pattern changes,
it changes in `lib/conventions.py` for every panel at once.

## Validation

Generation validates panel IDs, target refIds, legend label syntax, variables, grid bounds and
overlaps, and explicit job-scoped query contracts. It also rejects a chain-truth panel that reads
the Instance variable. Run both semantic validation and generated-file drift checks after editing:

```sh
python3 -m metrics.grafana.dashgen.build validate
python3 -m metrics.grafana.dashgen.build check
promtool check rules metrics/prometheus/rules/ton-recording.yml
```

The Overview's Active triage conditions table evaluates its condition registry inline from base
metrics. It is deliberately independent of Prometheus alert rules; paging and routing policy belong
to each deployment.
