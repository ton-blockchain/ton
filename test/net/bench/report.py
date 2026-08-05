#!/usr/bin/env python3
"""Report one steady-state window from bench.sh."""

import argparse
import re
from pathlib import Path


WIRE = {"quic": "quic", "rldp2": "adnl", "adnl": "adnl"}
NATIVE_FAMILIES = (
    "bench_connections_current",
    "bench_messages_sent_total",
    "bench_messages_received_total",
    "bench_queries_sent_total",
    "bench_queries_completed_total",
    "bench_errors_total",
    "bench_bytes_sent_total",
    "bench_bytes_received_total",
    "bench_connections_ready",
    "bench_connection_first_pass_seconds",
    "bench_connection_setup_seconds",
    "bench_connection_setup_reconnects_total",
)
COLUMNS = (
    "label", "implementation", "protocol", "workload", "topology", "request_bytes", "response_bytes",
    "generators", "connections_per_generator", "burst", "inflight", "threads", "tuned",
    "congestion_control",
    "warmup_seconds", "window_seconds", "git_revision", "binary_sha256",
    "target_per_s", "messages_per_s", "submitted_per_s", "pacing_pct", "delivery_pct", "app_mib_per_s",
    "cpu_us_per_message", "egress_datagrams_per_message", "egress_syscalls_per_message",
    "wire_bytes_per_message", "segments_per_descriptor", "descriptors_per_send", "datagrams_per_send",
    "packets_per_nonempty_flush", "flushes_gt16_pct", "target_peers", "active_peers_start",
    "active_peers_end", "peer_active_peers_start", "peer_active_peers_end", "rss_kib_per_peer",
    "cpu_cores", "rss_mib", "rss_growth_mib", "peer_cpu_cores", "peer_rss_mib",
    "peer_rss_growth_mib", "wire_packets_per_s", "wire_bytes_per_s", "wire_syscalls_per_s",
    "first_pass_seconds", "first_pass_connections_per_s", "setup_seconds", "setup_connections_per_s",
    "setup_reconnects", "drops", "failures",
    "driver_errors", "status",
)


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--implementation", choices=("ton", "quic-go", "quinn"), required=True)
    parser.add_argument("--protocol", choices=("quic", "rldp2", "adnl"), required=True)
    parser.add_argument("--workload", choices=("query", "message", "saturate", "idle"), required=True)
    parser.add_argument("--topology", required=True)
    parser.add_argument("--side", choices=("sender", "receiver"), required=True)
    parser.add_argument("--peers", type=int, required=True)
    parser.add_argument("--clients", type=int, required=True)
    parser.add_argument("--connections", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--response-size", type=int, required=True)
    parser.add_argument("--rate", type=float, default=0)
    parser.add_argument("--burst", type=int, required=True)
    parser.add_argument("--inflight", type=int, required=True)
    parser.add_argument("--threads", type=int, required=True)
    parser.add_argument("--tuned", choices=("NA", "0", "1"), required=True)
    parser.add_argument("--congestion-control", choices=("NA", "cubic", "bbr", "reno"), required=True)
    parser.add_argument("--warmup", type=float, required=True)
    parser.add_argument("--git-revision", required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--elapsed", type=float, required=True)
    parser.add_argument("--metrics-elapsed", type=float, required=True)
    parser.add_argument("--peer-metrics-elapsed", type=float, required=True)
    parser.add_argument("--cpu-start", type=float, required=True)
    parser.add_argument("--cpu-end", type=float, required=True)
    parser.add_argument("--peer-cpu-start", type=float, required=True)
    parser.add_argument("--peer-cpu-end", type=float, required=True)
    parser.add_argument("--peer-rss-start", type=float, required=True, help="KiB")
    parser.add_argument("--peer-rss-end", type=float, required=True, help="KiB")
    parser.add_argument("--rss-baseline", type=float, required=True, help="KiB before peers connect")
    parser.add_argument("--rss-start", type=float, required=True, help="KiB")
    parser.add_argument("--rss-end", type=float, required=True, help="KiB")
    parser.add_argument("--driver-errors", type=int, required=True)
    parser.add_argument("--label", required=True)
    return parser.parse_args()


def parse(path: Path):
    result = {}
    for line in path.read_text().splitlines():
        match = re.match(r"^(\S+?)(\{.*\})?\s+([\d.eE+-]+)$", line)
        if match:
            key = (match.group(1), match.group(2) or "")
            result[key] = result.get(key, 0.0) + float(match.group(3))
    return result


def total(sample, name, *labels):
    return sum(value for (metric, label_set), value in sample.items()
               if metric == name and all(label in label_set for label in labels))


def delta(start, end, name, *labels):
    return total(end, name, *labels) - total(start, name, *labels)


def app_events(start, end, app, direction, kind):
    return delta(start, end, f"{app}_messages_total", f'direction="{direction}"', f'kind="{kind}"')


def has_family(sample, name):
    return any(metric == name for metric, _ in sample)


def active_peers(sample, implementation, protocol):
    if implementation != "ton":
        return total(sample, "bench_connections_current")
    metric = {
        "quic": "ton_quic_transport_connections_current",
        "rldp2": "ton_rldp2_transport_connections",
        "adnl": "ton_adnl_transport_peer_pairs",
    }[protocol]
    return total(sample, metric)


def value_or_na(value, precision):
    return "NA" if value is None else f"{value:.{precision}f}"


def main():
    args = arguments()
    if args.implementation != "ton" and args.protocol != "quic":
        raise SystemExit("native implementations only support QUIC")

    directory = Path(args.dir)
    start = parse(directory / "metrics-start.txt")
    end = parse(directory / "metrics-end.txt")
    peer_start = parse(directory / "peer-metrics-start.txt")
    peer_end = parse(directory / "peer-metrics-end.txt")
    native = args.implementation != "ton"
    if args.side == "sender":
        client_start, client_end, client_elapsed = start, end, args.metrics_elapsed
        server_start, server_end, server_elapsed = peer_start, peer_end, args.peer_metrics_elapsed
    else:
        client_start, client_end, client_elapsed = peer_start, peer_end, args.peer_metrics_elapsed
        server_start, server_end, server_elapsed = start, end, args.metrics_elapsed

    missing = []

    def require(scope, samples, *families):
        for family in families:
            if any(not has_family(sample, family) for sample in samples):
                missing.append(f"{scope}:{family}")

    submitted = delivered = successful = 0.0
    drops = None if native else 0.0
    drop_details = []
    failures = 0.0
    rows = None

    if native:
        require("test", (start, end), *NATIVE_FAMILIES)
        require("peer", (peer_start, peer_end), *NATIVE_FAMILIES)
        if args.workload in ("message", "saturate"):
            submitted = delta(client_start, client_end, "bench_messages_sent_total")
            delivered = delta(server_start, server_end, "bench_messages_received_total")
        elif args.workload == "query":
            submitted = delta(client_start, client_end, "bench_queries_sent_total")
            successful = delta(client_start, client_end, "bench_queries_completed_total")
        for scope_start, scope_end in ((start, end), (peer_start, peer_end)):
            failures += delta(scope_start, scope_end, "bench_errors_total")
    else:
        app = f"ton_{args.protocol}_app"
        wire = f"ton_{WIRE[args.protocol]}_wire"
        require("test", (start, end), *(f"{wire}_{name}_total" for name in ("packets", "bytes", "syscalls")))
        kind = "message" if args.workload in ("message", "saturate") else "query"
        if args.workload in ("message", "saturate"):
            require("client", (client_start, client_end), f"{app}_messages_total")
            require("server", (server_start, server_end), f"{app}_messages_total")
            if args.protocol == "quic":
                require("client", (client_start, client_end), "ton_quic_message_confirmation_failed_total")
            elif args.protocol == "rldp2":
                require("client", (client_start, client_end), "ton_rldp2_message_delivery_failed_total")
        elif args.workload == "query":
            require("client", (client_start, client_end),
                    f"ton_{args.protocol}_query_roundtrip_seconds_count",
                    f"ton_{args.protocol}_query_roundtrip_failed_total")
        else:
            peer_metric = {
                "quic": "ton_quic_transport_connections_current",
                "rldp2": "ton_rldp2_transport_connections",
                "adnl": "ton_adnl_transport_peer_pairs",
            }[args.protocol]
            require("test", (start, end), peer_metric)
            require("peer", (peer_start, peer_end), peer_metric)

        submitted = app_events(client_start, client_end, app, "out", kind)
        delivered = app_events(server_start, server_end, app, "in", kind)
        rows = {}
        for io in ("in", "out"):
            label = f'direction="{io}"'
            rows[io] = {
                "packets": delta(start, end, f"{wire}_packets_total", label),
                "bytes": delta(start, end, f"{wire}_bytes_total", label),
                "syscalls": delta(start, end, f"{wire}_syscalls_total", label),
            }

        drop_families = ((f"{wire}_dropped_total", "wire"),
                         (f"ton_{args.protocol}_transport_dropped_total", "transport"),
                         (f"{app}_dropped_total", "app"))
        if args.protocol == "rldp2":
            drop_families += (("ton_adnl_transport_dropped_total", "adnl-transport"),
                              ("ton_adnl_app_dropped_total", "adnl-app"))
        require("test", (start, end), *(family for family, _ in drop_families))
        require("peer", (peer_start, peer_end), *(family for family, _ in drop_families))
        for scope, scope_start, scope_end in (("test", start, end), ("peer", peer_start, peer_end)):
            for family, label in drop_families:
                value = delta(scope_start, scope_end, family)
                if value:
                    drops += value
                    drop_details.append(f"{scope}-{label}={value:.0f}")

        for scope_start, scope_end in ((start, end), (peer_start, peer_end)):
            failures += delta(scope_start, scope_end, f"ton_{args.protocol}_query_roundtrip_failed_total")
            if args.protocol == "quic":
                failures += delta(scope_start, scope_end, "ton_quic_message_confirmation_failed_total")
            elif args.protocol == "rldp2":
                failures += delta(scope_start, scope_end, "ton_rldp2_message_delivery_failed_total")

    cpu = args.cpu_end - args.cpu_start
    peer_cpu = args.peer_cpu_end - args.peer_cpu_start
    rss_per_peer = 0.0
    invalid = (failures != 0 or args.driver_errors != 0 or bool(missing) or args.elapsed <= 0 or
               args.metrics_elapsed <= 0 or args.peer_metrics_elapsed <= 0 or
               (drops is not None and drops != 0))
    messages_per_s = app_mib_per_s = cpu_per_message = 0.0
    submitted_per_s = pacing_pct = delivery_pct = 0.0
    egress_datagrams_per_message = None if native else 0.0
    egress_syscalls_per_message = None if native else 0.0
    wire_per_message = None if native else 0.0
    segments_per_descriptor = None
    descriptors_per_send = None
    datagrams_per_send = None
    packets_per_nonempty_flush = None
    flushes_gt16_pct = None

    rate = lambda value, seconds: value / seconds if seconds > 0 else 0.0
    cpu_cores = rate(cpu, args.elapsed)
    peer_cpu_cores = rate(peer_cpu, args.elapsed)
    rss_mib = args.rss_end / 1024
    rss_growth_mib = (args.rss_end - args.rss_start) / 1024
    peer_rss_mib = args.peer_rss_end / 1024
    peer_rss_growth_mib = (args.peer_rss_end - args.peer_rss_start) / 1024
    wire_packets_per_s = wire_bytes_per_s = wire_syscalls_per_s = None
    if rows is not None:
        wire_packets_per_s = rate(rows["in"]["packets"] + rows["out"]["packets"], args.metrics_elapsed)
        wire_bytes_per_s = rate(rows["in"]["bytes"] + rows["out"]["bytes"], args.metrics_elapsed)
        wire_syscalls_per_s = rate(rows["in"]["syscalls"] + rows["out"]["syscalls"], args.metrics_elapsed)

    connection_family = ("bench_connections_current" if native else {
        "quic": "ton_quic_transport_connections_current",
        "rldp2": "ton_rldp2_transport_connections",
        "adnl": "ton_adnl_transport_peer_pairs",
    }[args.protocol])
    if not native:
        require("test", (start, end), connection_family)
        require("peer", (peer_start, peer_end), connection_family)
    active_start = active_peers(start, args.implementation, args.protocol)
    active_end = active_peers(end, args.implementation, args.protocol)
    peer_active_start = active_peers(peer_start, args.implementation, args.protocol)
    peer_active_end = active_peers(peer_end, args.implementation, args.protocol)
    invalid |= any(value != args.peers for value in
                   (active_start, active_end, peer_active_start, peer_active_end))

    first_pass_seconds = first_pass_per_s = None
    setup_seconds = setup_per_s = setup_reconnects = None
    if native and args.clients == 1 and args.workload == "idle":
        first_pass_seconds = total(client_start, "bench_connection_first_pass_seconds")
        first_pass_per_s = args.peers / first_pass_seconds if first_pass_seconds else None
        setup_seconds = total(client_start, "bench_connection_setup_seconds")
        setup_per_s = args.peers / setup_seconds if setup_seconds else None
        setup_reconnects = total(client_start, "bench_connection_setup_reconnects_total")
        print(f"  setup        {first_pass_seconds:.3f}s first pass ({first_pass_per_s:.0f}/s); "
              f"{setup_seconds:.3f}s ready ({setup_per_s:.0f}/s), "
              f"{setup_reconnects:.0f} pre-readiness reconnects")

    print(f"  window       {args.elapsed:.2f}s CPU; {args.metrics_elapsed:.2f}/{args.peer_metrics_elapsed:.2f}s "
          "test/peer metrics after warm-up")
    if args.workload == "idle":
        rss_per_peer = (args.rss_end - args.rss_baseline) / active_end if active_end else 0.0
        if native:
            print(f"  idle         {active_start:.0f}->{active_end:.0f} / "
                  f"{peer_active_start:.0f}->{peer_active_end:.0f} active test/peer; transport I/O N/A")
        else:
            print(f"  idle         {active_start:.0f}->{active_end:.0f} / "
                  f"{peer_active_start:.0f}->{peer_active_end:.0f} active test/peer; "
                  f"{wire_packets_per_s:.1f} dgram/s; {wire_bytes_per_s:.1f} B/s; "
                  f"{wire_syscalls_per_s:.1f} syscall/s")
        print(f"  resources    {cpu_cores:.3f} test + {peer_cpu_cores:.3f} peer cores; "
              f"{rss_mib:.1f} MiB RSS; {rss_per_peer:.1f} KiB/peer retained; "
              f"{rss_growth_mib:+.1f} MiB/window; {peer_rss_mib:.1f} MiB peer RSS")
    else:
        if args.workload in ("message", "saturate"):
            submitted_per_s = rate(submitted, client_elapsed)
            messages_per_s = rate(delivered, server_elapsed)
            requested_per_s = args.rate * args.clients
            pacing_pct = 100 * submitted_per_s / requested_per_s if requested_per_s else 0.0
            delivery_pct = 100 * messages_per_s / submitted_per_s if submitted_per_s else 0.0
            invalid |= messages_per_s < 0.99 * submitted_per_s
            if args.workload == "message":
                invalid |= submitted_per_s < 0.9 * requested_per_s
        else:
            if not native:
                roundtrips = delta(client_start, client_end,
                                   f"ton_{args.protocol}_query_roundtrip_seconds_count")
                client_failures = delta(client_start, client_end,
                                        f"ton_{args.protocol}_query_roundtrip_failed_total")
                successful = roundtrips - client_failures
            submitted_per_s = rate(submitted, client_elapsed)
            delivery_pct = 100 * successful / submitted if submitted else 0.0
            invalid |= successful <= 0
            messages_per_s = rate(successful, client_elapsed)

        app_bytes = args.size + (args.response_size if args.workload == "query" else 0)
        app_mib_per_s = messages_per_s * app_bytes / (1024 * 1024)
        cpu_per_message = rate(cpu, args.elapsed) / messages_per_s * 1e6 if messages_per_s else 0.0
        if not native:
            wire_bytes = rows["in"]["bytes"] + rows["out"]["bytes"]
            per_message = lambda value: rate(value, args.metrics_elapsed) / messages_per_s if messages_per_s else 0.0
            egress_datagrams_per_message = per_message(rows["out"]["packets"])
            egress_syscalls_per_message = per_message(rows["out"]["syscalls"])
            wire_per_message = per_message(wire_bytes)
        invalid |= messages_per_s <= 0
        if args.workload in ("message", "saturate"):
            logical = f"; {messages_per_s / args.peers:.0f} broadcast/s" if args.topology.startswith("fanout-") else ""
            print(f"  application  {submitted_per_s:.0f}/s submitted ({pacing_pct:.1f}% pacing); "
                  f"{messages_per_s:.0f}/s delivered ({delivery_pct:.1f}% delivery{logical})")
        else:
            print(f"  application  {successful:.0f} successful round trips "
                  f"({messages_per_s:.0f}/s, {app_mib_per_s:.1f} MiB/s); "
                  f"{submitted_per_s:.0f}/s submitted ({delivery_pct:.1f}% completion)")
        wire_text = "N/A" if wire_per_message is None else f"{wire_per_message:.0f} wire B/msg"
        print(f"  cost         {cpu_per_message:.2f} us/msg under test; "
              f"{rate(peer_cpu, args.elapsed) / messages_per_s * 1e6 if messages_per_s else 0:.2f} us/msg peers; "
              f"{wire_text}")
        if native:
            print("  egress       N/A (native exporter has no wire/syscall counters)")
        else:
            print(f"  egress       {egress_datagrams_per_message:.3f} dgram/msg; "
                  f"{egress_syscalls_per_message:.3f} syscall/msg")

    if args.implementation == "ton" and args.protocol == "quic":
        prefix = "ton_quic_batching_egress_"
        require("test", (start, end),
                f"{prefix}flush_packets_count", f"{prefix}flush_packets_sum", f"{prefix}flush_packets_bucket",
                f"{prefix}gso_segments_count", f"{prefix}gso_segments_sum",
                f"{prefix}syscall_messages_count", f"{prefix}syscall_messages_sum")
        flush_count = delta(start, end, f"{prefix}flush_packets_count")
        zero_flushes = delta(start, end, f"{prefix}flush_packets_bucket", 'le="0"')
        nonempty_flushes = flush_count - zero_flushes
        flush_packets = delta(start, end, f"{prefix}flush_packets_sum")
        at_most_16 = delta(start, end, f"{prefix}flush_packets_bucket", 'le="16"')
        packets_per_nonempty_flush = flush_packets / nonempty_flushes if nonempty_flushes else 0.0
        flushes_gt16_pct = 100 * (flush_count - at_most_16) / nonempty_flushes if nonempty_flushes else 0.0

        send_count = delta(start, end, f"{prefix}syscall_messages_count")
        descriptors = delta(start, end, f"{prefix}syscall_messages_sum")
        segment_count = delta(start, end, f"{prefix}gso_segments_count")
        segments = delta(start, end, f"{prefix}gso_segments_sum")
        segments_per_descriptor = segments / segment_count if segment_count else 0.0
        descriptors_per_send = descriptors / send_count if send_count else 0.0
        datagrams_per_send = segments / send_count if send_count else 0.0
        print(f"  batching     {packets_per_nonempty_flush:.2f} pkt/nonempty flush; "
              f"{flushes_gt16_pct:.1f}% flushes >16 pkt; {segments_per_descriptor:.2f} segment/descriptor; "
              f"{descriptors_per_send:.2f} descriptor/send; {datagrams_per_send:.2f} dgram/send")

    invalid |= bool(missing)
    if missing:
        print(f"  metrics      missing {', '.join(missing)}")
    print(f"  drops        {', '.join(drop_details) if drop_details else ('N/A' if drops is None else 'none')}")
    if failures:
        print(f"  failures     {failures:.0f} implementation-reported errors")
    if args.driver_errors:
        print(f"  driver       {args.driver_errors} query errors")
    print(f"  status       {'INVALID' if invalid else 'ok'}")

    row = (
        args.label, args.implementation, args.protocol, args.workload, args.topology, str(args.size),
        str(args.response_size), str(args.clients), str(args.connections),
        str(args.burst) if args.workload == "message" else "NA",
        str(args.inflight) if args.workload == "query" else "NA", str(args.threads), args.tuned,
        args.congestion_control,
        f"{args.warmup:.3f}", f"{args.elapsed:.3f}", args.git_revision, args.binary_sha256,
        value_or_na(args.rate * args.clients if args.workload == "message" else None, 0),
        f"{messages_per_s:.0f}", f"{submitted_per_s:.0f}", f"{pacing_pct:.2f}",
        f"{delivery_pct:.2f}", f"{app_mib_per_s:.3f}", f"{cpu_per_message:.3f}",
        value_or_na(egress_datagrams_per_message, 4), value_or_na(egress_syscalls_per_message, 4),
        value_or_na(wire_per_message, 1), value_or_na(segments_per_descriptor, 3),
        value_or_na(descriptors_per_send, 3), value_or_na(datagrams_per_send, 3),
        value_or_na(packets_per_nonempty_flush, 3), value_or_na(flushes_gt16_pct, 2),
        str(args.peers), f"{active_start:.0f}", f"{active_end:.0f}", f"{peer_active_start:.0f}",
        f"{peer_active_end:.0f}", f"{rss_per_peer:.1f}", f"{cpu_cores:.4f}", f"{rss_mib:.1f}",
        f"{rss_growth_mib:.1f}", f"{peer_cpu_cores:.4f}", f"{peer_rss_mib:.1f}",
        f"{peer_rss_growth_mib:.1f}", value_or_na(wire_packets_per_s, 1),
        value_or_na(wire_bytes_per_s, 1), value_or_na(wire_syscalls_per_s, 1),
        value_or_na(first_pass_seconds, 3), value_or_na(first_pass_per_s, 1),
        value_or_na(setup_seconds, 3), value_or_na(setup_per_s, 1), value_or_na(setup_reconnects, 0),
        value_or_na(drops, 0), f"{failures:.0f}", f"{args.driver_errors}",
        "INVALID" if invalid else "ok",
    )
    assert len(row) == len(COLUMNS)
    (directory / "summary.tsv").write_text("\t".join(COLUMNS) + "\n" + "\t".join(row) + "\n")
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
