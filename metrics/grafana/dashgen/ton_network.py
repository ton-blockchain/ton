"""TON Network: what the transports carry, how fast they answer, and where packets die."""

from functools import partial

from .lib import (
    LIST_LEGEND,
    NODE,
    NODE_LABELS,
    WARN,
    agg_line,
    agg_timeseries,
    agg_variable,
    attributed,
    chain_timeseries,
    dashboard,
    failure_bands,
    fleet_timeseries,
    histogram_quantile,
    line,
    per_node_timeseries,
    plain_stat,
    ratio,
    row,
    sel,
    series_style,
    standard_variables,
    summed,
    table_legend,
    threshold_lines,
    thresholds,
    top_total,
    variable_custom,
    variable_query,
    worst_node_stat,
)

SELF = "ton-network"
RATE = "$__rate_interval"
# Every series on this board is scope-filtered; the peer-attributable QUIC ones also by trust.
NET = {**NODE, "ton_scope": "~$scope"}
TRUSTED = {**NET, "trust": "~$trust"}
BY_NODE = "{{ton_scope}} / {{job}} / {{instance}}"
NODE_KEYS = "job, instance, ton_scope"
RTT = f"ton_quic_transport_mean_rtt_seconds{sel(**NET)}"
RTT_BANDS = threshold_lines(0.5, 2, warn_color=WARN)

# A node here is one scope on one node, so that is what an attribution overlay transplants. The
# board's default legend is the plain list, so the ▾ flavour keeps it rather than the table one.
NET_LABELS = (*NODE_LABELS, "ton_scope")
net_timeseries = partial(fleet_timeseries, transplant=NET_LABELS)
net_agg_timeseries = partial(agg_timeseries, transplant=NET_LABELS, legend=LIST_LEGEND)


def net_rate(metric, by=None, *, over=RATE, trusted=False, **labels):
    """Per-second rate of a transport counter, filtered the way this board filters everything."""
    return summed(by, f"rate({metric}{sel(**(TRUSTED if trusted else NET), **labels)}[{over}])")


def share(numerator, denominator, by=None, *, floor=0, trusted=False, **labels):
    """One rate over another, guarded so a quiet period draws nothing at all."""
    return (f"{net_rate(numerator, by, trusted=trusted, **labels)}"
            f" / ({net_rate(denominator, by, trusted=trusted, **labels)} > {floor})")


def node_rate(name, metric, key="direction", **kw):
    """One node's rate of a transport counter, kept per `key` inside the node; the ▾ picks which."""
    return agg_line(net_rate(metric, f"{NODE_KEYS}, {key}", **kw), key=key, name=name)


def node_share(name, numerator, denominator, key=None, **kw):
    """One node's share of one rate over another, kept per `key` inside the node."""
    by = f"{NODE_KEYS}, {key}" if key else NODE_KEYS
    return agg_line(share(numerator, denominator, by, **kw), key=key, name=name)


def busiest(metric, *, n, trusted=False, **labels):
    """Keep the n payload types that moved the most over the visible range."""
    return top_total("tl", metric, n=n, selector=sel(**(TRUSTED if trusted else NET), **labels))


def quantile(metric, *, q=0.95, by=None, trusted=False, **labels):
    """The p95 of a latency histogram, over the selected peers."""
    return histogram_quantile(q, metric, by=by, **(TRUSTED if trusted else NET), **labels)


def tagged_wire(by=None, **labels):
    """adnl and quic wire bytes as one series, each side tagged with the transport it came from."""
    def tagged(transport):
        counter = f"ton_{transport}_wire_bytes_total{sel(**NET, **labels)}"
        return f'label_replace(rate({counter}[{RATE}]), "transport", "{transport}", "", "")'

    return summed(by, f"{tagged('adnl')} or {tagged('quic')}")


def rx_overflow(by=None):
    """Datagrams the kernel receive buffer threw away, both transports together."""
    def dropped(transport):
        counter = (f"ton_{transport}_wire_dropped_total"
                   f"{sel(**NET, direction='in', reason='limited')}")
        return f'label_replace(rate({counter}[{RATE}]), "transport", "{transport}", "", "")'

    return summed(by, f"{dropped('quic')} or {dropped('adnl')}")


