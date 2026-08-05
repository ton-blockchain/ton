#!/usr/bin/env python3
"""Compact human summary of a suite summary.tsv: medians across repetitions, key columns only."""
import collections
import csv
import re
import statistics
import sys


def scenario_of(row):
    """The label minus the implementation/protocol/repetition suffixes it may carry."""
    label = row["label"]
    suffix = f"-{row['implementation']}-{row['protocol']}"
    if label.endswith(suffix):
        label = label[: -len(suffix)]
    return re.sub(r"-r\d+$", "", label)


def median(rows, field):
    values = []
    for row in rows:
        try:
            values.append(float(row[field]))
        except (KeyError, ValueError):
            pass
    return statistics.median(values) if values else None


def fmt_rate(value):
    if value is None:
        return "-"
    return f"{value / 1000:.1f}k" if value >= 10000 else f"{value:.0f}"


def fmt(value, spec=".2f"):
    return "-" if value is None else format(value, spec)


def main():
    rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
    groups = collections.defaultdict(list)
    order = []
    for row in rows:
        key = (scenario_of(row), row["implementation"], row["protocol"])
        if key not in groups:
            order.append(key)
        groups[key].append(row)

    def status_of(group):
        if all(r.get("status") == "ok" for r in group):
            return "ok"
        return f"{sum(r.get('status') == 'ok' for r in group)}/{len(group)} ok"

    def emit(table):
        widths = [max(len(row[i]) for row in table) for i in range(len(table[0]))]
        for row in table:
            print("  ".join(cell.ljust(width) for cell, width in zip(row, widths)).rstrip())

    traffic = [("scenario", "impl", "proto", "delivered/s", "app MiB/s", "cpu us/msg",
                "peer cores", "dgram/msg", "wire B/msg", "status")]
    idle = [("scenario", "impl", "proto", "peers", "cores", "RSS MiB", "KiB/peer",
             "wire dgram/s", "status")]
    for key in order:
        scenario, impl, proto = key
        group = groups[key]
        if group[0].get("workload") == "idle":
            idle.append((scenario, impl, proto,
                         fmt(median(group, "target_peers"), ".0f"),
                         fmt(median(group, "cpu_cores"), ".4f"),
                         fmt(median(group, "rss_mib"), ".1f"),
                         fmt(median(group, "rss_kib_per_peer"), ".1f"),
                         fmt(median(group, "wire_packets_per_s"), ".1f"),
                         status_of(group)))
            continue
        traffic.append((scenario, impl, proto,
                        fmt_rate(median(group, "messages_per_s")),
                        fmt(median(group, "app_mib_per_s"), ".1f"),
                        fmt(median(group, "cpu_us_per_message")),
                        fmt(median(group, "peer_cpu_cores")),
                        fmt(median(group, "egress_datagrams_per_message"), ".3f"),
                        fmt(median(group, "wire_bytes_per_message"), ".0f"),
                        status_of(group)))

    if len(traffic) > 1:
        emit(traffic)
    if len(idle) > 1:
        if len(traffic) > 1:
            print()
        print("idle (established connections doing nothing; cores and RSS are the result)")
        emit(idle)


if __name__ == "__main__":
    main()
