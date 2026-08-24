"""TON Overview: is the fleet healthy, and which board explains the answer."""

from .lib import (
    JOB,
    JOB_SEL,
    LIST_LEGEND,
    NODE,
    SEL,
    SLOT,
    agg_line,
    agg_timeseries,
    agg_variable,
    attributed,
    breach_timeline,
    chain_timeseries,
    column,
    condition,
    dashboard,
    failure_bands,
    fleet_timeseries,
    line,
    mempool,
    node_severity,
    node_severity_timeline,
    of_target_bands,
    plain_stat,
    rate,
    ratio,
    row,
    same_reporters,
    scorecard,
    sel,
    series_style,
    slot_variable,
    standard_variables,
    summed,
    table_legend,
    thresholds,
    top_keys,
    top_total,
    triage_table,
    worker_occupancy,
    worst_node_stat,
)

ACTORS, BLOCKCHAIN, NETWORK = "ton-actors", "ton-blockchain", "ton-network"
RATE = "$__rate_interval"
TRIAGE_RATE = "5m"


def both_transports(metric, order, *, over=RATE, **labels):
    """One per-node series covering adnl and quic, each side tagged with the transport it came
    from. `order` is per panel: the two families were written in whichever order came to hand."""
    def tagged(transport):
        counter = f"ton_{transport}_{metric}_total{sel(**NODE, **labels)}"
        return f'label_replace(rate({counter}[{over}]), "transport", "{transport}", "", "")'

    return summed("job, instance", " or ".join(tagged(t) for t in order))


def masterchain_rate(over):
    """Masterchain blocks/s: the seqno delta when every node reports arrivals, else their median."""
    seqno = f"ton_masterchain_seqno{JOB_SEL}"
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='-1')}"
    delta = f"clamp_min((max({seqno}) - max({seqno} offset 1m)) / 60, 0)"
    everyone_reports = same_reporters(seqno, arrivals)
    median = f"quantile(0.5, sum by (job, instance) (rate({arrivals}[{over}])))"
    return f"({delta} and on () ({everyone_reports})) or {median}"


def mc_age_floor():
    """The freshest node's masterchain age per job: the chain-versus-node stall discriminator.

    Job-scoped, never narrowed by $instance — a chain stall is a property of the whole job, so the
    floor climbing means the fleet is behind together and no single node is to blame.
    """
    return (f"min by (job) (clamp_min(ton_masterchain_block_age_seconds"
            f"{JOB_SEL}, 0))")


def shard_rate(over):
    """Shardchain blocks/s as the median node's first-arrival rate."""
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='0')}"
    return f"quantile(0.5, sum by (job, instance) (rate({arrivals}[{over}])))"


def shard_collations(over):
    """Shard blocks this fleet collated successfully - the denominator of the shard-rate ratios."""
    return summed(None, f"rate(ton_collated_blocks_total"
                        f"{sel(**JOB, chain='shard', result='ok')}[{over}])")


def collation_budget(over):
    """Collation seconds per produced block, minus waiting, as a share of the configured slot."""
    def phase(name):
        spent = sel(**JOB, operation="collate", chain="shard", phase=name,
                    clock="elapsed", result="ok")
        return summed(None, f"rate(ton_block_processing_seconds_total{spent}[{over}])")

    return (f"({phase('total')} - {phase('wait_externals')})"
            f" / ({shard_collations(over)} > 0) / {SLOT}")


def discarded_share(over):
    """Candidates collated but never applied - trustworthy only while every node reports both."""
    counted = f"ton_collated_blocks_total{sel(**JOB, chain='shard', result='ok')}"
    arrivals = f"ton_first_received_total{sel(**JOB, workchain='0')}"
    both_report = same_reporters(counted, arrivals)
    return (f"clamp_min(1 - {shard_rate(over)} / {shard_collations(over)}, 0)"
            f" and on () ({shard_collations(over)} > 0) and on () ({both_report})")


def error_share(metric, over):
    """Failed attempts over all attempts for one block-production stage."""
    attempts = f"ton_{metric}_blocks_total{sel(**JOB, chain='shard')}"
    errors = f"ton_{metric}_blocks_total{sel(**JOB, chain='shard', result='error')}"
    return (f"sum(rate({errors}[{over}]))"
            f" / (sum(rate({attempts}[{over}])) > 0)")


def failure_ratio(name, failed, attempted, **labels):
    """One node's failures over attempts; healthy zeroes stay in fleet percentiles."""
    by = "job, instance"
    attempts = rate(attempted, by, **labels)
    failures = rate(failed, by, **labels)
    guarded = ratio(f"({failures}) or 0 * ({attempts})", attempts)
    return agg_line(guarded, name=name)