def drops(transport, tier, *, trusted=False):
    """One drop stream: which tier of which transport threw the traffic away, and why."""
    key = ("trust, " if trusted else "") + "direction, reason"
    trust = "{{trust}} " if trusted else ""
    dropped = net_rate(f"ton_{transport}_{tier}_dropped_total", f"{NODE_KEYS}, {key}",
                       trusted=trusted)
    return agg_line(f"{dropped} > 0", key=key,
                    name=f"{transport} {tier} {trust}{{{{direction}}}} {{{{reason}}}}")


def app_traffic(metric):
    """The same payload counter split per transport, with QUIC also split by peer class."""
    return [
        line("adnl {{kind}} {{direction}}", net_rate(f"ton_adnl_{metric}", "kind, direction")),
        line("quic {{trust}} {{kind}} {{direction}}",
             net_rate(f"ton_quic_{metric}", "trust, kind, direction", trusted=True)),
        line("rldp2 {{kind}} {{direction}}", net_rate(f"ton_rldp2_{metric}", "kind, direction")),
    ]


def top_payloads(direction, *, n=6):
    """The heaviest payload types in one direction, quic first, then plain adnl."""
    quic, adnl = "ton_quic_app_bytes_total", "ton_adnl_app_bytes_total"
    return [
        line("quic {{trust}} {{tl}}",
             net_rate(quic, "trust, tl", trusted=True, direction=direction)
             + busiest(quic, n=n, trusted=True, direction=direction)),
        line("adnl {{tl}}",
             net_rate(adnl, "tl", direction=direction)
             + busiest(adnl, n=n, direction=direction)),
    ]


def broadcast(metric, *, n=6):
    """Overlay broadcast content by TL type, in and out."""
    counter = f"ton_overlay_broadcast_{metric}_total"
    return [line(f"{direction} {{{{tl}}}}",
                 net_rate(counter, "tl", direction=direction)
                 + busiest(counter, n=n, direction=direction))
            for direction in ("in", "out")]


def transfer_rate(**labels):
    """RLDP2 bulk transfers per second, optionally only those in one terminal state."""
    return f"rate(ton_rldp2_transport_transfers_total{sel(**NET, **labels)}[{RATE}])"


def transfer_failure_share():
    """One node's failed and timed-out transfers over all its terminal outcomes."""
    by = f"{NODE_KEYS}, direction"
    failed = summed(f"{by}, state", transfer_rate(state="~failed|timeout"))
    ended = summed(by, transfer_rate())
    return (f"({failed} / on ({by}) group_left {ended})"
            f" and on ({by}) ({ended} > 0)")


def adnl_stack_bytes():
    """Everything an adnl datagram carried: its own payloads plus the rldp2 traffic on top."""
    def tagged(metric, layer, **labels):
        return (f'label_replace(rate(ton_{metric}{sel(**NET, **labels)}[{RATE}]),'
                f' "layer", "{layer}", "", "")')

    return (f'{tagged("adnl_app_bytes_total", "adnl", tl="!rldp2.messagePart")}'
            f' or {tagged("rldp2_app_bytes_total", "rldp2")}')


def overflow_by_node(transport):
    """Kernel receive-buffer drops for one transport, kept per node."""
    dropped = net_rate(f"ton_{transport}_wire_dropped_total", NODE_KEYS,
                       direction="in", reason="limited")
    return line(f"{transport} {BY_NODE}", f"{dropped} > 0")


def handshake_completion(direction):
    """One node's completed handshakes over its dial attempts, over 15m; high is good here."""
    done = net_rate("ton_quic_transport_handshakes_total", NODE_KEYS, over="15m",
                    direction=direction, result="completed")
    tried = net_rate("ton_quic_transport_connections_total", NODE_KEYS, over="15m",
                     direction=direction)
    return agg_line(ratio(done, tried), name=direction, pick="min")


def handshakes(result):
    """Handshakes that reached one outcome, per direction, on the busiest node."""
    return node_rate(f"{result} {{{{direction}}}}", "ton_quic_transport_handshakes_total",
                     result=result)


def wire_throughput(transport):
    """Bytes one transport put on the wire, per direction."""
    return line(f"{transport} {{{{direction}}}}",
                net_rate(f"ton_{transport}_wire_bytes_total", "direction"))


