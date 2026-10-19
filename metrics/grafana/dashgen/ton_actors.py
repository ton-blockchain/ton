"""TON Actor Framework: scheduler occupancy, per-class workload, perf counters."""

from .lib import (
    SEL,
    TICKS_TO_SECONDS,
    WARN,
    agg_line,
    agg_timeseries,
    agg_variable,
    dashboard,
    gauge,
    line,
    per_node_timeseries,
    plain_stat,
    rate,
    ratio,
    recent_max,
    row,
    standard_variables,
    tail_max_panel,
    threshold_lines,
    thresholds,
    top_peak,
    top_total,
    variable_custom,
    worker_occupancy,
    worst_node_stat,
)


def occupancy_by_kind():
    """The same ratio per (node, worker kind); here the tick conversion follows the sum."""
    by, kinds = "job, instance, worker", "~cpu|io"
    busy = rate("ton_actor_worker_busy_ticks_total", by, worker=kinds)
    threads = gauge("ton_actor_worker_threads", by, agg="max", worker=kinds)
    return f"max by ({by}) (({busy} / {TICKS_TO_SECONDS}) / on ({by}) clamp_min({threads}, 1))"


# Perf charts carry up to 50 thin series, so they drop the fill and move the legend aside.
PERF_TOP = "${perf_top:raw}"
PERF_LOOK = {
    "fieldConfig": {"defaults": {"min": 0, "custom": {"lineWidth": 1, "fillOpacity": 0}}},
    "options": {"legend": {"placement": "right"}},
}


def perf_timeseries(title, inner, *, top, unit, description, id):
    """A TD_PERF_COUNTER chart: full width, capped at the $perf_top costliest operations."""
    return agg_timeseries(
        f"{title} — top ${{perf_top:text}}", agg_line(inner, key="op", top=top),
        unit=unit, description=description, id=id, w=24, h=13, overrides=PERF_LOOK,
    )


VARIABLES = [
    *standard_variables(),
    agg_variable(),
    variable_custom(
        "perf_top", label="Perf series", choices=["8", "12", "20", "50"], selected="50",
        description=(
            "Number of TD_PERF_COUNTER operations shown in each perf chart. Use 50 to include up "
            "to 50 registered operations."
        ),
    ),
]