def synced_applied(over):
    """Applied externals per second, counting only nodes whose view of the chain is current."""
    fresh = f"abs(ton_masterchain_block_age_seconds{JOB_SEL}) < 120"
    caught_up = (f"clamp_min(ton_masterchain_seqno{JOB_SEL}"
                 f" - ton_shardclient_seqno{JOB_SEL}, 0) <= 2")
    applied = f"rate(ton_applied_ext_messages_total{sel(**JOB, chain='shard')}[{over}])"
    return (f"quantile(0.5, {applied} and on (job, instance) ({fresh})"
            f" and on (job, instance) ({caught_up}))")


def chain_activity(title, chain, block_rate, *, id, description):
    """Blocks the chain applied, plus what this fleet collated and validated for it."""
    def outcome(verb, metric, extra=""):
        return line(f"{verb} {{{{result}}}}{extra}",
                    summed("result", f"rate(ton_{metric}_blocks_total"
                                     f"{sel(**JOB, chain=chain)}[{RATE}])"))

    validators = summed("job, instance", f"ton_validator_groups{sel(**JOB, chain=chain)} > 0",
                        agg="max")
    per_validator = f" / scalar(clamp_min(count({validators}), 1))"
    validated = outcome("validated", "validated", " / validator")
    return chain_timeseries(
        title,
        line("chain block rate", block_rate),
        outcome("collated", "collated"),
        line(validated.name, validated.expr + per_validator),
        scope="every reporter in the selected job", unit="ops", id=id, w=6, h=9,
        query_scope="job",
        styles=[series_style("chain block rate", color="green"),
                series_style("collated ok", color="blue"),
                series_style("validated ok / validator", color="purple"),
                series_style("collated error", color="red"),
                series_style("validated error / validator", color="orange")],
        description=description,
    )


def validator_count(**chain):
    """Nodes holding at least one validator group; without a chain, of any kind."""
    groups = f"ton_validator_groups{sel(**NODE, **chain)}"
    return f"count(max by (job, instance) ({groups}{' > 0' if chain else ''})) or vector(0)"


def top_traffic(direction):
    """The busiest payload types per transport, ranked by total volume over the visible range."""
    def per_transport(transport):
        metric = f"ton_{transport}_app_messages_total"
        scope = sel(**NODE, direction=direction)
        total = summed("tl", summed("job, instance, tl",
                                    f"increase({metric}{scope}[$__range] @ end())"), agg="max")
        return agg_line(summed("job, instance, tl", f"rate({metric}{scope}[{RATE}])"),
                        key="tl", name=f"{transport} {{{{tl}}}}",
                        top=top_keys("tl", total, n=5))

    return [per_transport("adnl"), per_transport("quic")]


# Triage conditions registry — one inline red/warn bool per condition over base ton_* metrics, the
# single source of truth every triage panel derives from (severity, breach, scorecard sev column,
# node timeline, triage table). Node conditions (keyed (job, instance)) feed severity; chain ones
# (keyed (job)/fleet) feed breach and the triage table only. A red cell and red breach band derive
# from the same expression. max by (job,
# instance) normalises every node bool so a stray target label cannot leak into the severity sum.
UP = f"max by (job, instance) (up{SEL})"
MC_AGE = f"max by (job, instance) (clamp_min(ton_masterchain_block_age_seconds{SEL}, 0))"
SHARD_LAG = (f"max by (job, instance) (clamp_min(ton_masterchain_seqno{SEL}"
             f" - ton_shardclient_seqno{SEL}, 0))")
WEDGE = f"max by (job, instance) (ton_actor_scheduler_current_execute_seconds{SEL})"
# Thresholds must not change when an operator zooms the dashboard: these expressions feed the
# triage registry, its debounce history, and the scorecard. Exploratory panels below still use the
# scrape-safe Grafana rate interval.
RX_RATE = both_transports("wire_dropped", ("quic", "adnl"), over=TRIAGE_RATE,
                          direction="in", reason="limited")
CPU_OCC = worker_occupancy("cpu", over=TRIAGE_RATE)
IO_OCC = worker_occupancy("io", over=TRIAGE_RATE)

# Red when the head is old, or when a node that was reporting the gauge stopped while its scrapes
# still succeed: the second arm catches a wedged or resyncing validator subsystem whose gauge
# simply vanishes. It is gated on having reported within the hour ("never reported" is a
# different animal — a liteserver, a node_exporter, Prometheus itself) AND on the target being
# up, because a down target loses every series at once and that is already the unreachable
# condition — one fault, one flag.
MC_REPORTED = (f"count by (job, instance) (max_over_time("
               f"ton_masterchain_block_age_seconds{SEL}[1h])) > bool 0")
MC_STALE_RED = (f"({MC_AGE} > bool 120) or ((({MC_REPORTED})"
                f" unless max by (job, instance) (ton_masterchain_block_age_seconds{SEL}))"
                f" and on (job, instance) ({UP} == 1))")