def datagrams_per_syscall(transport):
    """How many datagrams one syscall carried on one node; more is cheaper, so low is bad."""
    return agg_line(share(f"ton_{transport}_wire_packets_total",
                          f"ton_{transport}_wire_syscalls_total", f"{NODE_KEYS}, direction"),
                    key="direction", name=f"{transport} {{{{direction}}}}", pick="min")


def outbound_queries(transport, *, n=5):
    """The busiest outbound query types of one transport."""
    counter = f"ton_{transport}_query_roundtrip_seconds_count"
    return line(f"{transport} {{{{tl}}}}",
                net_rate(counter, "tl") + busiest(counter, n=n))


VARIABLES = [
    *standard_variables(),
    agg_variable(),
    variable_query(
        "scope", label="scope", sort=1,
        query="label_values(ton_exporter_collections_total, ton_scope)",
        description=(
            "Metric-scope filter: which exporter scope the transport series are read from. Nodes "
            "report their own scope values, so a fleet mid-rollout can expose more than one."
        ),
        extra={"definition": "label_values(ton_exporter_collections_total, ton_scope)",
               "allValue": ".*"},
    ),
    variable_custom(
        "trust", label="QUIC peer class", choices=["trusted", "untrusted"], selected="All",
        include_all=True, all_value=".*",
        description=(
            "Filters peer-attributable QUIC app, query, delivery, and ready-connection metrics. "
            "The wire/app overhead ratio remains all-peers so its app denominator matches the "
            "unsplit wire numerator. Socket/pre-auth QUIC and all ADNL, RLDP2, and overlay series "
            "also remain all-peers. Pre-trust builds expose no trust label: their QUIC series "
            "appear under All, but not under trusted or untrusted."
        ),
    ),
]