ROWS = [
    row("Actor health", id=200, panels=[
        worst_node_stat(
            "Actor CPU busy", worker_occupancy("cpu"), id=1,
            unit="percentunit", decimals=1,
            thresholds=thresholds(("yellow", 0.5), ("red", 0.8)),
            description=(
                "Worst selected node's CPU-worker occupancy from actor and coroutine dispatches, "
                "including elapsed time in the current dispatch. The outer dispatch scope includes "
                "signal-only work, gaps between handlers and finish bookkeeping. This is elapsed "
                "scheduler occupancy, not OS CPU; blocking inside actor code is included."
            ),
        ),
        worst_node_stat(
            "Actor IO busy", worker_occupancy("io"), id=5,
            unit="percentunit", decimals=1,
            thresholds=thresholds(("yellow", 0.5), ("red", 0.8)),
            description=(
                "Worst selected node's IO-worker occupancy from actor dispatches, including "
                "elapsed time in the current dispatch. The outer dispatch scope includes "
                "signal-only work, gaps between handlers and finish bookkeeping. Each scheduler "
                "has one IO worker, so sustained pressure means moving work off polling actors "
                "rather than adding CPU workers."
            ),
        ),
        worst_node_stat(
            "Local run queue", gauge("ton_actor_scheduler_local_queue_length", "job, instance"),
            id=18, unit="short",
            thresholds=thresholds(("yellow", 1), ("red", 10)),
            description=(
                "Largest selected target's visible runnable backlog, summed over its "
                "scheduler-local CPU queues. This is a lower bound because shared CPU and IO "
                "queues are not exposed. Nonzero on most scrapes indicates sustained CPU-worker "
                "saturation; an isolated sample can be transient."
            ),
        ),
        worst_node_stat(
            "Longest running handler",
            gauge("ton_actor_scheduler_current_execute_seconds", "job, instance, actor", agg="max"),
            id=19, name_labels=("instance", "actor"), unit="s", decimals=2,
            thresholds=thresholds(("yellow", 1), ("red", 5)),
            description=(
                "Point sample of the longest actor or coroutine dispatch currently in flight on "
                "any selected scheduler; the collecting worker is excluded. Yellow starts at 1 s "
                "and red at 5 s. The colors are diagnostic guides; treat red as a wedge only when "
                "it persists for several scrapes because a short spike can be a legitimate large "
                "actor batch or coroutine resumption."
            ),
        ),
        worst_node_stat(
            "Max queue delay",
            recent_max("ton_actor_max_queue_ticks", "job, instance, type"), id=20,
            name_labels=("instance", "type"), unit="s", decimals=3,
            thresholds=thresholds(("yellow", 0.1), ("red", 1)),
            description=(
                "Worst scheduling delay retained by two recent max buckets, converted from each "
                "target's raw ticks before taking the fleet maximum. The buckets are nominally 10 "
                "minutes at the assumed TSC rate; exact wall duration is platform-dependent. A "
                "spike cliff-drops when both buckets expire, which is a window artifact rather "
                "than necessarily an instantaneous recovery. Yellow starts at 100 ms and red at 1 "
                "s as diagnostic guides, not SLOs."
            ),
        ),
        worst_node_stat(
            "Max executor drain",
            recent_max("ton_actor_max_execute_ticks", "job, instance, type"), id=21,
            name_labels=("instance", "type"), unit="s", decimals=3,
            thresholds=thresholds(("yellow", 0.1), ("red", 1)),
            description=(
                "Worst timed executor drain retained by two recent max buckets. It covers "
                "signal/mailbox processing but excludes finish-time bookkeeping. The buckets are "
                "nominally 10 minutes at the assumed TSC rate; exact wall duration is "
                "platform-dependent. A spike cliff-drops when both buckets expire, which is a "
                "window artifact. Yellow starts at 100 ms and red at 1 s as diagnostic guides, not "
                "SLOs."
            ),
        ),
        worst_node_stat(
            "Peak messages/s", rate("ton_actor_messages_total", "job, instance"), id=2,
            unit="ops", decimals=0, thresholds=thresholds(base="blue"),
            description=(
                "Actor-message and bare-coroutine throughput on the busiest selected target. Fleet "
                "sums scale with node count; this card highlights a hot node instead."
            ),
        ),
        plain_stat(
            "Nodes without actor stats",
            f"(count(up{SEL} == 1) or vector(0))"
            f" - (count(ton_actor_stats_enabled{SEL} == 1) or vector(0))",
            id=6, scope="all selected targets", unit="short", sparkline=False,
            thresholds=thresholds(("red", 1)),
            description=(
                "Selected, scraped-up targets that do not export enabled actor statistics. This "
                "includes an explicit disabled gate and older builds without the actor tier. Zero "
                "is healthy. A down target drops out of both terms; use TON Overview for target "
                "reachability."
            ),
        ),
    ]),
    row("Scheduler pressure", id=201, panels=[
        per_node_timeseries(
            "Worker occupancy by node/kind",
            line("{{job}} {{instance}} {{worker}}", occupancy_by_kind()),
            id=7, unit="percentunit", thresholds=threshold_lines(0.5, 0.8),
            description=(
                "Per-target CPU/IO worker utilization: outer actor/coroutine dispatch time divided "
                "by available threads of that kind, then the worst target for each node/kind. 1.0 "
                "is 100% pool occupancy. IO near 1 is the sharper alarm because each scheduler has "
                "one unsplittable poll thread. This is scheduler-group occupancy, not OS CPU; one "
                "saturated scheduler can be diluted by idle siblings. Blocking, signal-only work, "
                "gaps between handlers and finish bookkeeping are included. The counter includes "
                "elapsed time in the current dispatch; Longest in-flight execution is the "
                "companion wedge signal. Values can briefly exceed 1 after a thread-count change "
                "because the numerator is windowed and the denominator is instantaneous."
            ),
        ),
        per_node_timeseries(
            "Local run-queue by node",
            line("{{job}} {{instance}}",
                 gauge("ton_actor_scheduler_local_queue_length", "job, instance")),
            id=9, unit="short", thresholds=threshold_lines(1, 10, warn_color=WARN),
            description=(
                "Runnable entries (actors with mail + resumable coroutines) in the per-cpu-worker "
                "work-stealing queues, summed per node. Lower bound on the backlog: the shared "
                "MPMC queue and the io queue expose no size, so work waiting there is invisible. "
                "Nonzero on most scrapes = cpu workers saturated."
            ),
        ),
        per_node_timeseries(
            "Longest in-flight execution by node",
            line("{{instance}}: {{actor}}",
                 gauge("ton_actor_scheduler_current_execute_seconds",
                       "job, instance, actor", agg="max")),
            id=17, unit="s", w=24, h=7, thresholds=threshold_lines(1, 5),
            description=(
                "Point sample at scrape: of the other workers currently inside an actor or "
                "coroutine dispatch, how long the longest-running one has been at it. Dispatches "
                "shorter than the scrape interval are never seen — this catches a wedged or very "
                "long turn, not a duty cycle. The collecting worker is excluded. Each series is "
                "labeled with the (id-stripped) name of the actor holding the worker at sample "
                "time — during a wedge the rising line names the stuck actor directly."
            ),
        ),
    ]),
    row("Actor workload", id=202, panels=[
        agg_timeseries(
            "Actor handler load",
            agg_line(rate("ton_actor_busy_ticks_total", "type, job, instance", per_tick=True),
                     key="type",
                     top=top_total("type", "ton_actor_busy_ticks_total", per_tick=True)),
            id=10, unit="short",
            description=(
                "Handler or coroutine-resumption seconds per second for each class, computed per "
                "node and then collapsed with Node aggregation. Actor and coroutine samples enter "
                "when the handler or resumption returns; live outer-dispatch time remains visible "
                "in the worker panels. Nodes without that class are absent rather than zero. "
                "Blocking and descheduling are included, but signal-only work, gaps and dispatch "
                "bookkeeping are not, so this does not sum to worker occupancy. Top 8 classes by "
                "total busy time over the visible range."
            ),
        ),
        agg_timeseries(
            "Actor messages/s",
            agg_line(rate("ton_actor_messages_total", "type, job, instance"), key="type",
                     top=top_total("type", "ton_actor_messages_total")),
            id=11, unit="ops",
            description=(
                "Actor-message or coroutine-resumption throughput for each class, computed per "
                "node and then collapsed with Node aggregation. Nodes without that class are "
                "absent rather than zero. Compare with handler load: high throughput plus low load "
                "means cheap, frequent work. Top 8 classes by total messages over the visible "
                "range."
            ),
        ),
        per_node_timeseries(
            "Messages/s by node/kind",
            line("{{job}} {{instance}} {{worker}}",
                 rate("ton_actor_worker_messages_total", "job, instance, worker")),
            id=8, unit="ops",
            description=(
                "Actor messages plus bare coroutine resumptions per second, split by node and "
                "io/cpu worker kind."
            ),
        ),
        agg_timeseries(
            "Handler time/message",
            agg_line(ratio(rate("ton_actor_busy_ticks_total", "type, job, instance", per_tick=True),
                           rate("ton_actor_messages_total", "type, job, instance")),
                     key="type",
                     top=top_total("type", "ton_actor_busy_ticks_total", per_tick=True)),
            id=16, unit="s", h=8,
            description=(
                "Handler seconds per message for each class, computed per node with traffic and "
                "then collapsed with Node aggregation. It includes blocking and descheduling but "
                "excludes signal-only work and outer dispatch bookkeeping, so it is not full "
                "worker cost. Top 8 classes by total busy time over the visible range; noisy for "
                "classes with few messages."
            ),
        ),
    ]),
    row("Tail & batching", id=203, panels=[
        tail_max_panel(
            "Max single message", "type",
            recent_max("ton_actor_max_message_ticks", "type, job, instance"),
            top=top_peak("type", "ton_actor_max_message_ticks", per_tick=True),
            id=12, unit="s", thresholds=threshold_lines(0.1, 1),
            description=(
                "Worst single message execution retained by two recent max buckets. Their nominal "
                "length is 10 minutes at the assumed TSC rate; exact wall duration is "
                "platform-dependent. Raw ticks are divided by each target's calibrated tick "
                "frequency before nodes are aggregated. Elapsed time includes blocking and "
                "descheduling; no percentiles are collected on the actor hot path. Diagnostic "
                "guides (not SLOs) are yellow at 100 ms and red at 1 s; the color marks a retained "
                "spike, not necessarily a problem still live."
            ),
        ),
        tail_max_panel(
            "Max queue delay", "type",
            recent_max("ton_actor_max_queue_ticks", "type, job, instance"),
            top=top_peak("type", "ton_actor_max_queue_ticks", per_tick=True),
            id=14, unit="s", thresholds=threshold_lines(0.1, 1),
            description=(
                "Worst wait between an actor becoming runnable (pushed onto a scheduler queue) and "
                "a worker starting to execute it — pure scheduling latency, before any execution. "
                "Large values with an empty run-queue mean one retained long drain delayed "
                "everyone once; compare Max executor drain and Max batch messages. Large values "
                "with a queue nonzero on most scrapes support sustained saturation. Values are "
                "retained by two recent TSC buckets and can cliff-drop when they expire; exact "
                "wall duration is platform-dependent. Diagnostic guides (not SLOs) are yellow at "
                "100 ms and red at 1 s; the color marks a retained spike, not necessarily a "
                "problem still live."
            ),
        ),
        tail_max_panel(
            "Max executor drain", "type",
            recent_max("ton_actor_max_execute_ticks", "type, job, instance"),
            top=top_peak("type", "ton_actor_max_execute_ticks", per_tick=True),
            id=13, unit="s", thresholds=threshold_lines(0.1, 1),
            description=(
                "Longest timed executor drain for one actor: signals and mailbox messages "
                "processed before finish-time bookkeeping. Actor teardown is timed as a message "
                "after this timer stops, so max single message can be higher. Use this as a "
                "head-of-line-blocking indicator, not an exact full lock-hold duration. Values are "
                "retained by two recent TSC buckets and can cliff-drop when they expire; exact "
                "wall duration is platform-dependent. Diagnostic guides (not SLOs) are yellow at "
                "100 ms and red at 1 s; the color marks a retained spike, not necessarily a "
                "problem still live."
            ),
        ),
        tail_max_panel(
            "Max batch messages", "type",
            recent_max("ton_actor_max_batch_messages", "type, job, instance", per_tick=False),
            top=top_peak("type", "ton_actor_max_batch_messages"),
            id=15, unit="short",
            description=(
                "Largest number of messages drained in one executor batch for each class across "
                "the two recent TSC buckets; exact wall duration is platform-dependent. Use with "
                "Max executor drain: a high count with a short drain can be efficient "
                "amortization, while a high count with a long drain is head-of-line risk. There is "
                "no universal bad batch count, so this contextual panel deliberately has no alarm "
                "threshold."
            ),
        ),
    ]),
    row("Perf counters", id=204, panels=[
        perf_timeseries(
            "Perf elapsed load",
            rate("ton_perf_op_ticks_total", "op, job, instance", per_tick=True),
            top=top_total("op", "ton_perf_op_ticks_total", n=PERF_TOP, per_tick=True),
            id=100, unit="short",
            description=(
                "TD_PERF_COUNTER elapsed load per operation, converted from ticks per node and "
                "then collapsed with Node aggregation. Nodes without that operation are absent "
                "rather than zero. 1.0 means one scope-equivalent continuously elapsed on a node, "
                "including blocking and descheduling; this is not OS CPU load. Perf series "
                "controls how many operations are ranked by total elapsed time over the visible "
                "range; choose 50 to show up to 50 registered counters."
            ),
        ),
        perf_timeseries(
            "Perf ops/s", rate("ton_perf_ops_total", "op, job, instance"),
            top=top_total("op", "ton_perf_ops_total", n=PERF_TOP),
            id=108, unit="ops",
            description=(
                "TD_PERF_COUNTER executions per second for each operation, computed per node and "
                "then collapsed with Node aggregation. Nodes without that operation are absent "
                "rather than zero. Perf series controls how many operations are ranked by total "
                "executions over the visible range; choose 50 to show up to 50 registered "
                "counters."
            ),
        ),
        perf_timeseries(
            "Perf time/op",
            ratio(rate("ton_perf_op_ticks_total", "op, job, instance", per_tick=True),
                  rate("ton_perf_ops_total", "op, job, instance")),
            top=top_total("op", "ton_perf_op_ticks_total", n=PERF_TOP, per_tick=True),
            id=116, unit="s",
            description=(
                "Elapsed seconds per TD_PERF_COUNTER execution, computed per node with traffic and "
                "then collapsed with Node aggregation. Blocking and descheduling are included, so "
                "this is not OS CPU time. Tick calibration makes values comparable across "
                "machines. Perf series controls how many operations are ranked by total elapsed "
                "time over the visible range; choose 50 to show up to 50 registered counters."
            ),
        ),
    ]),
    row("Inventory & batching", id=205, collapsed=True, panels=[
        worst_node_stat(
            "Alive actors", gauge("ton_actor_alive", "job, instance"), id=3,
            unit="short", thresholds=thresholds(base="blue"),
            x=6,  # the tiles in this row have always started at column 6
            description=(
                "Highest actor count on any selected node. A rising floor over hours is a leak "
                "signal; using the worst node avoids diluting a one-node leak as the selected "
                "fleet grows."
            ),
        ),
        plain_stat(
            "Weighted batch",
            f"{rate('ton_actor_messages_total', None)}"
            f" / ({rate('ton_actor_executions_total', None)} > 0)",
            id=4, scope="all selected nodes", unit="short", decimals=2,
            thresholds=thresholds(base="blue"),
            description=(
                "Total completed work items divided by total executions across selected nodes: an "
                "execution-weighted batching ratio. Values above 1 mean scheduling was amortized "
                "over multiple messages; signal-only turns can make it fall below 1. No executions "
                "in the window renders No data."
            ),
        ),
    ]),
]


def build():
    return dashboard("ton-actors", "TON Actor Framework", ["ton", "actors"],
                     VARIABLES, ROWS, refresh="15s", version=1)