TRIAGE_CONDITIONS = [
    condition("unreachable", "unreachable", severity="critical", scope="node",
              red=f"{UP} == bool 0", for_s=120),
    # There is no exporter-staleness condition: collection happens at scrape time, so a wedged or
    # starved gather is a scrape that never answers — up goes to 0 and `unreachable` above owns it,
    # already flap-aware through its debounce.
    condition("mc_stale", "mc stale", severity="warning", scope="node",
              warn=f"{MC_AGE} > bool 30", red=MC_STALE_RED,
              for_s=120),
    condition("shard_lag", "shard lag", severity="warning", scope="node",
              warn=f"{SHARD_LAG} > bool 5", red=f"{SHARD_LAG} > bool 20",
              for_s=300),
    condition("cpu_saturated", "cpu saturated", severity="warning", scope="node",
              warn=f"({CPU_OCC}) > bool 0.8", red=f"({CPU_OCC}) > bool 0.95",
              for_s=600),
    condition("io_saturated", "io saturated", severity="warning", scope="node",
              warn=f"({IO_OCC}) > bool 0.8", red=f"({IO_OCC}) > bool 0.95",
              for_s=600),
    condition("wedged_worker", "wedged worker", severity="critical", scope="node",
              warn=f"{WEDGE} > bool 5", red=f"{WEDGE} > bool 30",
              for_s=60),
    condition("rx_overflow", "rx overflow", severity="warning", scope="node",
              warn=f"({RX_RATE}) > bool 10", red=f"({RX_RATE}) > bool 1000",
              for_s=300),
    condition("mc_rate_low", "mc rate low", severity="critical", scope="chain",
              warn=f"({masterchain_rate('1m')}) < bool (0.8 / {SLOT})",
              red=f"({masterchain_rate('1m')}) < bool (0.5 / {SLOT})",
              for_s=120),
    condition("chain_stalled", "chain stalled", severity="critical", scope="chain",
              red=f"({mc_age_floor()}) > bool 120",
              for_s=120),
    condition("shard_discard", "shard discard", severity="warning", scope="chain",
              red=f"({discarded_share('10m')}) > bool 0.3",
              for_s=600),
    condition("collation_slow", "collation slow", severity="warning", scope="chain",
              red=f"({collation_budget('1m')}) > bool 1",
              for_s=300),
]

# The inline composite the severity tiles, scorecard sev column and node timeline all read.
NODE_SEVERITY = node_severity(TRIAGE_CONDITIONS)

# The scorecard columns: inline severity leads (hidden, sorts worst-first), then one health
# dimension per column. Occupancy and rx overflow are inline (worker_occupancy, both transports);
# mc age, shard lag and wedge are the raw gauges their conditions threshold.
TRIAGE_COLUMNS = [
    column("sev", NODE_SEVERITY, thresholds=thresholds(("yellow", 1), ("red", 2))),
    column("mc age", MC_AGE,
           unit="s", thresholds=thresholds(("yellow", 30), ("red", 120)), drill=BLOCKCHAIN),
    column("shard lag", SHARD_LAG,
           unit="short", thresholds=thresholds(("yellow", 5), ("red", 20)), drill=BLOCKCHAIN),
    column("cpu occ", CPU_OCC, unit="percentunit",
           thresholds=thresholds(("yellow", 0.8), ("red", 0.95)), drill=ACTORS),
    column("io occ", IO_OCC, unit="percentunit",
           thresholds=thresholds(("yellow", 0.8), ("red", 0.95)), drill=ACTORS),
    column("wedge s", WEDGE,
           unit="s", thresholds=thresholds(("yellow", 5), ("red", 30)), drill=ACTORS),
    column("rx overflow", RX_RATE,
           unit="pps", thresholds=thresholds(("yellow", 10), ("red", 1000)), drill=NETWORK),
]

VARIABLES = [
    *standard_variables(),
    agg_variable(),
    slot_variable(),
]

