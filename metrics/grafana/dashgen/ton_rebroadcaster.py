"""TON Rebroadcaster: proto-2 forwarding, public dissemination, and externals."""

from .lib import (
    BUILTIN_ANNOTATION,
    NODE,
    column,
    dashboard,
    line,
    panel,
    per_node_timeseries,
    scorecard,
    sel,
    standard_variables,
    summed,
    table_legend,
    target,
    thresholds,
)

SELF = "ton-rebroadcaster"
RATE = "$__rate_interval"
STATUS_WINDOW = "5m"

BOOL_HEALTH = thresholds(("green", 1), base="red")
BROADCAST_AGE = thresholds(("yellow", 5), ("red", 15))


def node_gauge(metric, *extra_labels, **labels):
    by = ", ".join(("job", "instance", *extra_labels))
    return summed(by, f"{metric}{sel(**NODE, **labels)}", agg="max")


def node_rate(metric, *extra_labels, **labels):
    by = ", ".join(("job", "instance", *extra_labels))
    return summed(by, f"rate({metric}{sel(**NODE, **labels)}[{RATE}])")


def node_increase(metric, *extra_labels, over=STATUS_WINDOW, **labels):
    by = ", ".join(("job", "instance", *extra_labels))
    return summed(by, f"increase({metric}{sel(**NODE, **labels)}[{over}])")


UP = node_gauge("up")


def node_default(expr, value):
    """Keep selected targets visible when a rebroadcaster metric family is absent."""
    return f"({expr}) or on (job, instance) ({value} + 0 * ({UP}))"


ENABLED = node_default(node_gauge("ton_rebroadcaster_enabled"), 0)
CONFIGURED_FANOUT = node_default(node_gauge("ton_rebroadcaster_configured_fanout"), 0)


def broadcast_age(
    chain=None, broadcast_type=None, *, oldest=False, default=False, smooth=None
):
    labels = {}
    extra = []
    if chain is None:
        extra.append("chain")
    else:
        labels["chain"] = chain
    if broadcast_type is None and not oldest:
        extra.append("type")
    elif broadcast_type is not None:
        labels["type"] = broadcast_type

    by = ", ".join(("job", "instance", *extra))
    metric = f"ton_rebroadcaster_last_broadcast_timestamp_seconds{sel(**NODE, **labels)}"
    age = f"clamp_min(timestamp({metric}) - {metric}, 0)"
    if smooth:
        age = f"avg_over_time(({age})[{smooth}:1s])"
    expr = summed(by, age, agg="max")
    return node_default(expr, 1e9) if default else expr


MC_AGE = broadcast_age("master", oldest=True, default=True)
SHARD_AGE = broadcast_age("shard", oldest=True, default=True)
NO_FAST_SYNC_ROUTE = node_default(
    node_increase(
        "ton_rebroadcaster_external_messages_total", result="no_fast_sync_route"
    ),
    0,
)

RED = (
    f"clamp_max(({UP} == bool 0) + ({ENABLED} == bool 0) + "
    f"({MC_AGE} > bool 15) + ({SHARD_AGE} > bool 15) + "
    f"({NO_FAST_SYNC_ROUTE} > bool 0), 1)"
)
WARN = f"clamp_max(({MC_AGE} > bool 5) + ({SHARD_AGE} > bool 5), 1)"
SEVERITY = f"2 * ({RED}) + ({WARN})"


def broadcast_rate_panel(chain, title, panel_id):
    metric = "ton_rebroadcaster_broadcasts_total"
    return per_node_timeseries(
        title,
        line(
            "{{instance}} / {{type}} / {{source}}",
            node_rate(metric, "type", "source", chain=chain),
        ),
        line("{{instance}} / {{type}} / total", node_rate(metric, "type", chain=chain)),
        id=panel_id,
        unit="ops",
        h=9,
        legend=table_legend("lastNotNull", "min", "max", sort="Min"),
        description=(
            f"{chain.capitalize()} candidate and finality broadcasts/s scheduled from fast-sync "
            "or custom overlays to the public Plumtree overlay by each selected hero. Candidate "
            "and finality totals separately sum their first-arrival sources."
        ),
    )


def broadcast_source_pie(chain, broadcast_type, title, panel_id):
    metric = f"ton_rebroadcaster_broadcasts_total{sel(**NODE, chain=chain, type=broadcast_type)}"
    return panel(
        "piechart",
        title,
        (
            f"Share of selected {chain} {broadcast_type} broadcasts by the source whose "
            "first arrival won rebroadcast deduplication over the visible time range."
        ),
        id=panel_id,
        unit="short",
        w=6,
        h=7,
        targets=[
            target(
                f"sum by (source) (increase({metric}[$__range] @ end()))",
                legend="{{source}}",
                instant=True,
            )
        ],
        options={
            "displayLabels": ["name", "percent"],
            "legend": {
                "displayMode": "table",
                "placement": "right",
                "showLegend": False,
                "values": [],
            },
            "pieType": "pie",
            "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
            "tooltip": {"hideZeros": False, "mode": "single", "sort": "none"},
        },
        defaults={
            "color": {"mode": "palette-classic"},
            "custom": {"hideFrom": {"legend": False, "tooltip": False, "viz": False}},
        },
        query_scope="selected",
    )