ROWS = [
    plain_stat(
        "UDP in (fleet)", tagged_wire(direction="in"),
        id=1, scope="every selected node", unit="Bps", decimals=None, w=4,
        thresholds=thresholds(base="blue"),
        description=(
            "UDP payload bytes/s the sockets received, summed over ADNL and QUIC for all selected "
            "nodes. RLDP2 rides inside ADNL and is already counted there. Informational; no health "
            "threshold."
        ),
    ),
    plain_stat(
        "UDP out (fleet)", tagged_wire(direction="out"),
        id=2, scope="every selected node", unit="Bps", decimals=None, w=4,
        thresholds=thresholds(base="blue"),
        description=(
            "UDP payload bytes/s the kernel accepted for sending, summed over ADNL and QUIC for "
            "all selected nodes. Refused sends are drops, not wire traffic. Informational; no "
            "health threshold."
        ),
    ),
    plain_stat(
        "QUIC query RTT p95", quantile("ton_quic_query_roundtrip_seconds", trusted=True),
        id=3, scope="every selected node", unit="s", decimals=2, w=4,
        thresholds=thresholds(("yellow", 0.5), ("red", 2)),
        description=(
            "Outbound QUIC query roundtrip p95 for the selected peer class, computed from "
            "histogram buckets summed across the fleet. Includes network path, peer processing, "
            "and transfer time. Yellow at 0.5 s; red at 2 s."
        ),
    ),
    plain_stat(
        "QUIC query fail %",
        share("ton_quic_query_roundtrip_failed_total", "ton_quic_query_roundtrip_seconds_count",
              trusted=True),
        id=4, scope="every selected node", unit="percentunit", decimals=2, w=4,
        thresholds=thresholds(("yellow", 0.01), ("red", 0.05)),
        description=(
            "Outbound QUIC query failures divided by measured roundtrips for the selected peer "
            "class across the fleet. Green below 1%, yellow at 1%, red at 5%. No data means no "
            "completed QUIC queries in the interval; it is not presented as a healthy 0%."
        ),
    ),
    plain_stat(
        "QUIC packet loss %",
        f"{net_rate('ton_quic_transport_packets_lost_total')}"
        f" / ({net_rate('ton_quic_transport_packets_total', direction='out')} > 0)",
        id=5, scope="every selected node", unit="percentunit", decimals=2, w=4,
        thresholds=thresholds(("yellow", 0.01), ("red", 0.05)),
        description=(
            "QUIC packets declared lost divided by outbound packets across the fleet. Most loss is "
            "recovered, but it costs bandwidth and tail latency. Yellow at 1%; red at 5%. No "
            "outbound packets in the current interval renders No data, not 0%."
        ),
    ),
    worst_node_stat(
        "Kernel RX overflow", rx_overflow("job, instance"),
        id=6, drill=SELF, unit="pps", decimals=None, w=4,
        thresholds=thresholds(("yellow", 10), ("red", 1000)),
        description=(
            "Worst-node ADNL + QUIC datagrams/s dropped because a socket receive buffer was full "
            "(SO_RXQ_OVFL). Sustained nonzero means ingress outruns the reader; raise SO_RCVBUF or "
            "add read capacity, then inspect the by-node panel. Linux-only: macOS/local nets "
            "report structural zero, meaning no signal rather than proof of no drops. The tile "
            "names the worst node."
        ),
    ),
    row("Service health", id=40, panels=[
        chain_timeseries(
            "Outbound query RTT p95 by transport",
            line("quic {{trust}}",
                 quantile("ton_quic_query_roundtrip_seconds", by="trust", trusted=True)),
            line("adnl", quantile("ton_adnl_query_roundtrip_seconds")),
            scope="every selected node", unit="s", id=19, h=9, thresholds=RTT_BANDS,
            description=(
                "Transport-accept to answer for queries we send: network + peer processing + "
                "transfer time. QUIC is split by peer class; ADNL remains all-peers. p95 comes "
                "from fleet-summed histogram buckets. RLDP2 bulk-transfer latency is separate "
                "below because it is not comparable with low-latency ADNL/QUIC."
            ),
        ),
        chain_timeseries(
            "Inbound query processing p95 by type",
            line("{{tl}}", quantile("ton_adnl_query_duration_seconds", by="tl")
                 + top_total("tl", "ton_adnl_query_duration_seconds_count", selector=sel(**NET))),
            scope="every selected node", unit="s", id=20, h=9, legend=table_legend("lastNotNull"),
            description=(
                "Time this node spends answering inbound queries, excluding network transfer. All "
                "transports funnel through this ADNL delivery-layer timer. Histogram buckets are "
                "summed across the fleet; the top 8 types are ranked by call volume over the "
                "visible range."
            ),
        ),
        chain_timeseries(
            "Message delivery p95",
            line("quic {{trust}}",
                 quantile("ton_quic_message_delivery_seconds", by="trust", trusted=True)),
            scope="every selected node", unit="s", id=21,
            description=(
                "QUIC fire-and-forget delivery p95 to the receiver's empty acknowledgement, split "
                "by peer class and computed from fleet-summed histogram buckets. Plain ADNL has no "
                "acknowledgement. RLDP2 transfer delivery is separate below because bulk-transfer "
                "latency is not comparable."
            ),
        ),
        net_agg_timeseries(
            "Failure & loss ratios",
            node_share("quic {{trust}} delivery", "ton_quic_message_delivery_failed_total",
                       "ton_quic_message_delivery_seconds_count", "trust", trusted=True),
            node_share("quic {{trust}} roundtrip", "ton_quic_query_roundtrip_failed_total",
                       "ton_quic_query_roundtrip_seconds_count", "trust", trusted=True),
            node_share("rldp2 roundtrip", "ton_rldp2_query_roundtrip_failed_total",
                       "ton_rldp2_query_roundtrip_seconds_count"),
            agg_line(ratio(net_rate("ton_quic_transport_packets_lost_total", NODE_KEYS),
                           net_rate("ton_quic_transport_packets_total", NODE_KEYS,
                                    direction="out")), name="quic packet loss"),
            node_share("adnl roundtrip", "ton_adnl_query_roundtrip_failed_total",
                       "ton_adnl_query_roundtrip_seconds_count"),
            node_share("rldp2 delivery", "ton_rldp2_message_delivery_failed_total",
                       "ton_rldp2_message_delivery_seconds_count"),
            unit="percentunit", id=22, thresholds=failure_bands(),
            description=(
                "Failures divided by measured deliveries, roundtrips, or packets, computed per "
                "node and then collapsed by the Node aggregation switch rather than fleet-wide, so "
                "one struggling peer set is not averaged away; at the default worst each line is "
                "its worst node and the hidden ⇒ row names it. Idle series disappear rather than "
                "presenting 0/0 as healthy. QUIC app outcomes are split by peer class; packet "
                "loss, ADNL, and RLDP2 remain all-peers. Persistent values above 1% deserve "
                "investigation."
            ),
        ),
    ]),
    row("Path & packet health", id=41, panels=[
        net_agg_timeseries(
            "Network drops by tier and reason",
            drops("quic", "wire"), drops("quic", "transport"), drops("quic", "app", trusted=True),
            drops("adnl", "transport"), drops("adnl", "wire"), drops("adnl", "app"),
            drops("rldp2", "transport"), drops("rldp2", "app"),
            unit="ops", id=16,
            description=(
                "Nonzero drop rates by tier, direction, and reason, computed per node and then "
                "collapsed by the Node aggregation switch — at the default worst, each is its "
                "worst node, which the hover rows name, because drops usually come from one node. "
                "The "
                "peer-class filter applies only to QUIC app drops; wire, pre-auth transport, ADNL, "
                "and RLDP2 remain all-peers. QUIC app out/limited indicates stream-credit "
                "exhaustion."
            ),
        ),
        net_timeseries(
            "QUIC RTT across nodes (p50 / p95 / max)",
            line("p50 node", f"quantile(0.5, {RTT} > 0)"),
            line("p95 node", f"quantile(0.95, {RTT} > 0)"),
            attributed("max node", RTT),
            unit="s", id=17, thresholds=RTT_BANDS,
            description=(
                "Distribution over nodes of ton_quic_transport_mean_rtt_seconds — each node "
                "contributes one value: the connection-weighted mean smoothed RTT (ngtcp2 SRTT) "
                "over its open connections. p50 = the typical node, max = the worst node. This "
                "panel deliberately carries no ▾: it already draws the whole node distribution at "
                "once, so letting the Node aggregation switch pick one point of it would collapse "
                "three lines into one and lose the comparison the panel exists for. This is "
                "network path RTT; peer processing time is excluded (compare the roundtrip row, "
                "which includes it)."
            ),
        ),
        per_node_timeseries(
            "Kernel RX overflow by node/transport",
            overflow_by_node("quic"), overflow_by_node("adnl"),
            unit="pps", id=12, fill=6, legend=table_legend("lastNotNull"),
            thresholds=threshold_lines(10, 1000, warn_color="#EAB839"),
            description=(
                "SO_RXQ_OVFL by node and transport: datagrams dropped before userspace could read "
                "them. Only nonzero series render. Sustained drops call for more receive-buffer or "
                "reader capacity. Linux-only: macOS/local nets report structural zero, meaning no "
                "signal rather than proof of no drops."
            ),
        ),
        per_node_timeseries(
            "Mean RTT by node",
            line(BY_NODE, f"{RTT} > 0"),
            unit="s", id=18, fill=6, legend=table_legend("lastNotNull"),
            description=(
                "Each node's connection-weighted mean smoothed RTT. A node whose line sits far "
                "above the rest peers across a long path or has a saturated uplink; a sudden step "
                "usually means its peer set changed."
            ),
        ),
    ]),
    row("Failure drill-down", id=42, collapsed=True, panels=[
        net_agg_timeseries(
            "Inbound query failure ratio by type",
            node_share("{{tl}}", "ton_adnl_query_failed_total",
                       "ton_adnl_query_duration_seconds_count", "tl", floor=0.1),
            unit="percentunit", id=32, h=9,
            axis_label="failure share", thresholds=failure_bands(),
            legend=table_legend("lastNotNull"),
            description=(
                "Inbound query failure share by type at the common delivery layer: explicit errors "
                "and dropped answer promises over completed-query observations, computed per node "
                "and collapsed by the Node aggregation switch (worst node per type by default). "
                "All transports funnel through this layer; types below 0.1 q/s on a "
                "node are hidden there. High dht.* failure is usually expected client-mode "
                "rejection unless --dht-server is enabled."
            ),
        ),
        net_agg_timeseries(
            "Outbound roundtrip failure ratio by type",
            node_share("quic {{trust}} {{tl}}", "ton_quic_query_roundtrip_failed_total",
                       "ton_quic_query_roundtrip_seconds_count", "trust, tl",
                       floor=0.1, trusted=True),
            node_share("adnl {{tl}}", "ton_adnl_query_roundtrip_failed_total",
                       "ton_adnl_query_roundtrip_seconds_count", "tl", floor=0.1),
            node_share("rldp2 {{tl}}", "ton_rldp2_query_roundtrip_failed_total",
                       "ton_rldp2_query_roundtrip_seconds_count", "tl", floor=0.1),
            unit="percentunit", id=33, h=9,
            axis_label="failure share", thresholds=failure_bands(),
            legend=table_legend("lastNotNull"),
            description=(
                "Outbound query failure share by type and transport, computed per node and "
                "collapsed by the Node aggregation switch (worst node per series by default); "
                "series below 0.1 q/s on a node are hidden there. QUIC is split by peer class; "
                "ADNL and RLDP2 remain all-peers."
            ),
        ),
    ]),
    row("Connections & capacity", id=43, collapsed=True, panels=[
        chain_timeseries(
            "Open and ready QUIC connections",
            line("installed {{direction}}",
                 summed("direction", f"ton_quic_transport_connections_current{sel(**NET)}")),
            line("ready {{trust}} {{direction}}",
                 summed("direction, trust",
                        f"ton_quic_transport_connections_ready{sel(**TRUSTED)}")),
            scope="every selected node", unit="short", id=34,
            styles=[series_style("^installed ", regex=True, width=1, fill=0, dash=(10, 10))],
            description=(
                "Installed QUIC connections include handshakes and remain all-peers; ready paths "
                "are authenticated and split by direction and peer class. Installed and ready "
                "overlap and must not be added. Fleet sum across selected nodes."
            ),
        ),
        net_agg_timeseries(
            "New QUIC connections: attempts vs completions",
            node_rate("attempts {{direction}}", "ton_quic_transport_connections_total"),
            handshakes("completed"), handshakes("rejected"),
            unit="ops", id=14,
            description=(
                "Connection installs versus completed/rejected QUIC handshakes, per node and then "
                "collapsed by the Node aggregation switch — at the default worst each line is its "
                "busiest node, which the hover rows name, because a fleet sum hides the one node "
                "that redials. The gap is handshakes that died without a verdict, usually idle "
                "timeouts "
                "dialing peers that never answered."
            ),
        ),
        net_agg_timeseries(
            "Handshake completion / attempt",
            handshake_completion("out"), handshake_completion("in"),
            unit="percentunit", id=15,
            description=(
                "Completion event rate divided by connection-attempt event rate, by dial "
                "direction over 15m, per node and then collapsed by the Node aggregation switch. "
                "High is good here, so the switch runs the other way: worst is the fleet minimum "
                "and best its maximum, and the hover rows name the least successful node. This is "
                "a laggy proxy, not a "
                "cohort success percentage, and can transiently exceed 100%. Abandoned handshakes "
                "have no terminal outcome; no attempts in the window renders No data."
            ),
        ),
        per_node_timeseries(
            "Open QUIC streams by node",
            line(BY_NODE, f"ton_quic_transport_sids_current{sel(**NET)}"),
            unit="short", id=23, fill=6,
            description=(
                "Currently open streams per node, both directions of initiation (each side capped "
                "at 4096 per connection). A plateau near the cap while sends start failing with "
                "app dropped{out,limited} is the stream-credit exhaustion signature."
            ),
        ),
    ]),
    row("Traffic & efficiency", id=44, collapsed=True, panels=[
        chain_timeseries(
            "Wire throughput by transport",
            wire_throughput("adnl"), wire_throughput("quic"),
            scope="every selected node", unit="Bps", id=7,
            description=(
                "UDP payload bytes/s from each transport's socket accounting, summed across "
                "selected nodes. ADNL and QUIC own sockets; RLDP2 rides inside ADNL and is already "
                "present in its socket total."
            ),
        ),
        net_agg_timeseries(
            "App payload by transport",
            node_rate("adnl {{direction}}", "ton_adnl_app_bytes_total"),
            node_rate("quic {{trust}} {{direction}}", "ton_quic_app_bytes_total",
                      "trust, direction", trusted=True),
            node_rate("rldp2 {{direction}}", "ton_rldp2_app_bytes_total"),
            unit="Bps", id=9,
            description=(
                "Application payload bytes/s after protocol framing, per node and then collapsed "
                "by the Node aggregation switch rather than summed, so the panel does not grow "
                "with the selection; at the default worst each line is its busiest node and the "
                "hover rows name it. QUIC is split by peer class; ADNL and RLDP2 remain "
                "all-peers. ADNL includes RLDP2 datagrams, so ADNL + RLDP2 double-counts."
            ),
        ),
        per_node_timeseries(
            "Nodes by wire throughput (adnl+quic)", line(BY_NODE, tagged_wire(NODE_KEYS)),
            unit="Bps", id=8, w=24, fill=6, legend=table_legend("lastNotNull"),
            description=(
                "Busiest nodes by available ADNL+QUIC UDP bytes (in+out). A target remains visible "
                "when either collector family is absent."
            ),
        ),
        net_agg_timeseries(
            "Datagrams per syscall",
            datagrams_per_syscall("quic"), datagrams_per_syscall("adnl"),
            unit="short", id=10,
            description=(
                "Wire packets per UDP send/receive syscall, per node and then collapsed by the "
                "Node aggregation switch: 1.0 means no batching and higher is cheaper, so the "
                "switch runs the other way — worst is the fleet minimum, and the hover rows name "
                "the node batching worst. A direction disappears when it has no syscalls instead "
                "of producing "
                "an undefined ratio."
            ),
        ),
        net_agg_timeseries(
            "Wire / app bytes (overhead factor, log scale)",
            node_share("quic {{direction}}", "ton_quic_wire_bytes_total",
                       "ton_quic_app_bytes_total", "direction"),
            agg_line(ratio(net_rate("ton_adnl_wire_bytes_total", f"{NODE_KEYS}, direction"),
                           summed(f"{NODE_KEYS}, direction", adnl_stack_bytes())),
                     key="direction", name="adnl stack {{direction}}"),
            node_share("rldp2 over adnl {{direction}}", "ton_rldp2_wire_bytes_total",
                       "ton_rldp2_app_bytes_total", "direction"),
            unit="short", id=11, min=None, log=10,  # a log axis cannot start at zero
            legend=table_legend("lastNotNull"),
            description=(
                "Wire bytes per delivered payload byte, per node and then collapsed by the Node "
                "aggregation switch (worst node per direction by default). QUIC "
                "compares its own tiers; ADNL stack replaces rldp2.messagePart transfer data with "
                "RLDP2-delivered payload; RLDP2 alone exposes FEC, retransmission, and "
                "failed-transfer overhead. Idle transports disappear. The peer-class filter does "
                "not apply because wire counters are all-peers. Log scale."
            ),
        ),
    ]),
    row("Workload composition", id=45, collapsed=True, panels=[
        chain_timeseries(
            "App traffic by kind, count", *app_traffic("app_messages_total"),
            scope="every selected node", unit="ops", id=28, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Messages, queries, and answers/s at each transport's app boundary, summed across "
                "selected nodes. QUIC is split by peer class; ADNL and RLDP2 remain all-peers. "
                "Query and answer rates should track; a growing gap means unanswered queries."
            ),
        ),
        chain_timeseries(
            "App traffic by kind, bytes", *app_traffic("app_bytes_total"),
            scope="every selected node", unit="Bps", id=29, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "App payload bytes/s for the same events, summed across selected nodes. QUIC is "
                "split by peer class. Compare with counts for size per item: answers usually "
                "dominate bytes while queries dominate count on serving nodes."
            ),
        ),
        chain_timeseries(
            "Top payload types, inbound", *top_payloads("in"),
            scope="every selected node", unit="Bps", id=24, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Inbound app payload bytes/s by resolved TL constructor. QUIC is split by peer "
                "class; ADNL remains all-peers. Routing envelopes are unwrapped. Fleet sum; "
                "rank-stable top 6 per transport."
            ),
        ),
        chain_timeseries(
            "Top payload types, outbound", *top_payloads("out"),
            scope="every selected node", unit="Bps", id=25, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Outbound app payload bytes/s by resolved TL constructor. QUIC is split by peer "
                "class; ADNL remains all-peers. Broadcast FEC parts stay labeled as FEC "
                "constructors. Fleet sum; rank-stable top 6 per transport."
            ),
        ),
        chain_timeseries(
            "Top inbound QUIC queries by count",
            line("quic {{trust}} {{tl}}",
                 net_rate("ton_quic_app_messages_total", "trust, tl", trusted=True,
                          kind="query", direction="in")
                 + busiest("ton_quic_app_messages_total", n=10, trusted=True,
                           kind="query", direction="in")),
            scope="every selected node", unit="reqps", id=26, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Inbound QUIC queries/s by peer class and resolved TL constructor. The "
                "all-transport processing and failure panels remain above. Fleet sum; rank-stable "
                "top 10."
            ),
        ),
        chain_timeseries(
            "Top outbound queries by count",
            line("quic {{trust}} {{tl}}",
                 net_rate("ton_quic_query_roundtrip_seconds_count", "trust, tl", trusted=True)
                 + busiest("ton_quic_query_roundtrip_seconds_count", n=5, trusted=True)),
            outbound_queries("adnl"), outbound_queries("rldp2"),
            scope="every selected node", unit="reqps", id=27, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Outbound queries/s by type and transport, derived from roundtrip histogram "
                "counts. QUIC is split by peer class; ADNL and RLDP2 remain all-peers. Fleet sum; "
                "rank-stable top 5 per transport."
            ),
        ),
        chain_timeseries(
            "Broadcast content by type, count", *broadcast("messages"),
            scope="every selected node", unit="ops", id=30, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Overlay broadcast content after FEC reassembly, summed across selected nodes and "
                "ranked as a stable top 6 per direction. Inbound includes a node's own broadcasts; "
                "outbound is counted pre-FEC at send entry points."
            ),
        ),
        chain_timeseries(
            "Broadcast content by type, bytes", *broadcast("bytes"),
            scope="every selected node", unit="Bps", id=31, h=9,
            legend=table_legend("lastNotNull"),
            description=(
                "Broadcast content bytes/s, not wire bytes. Transport FEC-part bytes minus this "
                "signal the redundancy and duplicate-reception tax. Fleet sum; rank-stable top 6 "
                "per direction; inbound includes a node's own broadcasts."
            ),
        ),
    ]),
    row("RLDP2 bulk transfers", id=46, collapsed=True, panels=[
        chain_timeseries(
            "RLDP2 latency p95",
            line("query roundtrip p95", quantile("ton_rldp2_query_roundtrip_seconds")),
            line("message delivery p95", quantile("ton_rldp2_message_delivery_seconds")),
            scope="every selected node", unit="s", id=50,
            description=(
                "Outbound RLDP2 query roundtrip and message-delivery p95 from fleet-summed "
                "histogram buckets. RLDP2 carries bulk transfers, so compare it separately from "
                "low-latency ADNL/QUIC. No data is normal when RLDP2 is idle. Delivery covers only "
                "sends carrying a timeout."
            ),
        ),
        net_agg_timeseries(
            "RLDP2 transfer failures",
            agg_line(transfer_failure_share(), key="direction, state"),
            line("completed {{direction}}",
                 summed("direction", transfer_rate(state="completed"))),
            unit="percentunit", id=52,
            axis_label="failure share", thresholds=failure_bands(),
            legend=table_legend("lastNotNull", "max", sort="Max"),
            styles=[series_style("^completed ", regex=True, unit="ops", axis="right",
                                 axis_label="completed/s", width=1, fill=0, bands="off")],
            description=(
                "Failed and timed-out whole RLDP2 transfers as a share of all terminal outcomes, "
                "split by direction, per node and then collapsed by the Node aggregation switch — "
                "at the default worst, the hover rows name that node. "
                "Completed transfers/s stays visible on the right axis as fleet activity context "
                "without flattening the failure signal. Yellow at 1%; red at 5%."
            ),
        ),
        chain_timeseries(
            "RLDP2 wire activity",
            line("wire packets {{direction}}",
                 net_rate("ton_rldp2_wire_packets_total", "direction")),
            scope="every selected node", unit="pps", id=51, w=24,
            description=(
                "RLDP2 datagrams/s exchanged with ADNL across the fleet. A flat zero proves the "
                "metric is present while bulk transfer is idle. Transfer outcomes use their own "
                "panel and scale above."
            ),
        ),
    ]),
]


def build():
    return dashboard(SELF, "TON Network", ["ton", "quic", "network"],
                     VARIABLES, ROWS, refresh="30s", version=1, time=("now-3h", "now"))
