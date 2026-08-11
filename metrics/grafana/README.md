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
actor scheduler occupancy, live execution age, exporter reachability/freshness, and worst-node
kernel receive overflow. Throughput and the busiest-node mempool backlog are a separate context row,
alongside chain-level masterchain blocks/s, shard blocks/s per active shard, and active shard count. Deep
actor throughput is normalized per reporting node, and one card shows reporting masterchain
validators, shardchain validators, and unique validator targets in the selected fleet. Masterchain
and shardchain activity use separate panels. A collapsed quick-attribution row provides incident
pointers; full network, broadcast, and performance attribution stays in the detail dashboards.
Blockchain timing, split-reason, and external-message drill-downs live in `ton-blockchain.json`;
Overview keeps only their incident-entry signals. The durable TON job-regex textbox (default `ton`)
scopes every query. The `instance` selector scopes node-local views and reachability; chain-wide
activity, production, and collation signals deliberately ignore it so rotating collators remain
complete. Its choices come from Prometheus's `up` inventory so down targets remain visible. Actor
panels need builds that expose the `ton_actor_*` metric families. Set **Slot (s)** from the selected
network's chain configuration; it supplies target context and the denominator for collation-budget
ratios, but never changes the measured block-rate series.

## ton-blockchain.json — "TON Blockchain"

The blockchain drill-down starts with separate masterchain and shardchain rates, chain age/lag,
shard collation budget, and discarded-candidate estimate. A single-value **Chain** selector controls
the detailed activity, production, and external-execution panels, so masterchain and shardchain never
share a graph. Masterchain rate is derived from advance of the highest applied head observed across
the selected jobs. During a rolling upgrade that head is used only when every applied-block
reporter exports it; otherwise the dashboard falls back to the older reporter-median stream.
Shardchain has no global seqno, so its rate uses the median only to reconcile
duplicate canonical applied-block streams from healthy nodes. Both rates intentionally ignore the
instance selector and validator count. Collation is summed across rotating collators, and validation
rates are divided by active validators. Production panels expose time per attempt, an
additive outer collation pipeline with elapsed/real totals, nested transaction/storage work,
`want_split`, overload reasons, block work, and size. The external section separates global
admission from selected-chain execution and shows rejection and local-removal reasons,
backlog, and oldest-entry age. Its stages are correlated signals, not a conservation funnel.
Applied-external rates are medians of reporters whose absolute masterchain age is under 120 seconds;
shardchain observations additionally require shard-client lag of at most two blocks. This rejects
normal catch-up and future-skewed chain clocks, but the underlying counter still includes replay and
is not protocol-wide chain truth. Stat
cards are compact, instant summaries: gauges show the latest scrape, current flow rates use a fixed
one-minute window, and deliberately smoothed ratios carry their window in the title (for example,
10m). The six chain-state cards are repeated immediately below as normal time series with a visible
Y-axis and Last/Min/Max values for diagnosis. Replicated age and lag histories combine worst, p95,
average, and minimum because their node spread is diagnostic. Block-rate histories instead show one
chain-level series; selecting an individual reporter cannot redefine the blockchain. The
discarded-candidate estimate is fleet-wide within the selected jobs and
intentionally ignores the instance selector because collators rotate. It renders no data during a
partial metrics rollout rather than combining incompatible reporter populations. A curated block/state/storage
perf panel keeps the relevant consensus-signature, cell, CellDB, state-application, serialization,
transaction-storage, RocksDB commit, and file-sync counters visible without top-k truncation. **Slot (s)** is an explicit
deployment input used by target guides and budget ratios; block rate itself always comes from chain
state and is never normalized by that input.

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

## Static validation

Run this after editing a dashboard:

```sh
jq -e . metrics/grafana/dashboards/*.json >/dev/null

for dashboard in metrics/grafana/dashboards/*.json; do
  jq -e '
    (.templating.list[0].name == "datasource") and
    (.templating.list[0].type == "datasource") and
    (.templating.list[0].query == "prometheus") and
    (.templating.list[0].current == {}) and
    ([.. | objects | .datasource?
      | select(type == "object" and .type == "prometheus")
      | .uid] as $uids
      | ($uids | length) > 0
      and ($uids | all(. == "${datasource}")))
  ' "$dashboard" >/dev/null
done
```

The check deliberately rejects hard-coded Prometheus UIDs in panels and query variables.