ROWS = [
    row("Triage", id=199, panels=[
        triage_table(
            "Active triage conditions", TRIAGE_CONDITIONS, id=39, w=24, h=6,
            description=(
                "Dashboard triage conditions active now, recomputed inline from base ton_* metrics "
                "with no recording-rule or ALERTS dependency. Each row is a condition whose red "
                "expression held for at least half of its debounce window, so a flapping breach — a "
                "node red on most scrapes but never continuously — still lists. Critical sorts above warning above info; "
                "the severity cell carries the colour and the panel body stays neutral. The instance "
                "cell links to that node; a chain condition has no instance. This is operational "
                "dashboard state, not a paging or Alertmanager surface. An empty table means no "
                "triage condition is active."
            ),
        ),
        plain_stat(
            "Nodes bad now", f"count(({NODE_SEVERITY}) >= 2) or on () vector(0)",
            id=40, scope="all selected nodes", unit="short", w=8, h=5,
            thresholds=thresholds(("red", 1)),
            description=(
                "Selected nodes at severity red or worse right now — count(node_severity >= 2), "
                "the composite computed inline from the per-node red conditions (unreachable, "
                "masterchain stale, shard lag, worker saturation, wedged worker, RX overflow). "
                "A down node is counted once. Zero is green; any bad node turns it "
                "red. The scorecard below names them worst-first."
            ),
        ),
        worst_node_stat(
            "Node severity", NODE_SEVERITY, id=41, drill=None, unit="short", decimals=0,
            sparkline=False, name_labels=("instance",), w=8, h=5,
            thresholds=thresholds(("yellow", 1), ("red", 2)),
            description=(
                "The single worst node by inline node_severity (0 ok, 1 warn, 2 red, 3 warn+red), "
                "named. It is the top row of the scorecard, surfaced as a tile so the worst node "
                "has a name before you read the table. Warn (yellow) is a threshold crossed, red a "
                "hard one; 3 means both tiers are lit on the same node."
            ),
        ),
        plain_stat(
            "Chain stalled?", f"max({mc_age_floor()}) or on () vector(0)",
            id=42, scope="the freshest node per selected chain", unit="s", decimals=0, w=8, h=5,
            thresholds=thresholds(("yellow", 30), ("red", 120)),
            description=(
                "The masterchain age of the freshest node in each selected job — inline "
                "min by (job) (clamp_min(ton_masterchain_block_age_seconds, 0)), the "
                "chain-versus-node discriminator. If this grows the whole fleet is behind together "
                "and no single node is to blame: the chain has stalled or this Prometheus lost "
                "every node. A node stale while this floor stays low is that node's problem, shown "
                "as its mc-age cell in the scorecard. Red at 120 s mirrors TonMasterchainStalled."
            ),
        ),
        scorecard(
            "What is bad, where", TRIAGE_COLUMNS, id=43, w=24, h=8,
            description=(
                "One row per selected node, worst severity first, so the first screen is the "
                "worst nodes at any fleet size. Each cell paints its own threshold — the same "
                "numbers the inline triage conditions use — and links to the "
                "board that explains it: masterchain cells to TON Blockchain, worker "
                "and wedge cells to TON Actors, RX overflow to TON Network, the node name to its "
                "own overview. The leading severity column (inline node_severity) drives the sort "
                "and is hidden. Instant vectors only: it loads at four hundred targets. Occupancy "
                "colours at 0.8/0.95 (the saturation condition), tighter than the 0.5/0.8 "
                "Fleet-health tiles, which watch a gentler slope."
            ),
        ),
        node_severity_timeline(
            "Nodes that went bad — history", TRIAGE_CONDITIONS, id=45, w=24, h=8,
            description=(
                "A row for every node-and-condition pair that was unhealthy at some point in the "
                "window — the row label names both, so each band answers what was bad, where, and "
                "when in one look; gaps are the healthy stretches. Yellow is the warn tier, red "
                "the bad tier, and a node tripping several conditions shows them as parallel "
                "rows. Healthy pairs are not drawn, so the panel stays legible on a large fleet "
                "and an empty one means everything stayed healthy; the magnitudes behind the "
                "colours live on the scorecard above."
            ),
        ),
        breach_timeline(
            "Breach history", TRIAGE_CONDITIONS, id=44, w=24, h=7,
            description=(
                "One stepped line per condition — the count breaching it, count(<red> == 1) "
                "computed inline — drawn only while that count is nonzero, so a gap means nobody "
                "breaches it and any visible step shows when a condition started and exactly how "
                "widespread it got; hover for the counts. Covers node-level conditions (the count "
                "is nodes breaching) and the chain-level ones — mc rate low, chain stalled, shard "
                "discard, collation slow (the count is jobs). Height is fixed, so this "
                "reads the same at eight nodes and four hundred; the two panels above say which "
                "node."
            ),
        ),
    ]),
    row("Fleet health", id=200, panels=[
        worst_node_stat(
            "Masterchain age",
            summed("job, instance", f"clamp_min(ton_masterchain_block_age_seconds{SEL}, 0)",
                   agg="max"),
            id=8, drill=BLOCKCHAIN, unit="s", decimals=1,
            thresholds=thresholds(("yellow", 30), ("red", 120)),
            description=(
                "Worst positive age of the last applied masterchain block across selected nodes "
                "(max — 'is any node stale'). A synced node tracks block time by a few seconds; a "
                "stalled or syncing node climbs. Negative/future clock skew is clamped to zero, so "
                "this is a staleness signal rather than a clock-skew alarm. Yellow at 30 s and red "
                "at 120 s are conservative deployment-neutral guides; tighten them for faster "
                "networks."
            ),
        ),
        worst_node_stat(
            "Shard lag",
            summed("job, instance", f"clamp_min(ton_masterchain_seqno{SEL}"
                                    f" - ton_shardclient_seqno{SEL}, 0)", agg="max"),
            id=10, drill=BLOCKCHAIN, unit="short",
            thresholds=thresholds(("yellow", 5), ("red", 20)),
            description=(
                "How many masterchain blocks the shard client trails the masterchain by "
                "(ton_masterchain_seqno - ton_shardclient_seqno, worst node). This is basechain "
                "processing lag: 0-2 is normal transient; sustained growth means shard blocks are "
                "applied slower than masterchain progresses, and basechain state (accounts, "
                "liteserver answers) goes stale even while MC block age looks healthy. Active "
                "shard count and validator-group membership live in the Workload row."
            ),
        ),
        worst_node_stat(
            "Actor CPU busy", worker_occupancy("cpu"), id=1, drill=ACTORS,
            unit="percentunit", decimals=1,
            thresholds=thresholds(("yellow", 0.5), ("red", 0.8)),
            description=(
                "Outer actor/coroutine dispatch time on cpu workers, including elapsed time in the "
                "current dispatch, divided by their thread count — worst node (max over the fleet; "
                "per-node and per-type breakdowns live on TON Actors). Signal-only work, gaps "
                "between handlers and finish bookkeeping are included. This is scheduler "
                "occupancy, not OS CPU: blocking and descheduling inside actor code are included. "
                "Sustained saturation can be relieved with more threads or less actor work."
            ),
        ),
        worst_node_stat(
            "Actor IO busy", worker_occupancy("io"), id=2, drill=ACTORS,
            unit="percentunit", decimals=1,
            thresholds=thresholds(("yellow", 0.5), ("red", 0.8)),
            description=(
                "Outer actor dispatch time on io workers, including elapsed time in the current "
                "dispatch, divided by their thread count — worst node (max over the fleet). "
                "Signal-only work, gaps between handlers and finish bookkeeping are included. This "
                "is scheduler occupancy, not OS CPU: blocking and descheduling inside actor code "
                "are included. Each scheduler has one io worker, so sustained saturation means "
                "moving work off polling actors."
            ),
        ),
        worst_node_stat(
            "Longest running handler",
            summed("job, instance, actor",
                   f"ton_actor_scheduler_current_execute_seconds{SEL}", agg="max"),
            id=18, drill=ACTORS, name_labels=("instance", "actor"), unit="s", decimals=2,
            sparkline=False, thresholds=thresholds(("yellow", 1), ("red", 5)),
            description=(
                "Longest actor or coroutine dispatch currently in flight on any node (max over the "
                "fleet); the collecting worker is excluded. This is a gauge sampled at each "
                "scrape, so short wedges between scrapes can be missed. Yellow starts at 1 s and "
                "red at 5 s. Treat red as a wedge only when it persists for several scrapes "
                "because a short spike can be a legitimate large batch. Per-type attribution lives "
                "on TON Actors. The tile names the worst node and the actor currently holding its "
                "worker."
            ),
        ),
        plain_stat(
            "Targets down", f"count(up{SEL} == 0) or on() vector(0)",
            id=19, scope="all selected targets", unit="short",
            thresholds=thresholds(("red", 1)),
            description=(
                "Configured Prometheus targets currently down for jobs matching the TON scrape job "
                "variable. Set it to your scrape_config name or regex. Collection runs at scrape "
                "time, so a node too wedged or starved to finish a gather within the scrape "
                "timeout lands here too, alongside an endpoint that no longer answers at all. "
                "Renders 0 when no matching up series exist at all (job absent from Prometheus), "
                "so pair with external scrape-target monitoring for total-pipeline loss."
            ),
        ),
        worst_node_stat(
            "Kernel RX overflow", both_transports("wire_dropped", ("quic", "adnl"),
                                                  direction="in", reason="limited"),
            id=7, drill=NETWORK, unit="pps", decimals=None, sparkline=False,
            thresholds=thresholds(("yellow", 10), ("red", 1000)),
            description=(
                "Worst-node datagrams/s dropped at socket receive queues across ADNL and QUIC "
                "(SO_RXQ_OVFL). Sustained nonzero means ingress outruns that node's readers. Reads "
                "0 structurally on macOS/local nets where SO_RXQ_OVFL is unavailable — 0 there is "
                "'no signal', not 'no drops'. The tile names the worst node. Aggregates all "
                "ton_scope values; ton-network filters by $scope, so numbers can differ."
            ),
        ),
    ]),
    row("Workload", id=201, panels=[
        plain_stat(
            "Masterchain block rate (of target)", f"({masterchain_rate('1m')}) * {SLOT}",
            id=23, scope="the whole chain", unit="percentunit", decimals=1, w=4,
            query_scope="job",
            thresholds=of_target_bands(),
            description=(
                "Canonical masterchain blocks applied per second over the last minute, evaluated "
                "now. The rate is derived from advance of the highest applied seqno across the "
                "selected jobs; during a mixed-version rollout it uses that head only when every "
                "applied-block reporter exports it, otherwise falling back to the median duplicate "
                "applied-block stream. This chain-level value intentionally ignores Instance and "
                "validator count. It is measured chain state; Slot (s) supplies only configured "
                "target context (target = 1 / slot). Shown as fraction of the configured target "
                "(1/$slot_seconds). Raw blk/s = value ÷ $slot_seconds; absolute rate lives on the "
                "ton-blockchain history row."
            ),
        ),
        plain_stat(
            "Shard block rate / active shard (of target)",
            f"(quantile(0.5, sum by (job, instance) (rate(ton_first_received_total"
            f"{sel(**JOB, workchain='0')}[1m])) / on (job, instance) clamp_min(avg_over_time("
            f"ton_active_shards{JOB_SEL}[1m]), 1))) * {SLOT}",
            id=20, scope="the median node", unit="percentunit", decimals=1, w=4,
            query_scope="job",
            thresholds=of_target_bands(),
            description=(
                "Canonical shard blocks applied per second per active shard over the last minute, "
                "evaluated now. Each duplicate node observation is divided by its average "
                "active-shard count over the same minute before the median reconciles reporters, "
                "so a split or merge does not pair a one-minute numerator with only the latest "
                "topology. This chain-level value intentionally ignores Instance and validator "
                "count. It is measured chain state; Slot (s) supplies only configured target "
                "context. Shown as fraction of the configured target (1/$slot_seconds). Raw blk/s "
                "= value ÷ $slot_seconds; absolute rate lives on the ton-blockchain history row."
            ),
        ),
        plain_stat(
            "Active shards", f"max(ton_active_shards{JOB_SEL})",
            id=21, scope="the best-informed node", unit="short", w=4,
            query_scope="job",
            thresholds=thresholds(base="blue"),
            description=(
                "Active non-masterchain leaf shards in the latest applied masterchain state. This "
                "is chain-level network topology, not the number of validator groups on one node, "
                "so the value intentionally ignores Instance."
            ),
        ),
        plain_stat(
            "Actor messages/s (per node)",
            f"sum(rate(ton_actor_messages_total{SEL}[{RATE}])) / clamp_min(count("
            f"max by (job, instance) (ton_actor_stats_enabled{SEL} == 1)), 1)",
            id=3, scope="the nodes that export actor stats", unit="ops", decimals=0, w=4,
            thresholds=thresholds(base="blue"),
            description=(
                "Mean actor messages plus bare coroutine resumptions completed per second per "
                "selected node with enabled actor statistics. The value remains comparable when "
                "the selected fleet size changes. Shows No data when no selected node has actor "
                "statistics enabled."
            ),
        ),
        plain_stat(
            "RX/node", f"avg({both_transports('wire_bytes', ('adnl', 'quic'), direction='in')})",
            id=4, scope="all selected nodes", unit="Bps", decimals=None, w=4,
            thresholds=thresholds(),
            description=(
                "Mean UDP payload bytes/s received per selected reporting node, summed locally "
                "across ADNL and QUIC sockets before averaging the fleet."
            ),
        ),
        plain_stat(
            "TX/node", f"avg({both_transports('wire_bytes', ('adnl', 'quic'), direction='out')})",
            id=5, scope="all selected nodes", unit="Bps", decimals=None, w=4,
            thresholds=thresholds(),
            description=(
                "Mean UDP payload bytes/s sent per selected reporting node, summed locally across "
                "ADNL and QUIC sockets before averaging the fleet."
            ),
        ),
        plain_stat(
            "Monitored validators (mc/shard/total)",
            line("mc", validator_count(chain="master")),
            line("shard", validator_count(chain="shard")),
            line("total", validator_count()),
            id=22, scope="all selected nodes", unit="short", sparkline=False,
            thresholds=thresholds(base="blue"),
            description=(
                "Monitored fleet counts, not the protocol-wide validator set: selected targets "
                "currently in at least one masterchain group (mc), in at least one shardchain "
                "group (shard), and all targets exporting validator-group state (total). The "
                "mc/shard counts reflect group membership at this instant and can flap with group "
                "rotation on small nets — that is rotation, not node loss."
            ),
        ),
        worst_node_stat(
            "Eligible mempool backlog", mempool.eligible(),
            id=9, drill=BLOCKCHAIN, unit="locale",
            thresholds=thresholds(("yellow", 1000), ("red", 5000)),
            description=(
                "Stored external messages whose active flag is true on the busiest selected node, "
                "across every destination chain and priority in that node's pool. A fleet sum would "
                "scale with fleet size and hide which node is falling behind. Green <1k, yellow "
                ">=1k, red >=5k. Eligible does not guarantee that the current collator can select "
                "every entry. Stateful exporters exclude postponed active=false storage; a postponed "
                "entry may remain there after its retry deadline until a collator snapshot revisits "
                "it. Legacy unlabeled history falls back to the old all-stored count. See TON "
                "Blockchain -> Mempool diagnostics for total storage, expiry-handler lag, and "
                "stock/flow balance."
            ),
        ),
        plain_stat(
            "Applied external TPS (shard, synced)", synced_applied("1m"),
            id=28, scope="the median synced node", unit="ops", decimals=1,
            query_scope="job",
            thresholds=thresholds(base="blue"),
            description=(
                "External messages observed in applied shardchain blocks per second over the last "
                "minute. The median includes only reporters whose absolute masterchain age is "
                "under 120 seconds and shard lag is at most two blocks, limiting replay spikes "
                "from syncing nodes and excluding future-skewed chain clocks. This counter is "
                "still a replicated node observation and includes catch-up replay; it is not an "
                "objective protocol-wide chain counter. Global admission is only context: other "
                "chains and local removal or expiry can absorb a gap. Hardcodes chain=\"shard\"; "
                "the ton-blockchain twin follows $chain and may include master."
            ),
        ),
        worst_node_stat(
            "Oldest pending external",
            mempool.oldest_age(),
            id=29, drill=BLOCKCHAIN, unit="s", decimals=0,
            thresholds=thresholds(("yellow", 60), ("red", 300)),
            description=(
                "Age of the oldest stored external message on the worst selected node, across all "
                "destination chains, priorities, and active/postponed states. Yellow begins at "
                "60s; red at 300s means messages are approaching their 600s TTL. A value at or "
                "past 600s means atomic deadline-driven expiry processing is late. Only stateful "
                "exporters are shown; legacy nodes are omitted because their periodic expiry "
                "semantics are not comparable."
            ),
        ),
    ]),
    row("Chain & reliability", id=202, panels=[
        chain_activity("Masterchain activity / s", "master", masterchain_rate(RATE), id=16,
            description=(
                "Masterchain only, over rolling one-minute windows across the selected jobs; this "
                "chain-wide panel intentionally ignores Instance because collators rotate. Block "
                "rate is derived from advance of the highest applied masterchain seqno; during a "
                "mixed-version rollout it uses that head only when every applied-block reporter "
                "exports it, otherwise falling back to the replica-median stream. Collations are "
                "summed because one rotating collator produces each candidate, and validations are "
                "divided by active masterchain validators so the rate stays comparable when "
                "validator count changes. A persistent collated-ok gap above block rate estimates "
                "discarded candidates."
            ),
        ),
        chain_activity("Shardchain activity / s", "shard", shard_rate(RATE), id=25,
            description=(
                "Shardchain only, across the selected jobs; this chain-wide panel intentionally "
                "ignores Instance because collators rotate. The median reconciles duplicate "
                "observations of the canonical applied-block stream, collations are summed because "
                "one rotating collator produces each candidate, and validations are divided by "
                "active shard validators so the rate stays comparable when the validator count "
                "changes. A persistent collated-ok gap above block rate estimates discarded "
                "candidates."
            ),
        ),
        chain_timeseries(
            "Collation budget used (5m)", line("budget used", collation_budget("5m")),
            scope="every collator in the fleet", unit="percentunit", id=30, h=9,
            query_scope="job",
            thresholds=thresholds(("yellow", 0.75), ("red", 1), style="line+area"),
            description=(
                "Elapsed collation wall time per produced shard block over a rolling five-minute "
                "window, divided by the configured Slot (s) (chain-wide: fleet sum of collation "
                "seconds over fleet sum of ok collations — one collator per block, so this is the "
                "chain truth). This backbone follows Job but intentionally ignores Instance "
                "because collators rotate. Above 100% collation alone overruns the configured slot "
                "and block rate must fall — the 'what takes so long' pointer; drill into TON "
                "Blockchain -> Block production for the phase breakdown. Yellow >=75%, red >=100%. "
                "Excludes wait_externals: time awaiting the external-message queue, including "
                "deliberate slot pacing and queue fetches. It is idle wait rather than measured "
                "collation work; on a fast build it can dominate elapsed time and fake a full "
                "budget. No successful collation in the window renders No data."
            ),
        ),
        fleet_timeseries(
            "Shard production & external issues",
            line("collation errors", error_share("collated", "5m")),
            line("validation errors", error_share("validated", "5m")),
            attributed("admission/storage issues",
                       ratio(rate("ton_mempool_ext_admission_total", "job, instance", over="5m",
                                  outcome="!~accepted|validated_only|duplicate|reprioritized"),
                             rate("ton_mempool_ext_admission_total", "job, instance", over="5m",
                                  outcome="!duplicate"))),
            unit="percentunit", id=17, thresholds=failure_bands(),
            description=(
                "Shardchain collation and validation error shares across the selected jobs; those "
                "two chain-wide signals intentionally ignore Instance because collators rotate. "
                "External admission/storage issues have no chain label, so that line is the worst "
                "selected node's share and names it; duplicates are excluded as normal broadcast "
                "traffic, while accepted, validated_only, and reprioritized are successful or "
                "stock-neutral rather than issues. A family with no attempts in the window renders "
                "No data. Local pool "
                "removals are detailed on TON Blockchain."
            ),
        ),
        agg_timeseries(
            "Transport failure ratios",
            failure_ratio("quic delivery", "ton_quic_message_delivery_failed_total",
                          "ton_quic_message_delivery_seconds_count"),
            failure_ratio("quic roundtrip", "ton_quic_query_roundtrip_failed_total",
                          "ton_quic_query_roundtrip_seconds_count"),
            failure_ratio("adnl roundtrip", "ton_adnl_query_roundtrip_failed_total",
                          "ton_adnl_query_roundtrip_seconds_count"),
            failure_ratio("inbound queries", "ton_adnl_query_failed_total",
                          "ton_adnl_query_duration_seconds_count", tl=r"!~dht\\..*"),
            failure_ratio("rldp2 delivery", "ton_rldp2_message_delivery_failed_total",
                          "ton_rldp2_message_delivery_seconds_count"),
            failure_ratio("rldp2 roundtrip", "ton_rldp2_query_roundtrip_failed_total",
                          "ton_rldp2_query_roundtrip_seconds_count"),
            unit="percentunit", id=31, h=8, legend=LIST_LEGEND, thresholds=failure_bands(),
            description=(
                "Each line is one node's failure share for that transport, collapsed across nodes "
                "by the Node aggregation switch — worst by default, so a single bad node is not "
                "diluted by the fleet, and the hidden ⇒ row names it. Delivery ratios cover "
                "fire-and-forget messages whose confirmation never came; roundtrip ratios cover "
                "queries that errored or timed out. Inbound failures include explicit errors and "
                "abandoned answer promises; DHT is excluded because client-mode nodes "
                "intentionally reject inbound DHT requests. A node contributes a healthy zero when "
                "it has attempts but no failures, so median and p95 describe the whole active fleet "
                "rather than only failing nodes. A transport with no attempts renders No data. "
                "Steady small percentages are normal; steps are not. Aggregates all ton_scope "
                "values; ton-network filters by $scope, so numbers can differ."
            ),
        ),
    ]),
    row("Quick attribution", id=203, collapsed=True, panels=[
        agg_timeseries(
            "Actor handler load",
            agg_line(rate("ton_actor_busy_ticks_total", "type, job, instance", per_tick=True),
                     key="type",
                     top=top_total("type", "ton_actor_busy_ticks_total", n=5, per_tick=True)),
            unit="percentunit", id=11, w=24, h=8,
            legend=table_legend("lastNotNull"), fill=0, line_width=1,
            description=(
                "Busiest actor/coroutine types: handler or resumption seconds per second — the "
                "fraction of one core — collapsed across the nodes running that type by the Node "
                "aggregation switch, worst by default, with the hidden ⇒ row naming that node. "
                "Actor and coroutine samples enter when the handler or resumption returns; live "
                "outer-dispatch time remains visible in worker occupancy. Top 5 types are ranked "
                "by total busy time over the visible window, so the series set stays stable while "
                "you scroll. Blocking and descheduling inside a handler count; signal-only work, "
                "gaps and dispatch bookkeeping don't. Values above 100% mean several instances of "
                "the type run concurrently on one node. Queues and tail latencies live on TON "
                "Actors."
            ),
        ),
        agg_timeseries(
            "Top per-node traffic by type, in", *top_traffic("in"),
            unit="ops", id=12, h=8, legend=table_legend("lastNotNull"),
            description=(
                "One node's rate for each inbound TL type, with ADNL and QUIC separate and top 5 "
                "per transport over the visible window; the Node aggregation switch picks which "
                "node, worst by default. This does not grow merely because more validators are "
                "selected. Per-query detail lives on TON Network."
            ),
        ),
        agg_timeseries(
            "Top per-node traffic by type, out", *top_traffic("out"),
            unit="ops", id=13, h=8, legend=table_legend("lastNotNull"),
            description=(
                "One node's rate for each outbound TL type, with messages, queries, and answers "
                "combined and top 5 per transport over the visible window; the Node aggregation "
                "switch picks which node, worst by default."
            ),
        ),
    ]),
]


def build():
    return dashboard("ton-overview", "TON Overview", ["ton", "overview"],
                     VARIABLES, ROWS, refresh="10s", version=1)