def arrival_rate_panel(chain, title, panel_id):
    return per_node_timeseries(
        title,
        line(
            "{{instance}} / {{type}} / {{source}}",
            node_rate(
                "ton_rebroadcaster_arrivals_total",
                "type",
                "source",
                chain=chain,
            ),
        ),
        id=panel_id,
        unit="ops",
        h=9,
        legend=table_legend("lastNotNull", "min", "max", sort="Min"),
        description=(
            f"{chain.capitalize()} candidate and finality arrivals/s received from fast-sync "
            "and custom overlays by each selected hero, including arrivals suppressed by "
            "rebroadcast deduplication."
        ),
    )


def broadcast_age_panel(chain, title, panel_id):
    return per_node_timeseries(
        title,
        line(
            "{{instance}} / {{type}}",
            broadcast_age(chain=chain, smooth="2s"),
        ),
        id=panel_id,
        unit="s",
        h=9,
        thresholds=BROADCAST_AGE,
        fill=0,
        legend=table_legend("lastNotNull", "max", sort="Max"),
        description=(
            f"Age at scrape time of each selected hero's latest forwarded {chain} candidate and "
            "finality. Yellow at 5 seconds and red at 15 seconds. Exporter reachability is "
            "monitored separately."
        ),
    )


def eager_peers_panel(chain, title, panel_id):
    return per_node_timeseries(
        title,
        line(
            "{{instance}}",
            node_gauge("ton_overlay_public_plumtree_eager_peers_avg", chain=chain),
        ),
        id=panel_id,
        unit="short",
        decimals=1,
        h=9,
        legend=table_legend("lastNotNull", "min", "max", sort="Min"),
        description=(
            f"Average eager-set size across the {chain} public overlay's Plumtree broadcast "
            "trees. Eager peers receive full candidate/finality payloads; the rest receive "
            "IHAVEs."
        ),
    )


VARIABLES = standard_variables()
datasource_variable, job_variable, instance_variable = VARIABLES
datasource_variable["hide"] = 2
job_variable["current"] = {
    "selected": True,
    "text": "ton-mainnet-hero",
    "value": "ton-mainnet-hero",
}
instance_variable["current"] = {
    "selected": True,
    "text": ["All"],
    "value": ["$__all"],
}

ROWS = [
    scorecard(
        "Rebroadcaster fleet",
        [
            column("severity", SEVERITY, thresholds=thresholds(("yellow", 1), ("red", 2))),
            column("up", UP, thresholds=BOOL_HEALTH, drill=SELF),
            column("enabled", ENABLED, thresholds=BOOL_HEALTH, drill=SELF),
            column("fanout", CONFIGURED_FANOUT, drill=SELF),
            column("mc last block ago", MC_AGE, unit="s", thresholds=BROADCAST_AGE, drill=SELF),
            column(
                "shard last block ago",
                SHARD_AGE,
                unit="s",
                thresholds=BROADCAST_AGE,
                drill=SELF,
            ),
        ],
        id=11,
        overview=SELF,
        h=8,
        description=(
            "One row per selected hero, sorted by hidden severity. Red means the target or "
            "rebroadcaster is down, the older candidate/finality stream for either chain has "
            "been silent for more than 15 seconds, or a validated external had no fast-sync "
            "route in the last five minutes. Yellow starts at five seconds of broadcast silence."
        ),
        overrides={"fieldConfig": {"defaults": {"custom": {"minWidth": 50}}}},
    ),
    broadcast_source_pie("master", "candidate", "Master candidate broadcast source", 43),
    broadcast_source_pie("master", "finality", "Master finality broadcast source", 44),
    broadcast_source_pie("shard", "candidate", "Shard candidate broadcast source", 45),
    broadcast_source_pie("shard", "finality", "Shard finality broadcast source", 46),
    broadcast_rate_panel("master", "Master broadcast rate by hero", 21),
    broadcast_rate_panel("shard", "Shard broadcast rate by hero", 26),
    arrival_rate_panel("master", "Master arrival rate by hero", 22),
    arrival_rate_panel("shard", "Shard arrival rate by hero", 27),
    broadcast_age_panel("master", "Master time since last broadcast", 24),
    broadcast_age_panel("shard", "Shard time since last broadcast", 30),
    eager_peers_panel("master", "Master Plumtree eager peers by hero", 33),
    eager_peers_panel("shard", "Shard Plumtree eager peers by hero", 34),
    per_node_timeseries(
        "Public overlay peers by hero",
        line(
            "all peers / {{instance}} / {{chain}}",
            node_gauge("ton_overlay_public_peer_memberships", "chain"),
        ),
        id=31,
        unit="short",
        h=9,
        legend=table_legend("lastNotNull", "min", "max", sort="Min"),
        description=(
            "Current public-overlay peer-table size for every selected hero and chain. The "
            "separate eager-peer panels show the peers currently receiving full Plumtree "
            "payloads."
        ),
    ),
    per_node_timeseries(
        "External relay outcomes by hero",
        line(
            "{{instance}} / {{result}}",
            node_rate(
                "ton_rebroadcaster_external_messages_total",
                "result",
                result="~(relayed|no_fast_sync_route)",
            ),
        ),
        id=42,
        unit="ops",
        h=9,
        legend=table_legend("lastNotNull", "max", sort="Max"),
        description=(
            "Validated public external messages/s reaching the relay stage. Relayed means the "
            "message was submitted to the matching fast-sync overlay; no_fast_sync_route means "
            "no matching overlay was available and the message was dropped."
        ),
    ),
]


def build():
    return dashboard(
        SELF,
        "TON Rebroadcaster",
        ["ton", "rebroadcaster", "network"],
        VARIABLES,
        ROWS,
        refresh="10s",
        version=1,
        time=("now-3h", "now"),
        annotations=(BUILTIN_ANNOTATION,),
    )
