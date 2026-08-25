"""The house vocabulary.

Every panel on a TON dashboard is one of these archetypes. The archetype owns
everything mechanical - datasource, tooltip, legend, reduce options, drill-down
link, the attribution overlay, the boilerplate half of a description - so a
board can only supply what is genuinely per-panel. A panel that fits nothing
here is either a missing archetype or a justified raw_panel().
"""

import copy
import string
from typing import NamedTuple

from .core import (
    NODE,
    SEL,
    panel,
    patch,
    ratio,
    sel,
    target,
    variable_custom,
    variable_datasource,
    variable_query,
    variable_textbox,
)
from .core import SELF_UID as SELF

WORST = " (worst node)"
AGG_MARK = "▾"  # marks panels the $agg selector controls
ATTR = "ATTR"  # refId of the hidden "which node is this" overlay; further ones count up
ATTR_COLOR = "#8E8E8E"  # neutral gray, so an overlay row reads as an annotation
WARN = "#EAB839"  # the house warning yellow, on every failure-ratio band
DRILL = "Drill down"
NODE_LABELS = ("job", "instance")  # what an overlay transplants onto the panel it annotates

AGG_DESCRIPTION = (
    "How per-class and per-operation panels collapse selected nodes that expose the series: "
    "worst = maximum, p95 = the 95th percentile, median = the typical node, and best = minimum. "
    "Time-per-item panels include only nodes with traffic, where the ratio is defined. "
    "Health and tail panels keep their fixed aggregation. "
    f"A {AGG_MARK} in the title marks panels controlled by this selector."
)

SLOT_DESCRIPTION = (
    "Expected production slot in seconds for the selected blockchain. Used only for target guides "
    "and collation-budget ratios; it never changes the measured block rate. Set it from the "
    "deployment's chain configuration."
)

# Every attributed panel says so, in the words of whatever its overlay rows are keyed by.
KEY_NOUNS = {"type": "class", "op": "operation", "tl": "type", "reason": "reason"}

# The tail panels additionally explain the retained-maximum window the $agg switch reads.
TAIL_PREAMBLE = (
    f'{AGG_MARK} Follows the "Node aggregation" switch: each node keeps a retained maximum '
    "(~10–20 min window); the switch picks which node's retained max to show — worst "
    "(default), p95, median, or best. "
)

# Actor-tier counters are raw TSC ticks; dividing by the node's calibrated rate gives seconds.
TICKS_TO_SECONDS = ("on (job, instance) group_left max by (job, instance) "
                    f"(ton_actor_ticks_per_second{SEL})")
TICKS_TO_SECONDS_OVER_RANGE = (
    "on (job, instance) group_left max by (job, instance) "
    f"(last_over_time(ton_actor_ticks_per_second{SEL}[$__range] @ end()))")

RATE_WINDOW = "$__rate_interval"
SLOT = "${slot_seconds:raw}"  # the Slot (s) textbox, read inline by every slot-relative panel


def summed(by, body, *, agg="sum"):
    """`agg by (by) (body)`, or the ungrouped `agg(body)` when `by` is None."""
    return f"{agg} by ({by}) ({body})" if by else f"{agg}({body})"


def same_reporters(left, right, *, by="job, instance"):
    """True when two metric families expose exactly the same reporter label sets."""
    left_set = summed(by, left, agg="max")
    right_set = summed(by, right, agg="max")
    left_only = f"count({left_set} unless on ({by}) {right_set}) or vector(0)"
    right_only = f"count({right_set} unless on ({by}) {left_set}) or vector(0)"
    return f"(({left_only}) == 0) and on () (({right_only}) == 0)"


def rate(metric, by, *, over=RATE_WINDOW, per_tick=False, **labels):
    """Per-second rate of a counter grouped by `by` (None sums the fleet into one series)."""
    body = f"rate({metric}{sel(**NODE, **labels)}[{over}])"
    body += f" / {TICKS_TO_SECONDS}" if per_tick else ""
    return summed(by, body)


def gauge(metric, by, *, agg="sum", per_tick=False, **labels):
    """Current value of a gauge grouped by `by`."""
    body = f"{metric}{sel(**NODE, **labels)}" + (f" / {TICKS_TO_SECONDS}" if per_tick else "")
    return summed(by, body, agg=agg)


def recent_max(metric, by, *, per_tick=True):
    """Per-node reading of a window="recent" retained maximum."""
    return gauge(metric, by, agg="max", per_tick=per_tick, window="recent")


def top_keys(key, ranking, *, n=8):
    """`and on (key) topk(...)`: restrict a chart to the n keys `ranking` scores highest."""
    return f" and on ({key}) topk({n}, {ranking})"


def top_total(key, metric, *, n=8, per_tick=False, selector=SEL):
    """`top` fragment: keep the n keys that accumulated the most over the visible range."""
    body = f"increase({metric}{selector}[$__range] @ end())"
    body += f" / {TICKS_TO_SECONDS_OVER_RANGE}" if per_tick else ""
    return top_keys(key, summed(key, body), n=n)


def top_peak(key, metric, *, n=8, per_tick=False):
    """`top` fragment: keep the n keys whose retained maximum was highest over the range."""
    body = f"max_over_time({metric}{sel(**NODE, window='recent')}[$__range] @ end())"
    body += f" / {TICKS_TO_SECONDS_OVER_RANGE}" if per_tick else ""
    return top_keys(key, summed(key, body, agg="max"), n=n)


def worker_occupancy(kind, *, over=RATE_WINDOW):
    """Busy tick-seconds of one worker kind over the threads of that kind."""
    busy = rate("ton_actor_worker_busy_ticks_total", "job, instance", over=over,
                per_tick=True, worker=kind)
    threads = gauge("ton_actor_worker_threads", "job, instance", worker=kind)
    return f"{busy} / on (job, instance) clamp_min({threads}, 1)"


def histogram_quantile(q, metric, *, by=None, over=RATE_WINDOW, **labels):
    """The `q` quantile of a `_bucket` histogram, grouped by `by` on top of `le`."""
    keys = f"{by}, le" if by else "le"
    body = summed(keys, f"rate({metric}_bucket{sel(**labels)}[{over}])")
    return f"histogram_quantile({q}, {body})"


class Line(NamedTuple):
    """One named series of a panel."""

    name: str
    expr: str
    ref_id: str
    bare: bool


class Attributed(NamedTuple):
    """One series, plus the recipe for the hidden row naming the node it came from."""

    name: str
    inner: str
    key: str
    pick: str
    top: str
    shown: str
    overlay: str = None


def line(name, expr, *, ref_id=None, bare=False):
    """A series and the legend that names it. `bare` drops the query-editor keys."""
    return Line(name, expr, ref_id, bare)


def attributed(name, inner, *, key=None, pick="max", top="", shown=None, overlay=None):
    """A series the panel collapses over nodes, tagged with the node it came from.

    `inner` must be a per-(job, instance) vector expression - a guarded per-node ratio
    counts, a fleet aggregate does not. The panel draws `max by (key) (inner)`, or `min`
    for pick="min" (success-type readings, where low is bad), and hides an overlay row
    beside it: the same value, carrying the labels of the node that dominated the visible
    range. `key=None` collapses to one series, `top` keeps only the busiest keys, and
    `shown` replaces the drawn expression where the panel must keep a chain-scoped value
    that no per-node maximum can stand in for.

    `overlay` replaces the derived row outright, for series where range dominance answers
    the wrong question. A sparse event share needs decomposition, not a stable owner: pass
    per-node contributions to the drawn value (same denominator, per-node numerator,
    guarded with > 0) and the rows name exactly the nodes responsible at each moment and
    sum to the drawn line.
    """
    return Attributed(name, inner, key, pick, top, shown, overlay)


def fleet_spread(inner, *, average="average", include_best=True):
    """A per-node reading as worst, p95, median, average, and optionally best.

    This is the default diagnostic shape for a metric where one bad node matters: the visible
    worst line receives stable hover attribution from :func:`attributed`, while p95/median show
    whether the problem is broad. ``inner`` must retain ``job`` and ``instance`` labels.
    """
    lines = [
        attributed("worst (max)", inner),
        line("p95", f"quantile(0.95, {inner})"),
        line("median", f"quantile(0.5, {inner})"),
        line(average, f"avg({inner})"),
    ]
    if include_best:
        lines.append(line("best (min)", f"min({inner})"))
    return lines


def _lines(lines):
    return [item if isinstance(item, (Line, Attributed)) else line(None, item) for item in lines]


def _targets(lines, *, instant=False):
    """refIds run A, B, C... over the series that did not ask for one of their own."""
    items = _lines(lines)
    free = (letter for letter in string.ascii_uppercase
            if letter not in {item.ref_id for item in items})
    return [target(item.expr, ref_id=item.ref_id or next(free), legend=item.name,
                   instant=instant, bare=item.bare) for item in items]


def hover_note(keys):
    """The sentence an attributed panel appends: what the ⇒ rows in its tooltip are."""
    nouns = {KEY_NOUNS.get(key, "series") for key in keys}
    noun = nouns.pop() if len(nouns) == 1 else "series"
    return (f' Hover attribution: the tooltip lists "{noun} ⇒ node" rows (hidden from the chart) '
            f"answering, per {noun}, whose number the drawn line is. Each row names one stable "
            "node for the visible range — on a Node-switch panel the node whose series stays "
            "nearest the drawn quantile (the dominant node under worst, the node the median "
            "follows under median), elsewhere the node that dominated the range — and always "
            "carries that node's own value, so where the row hugs the drawn line, the line is "
            "that node.")


def _drawn(item):
    return item.shown or summed(item.key, item.inner, agg=item.pick)


# Attribution subqueries are deliberately coarse. They answer "which node dominated the visible
# range", which needs no sub-minute resolution, and the step bounds their cost: without it the
# server's evaluation_interval decides, so a long range silently turns every one of the ~90
# attribution targets into a full-resolution range scan on each refresh.
ATTR_STEP = "5m"


def _overlay(item, node_labels=NODE_LABELS):
    """One node's own series, chosen to explain the drawn line: whose number is on screen.

    Mirroring the drawn aggregate here was wrong by any interpretation — a fleet median wearing
    one node's name asserts a load that node never had. Where the Node switch draws a fleet
    quantile, that quantile is in effect one node's number, so the row is the per-node inner
    filtered to the node whose series stayed nearest the drawn line over the visible range: the
    dominant node under worst, the node the median follows under median. Elsewhere the drawn
    line is a fleet figure no single node is, and the row names the range-dominant node — its
    maximum over the range, or minimum where low is the bad direction.

    `node_labels` is what identifies a node on the owning panel — the same tuple the panel
    transplants. Boards whose per-node identity carries more than (job, instance), like the
    network board's `ton_scope`, must join on all of it: matching on a subset lets one scope's
    row take another scope's value.
    """
    node = ", ".join(node_labels)
    on = f"{item.key}, {node}" if item.key else node
    drawn = _drawn(item)
    if "${agg}" in drawn:
        by = f" by ({item.key})" if item.key else ""
        join = f"on ({item.key}) group_left()" if item.key else "on () group_left()"
        miss = (f"avg_over_time((abs(({item.inner}) - {join} ({drawn})))"
                f"[$__range:{ATTR_STEP}] @ end())")
        return f"({item.inner}) and on ({on}) (bottomk{by}(1, {miss}))"
    picker, over = (("topk", "max_over_time") if item.pick == "max"
                    else ("bottomk", "min_over_time"))
    rank = f"{picker} by ({item.key}) (1, " if item.key else f"{picker}(1, "
    return (f"({item.inner}) and on ({on})"
            f" ({rank}{over}(({item.inner})[$__range:{ATTR_STEP}] @ end())))")


LIST_LEGEND = {"displayMode": "list", "placement": "bottom", "showLegend": True, "calcs": []}


def table_legend(*calcs, sort="Last *", placement="bottom"):
    """Legend as a table of reducer columns, sorted by one of them, biggest first.

    `sort=None` keeps the order the panel lists its series in - what a stack of sequential stages
    needs, where the series order is the order the stages run.
    """
    legend = {
        "displayMode": "table",
        "placement": placement,
        "showLegend": True,
        "calcs": list(calcs),
    }
    if sort:
        legend |= {"sortBy": sort, "sortDesc": True}
    return legend


TABLE_LEGEND = table_legend("lastNotNull")

STAT_OPTIONS = {
    "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
    "colorMode": "value",
    "graphMode": "area",
    "justifyMode": "auto",
    "orientation": "auto",
    "percentChangeColorMode": "standard",
    "showPercentChange": False,
    "textMode": "value_and_name",
    "wideLayout": True,
}

# An overlay frame carries the attribution label only: no line, no legend row, one flat color.
ATTR_PROPS = [
    {"id": "custom.hideFrom", "value": {"legend": True, "tooltip": False, "viz": False}},
    {"id": "custom.lineWidth", "value": 0},
    {"id": "custom.fillOpacity", "value": 0},
    {"id": "custom.showPoints", "value": "never"},
    {"id": "custom.lineStyle", "value": {"fill": "solid"}},
    {"id": "custom.spanNulls", "value": False},
    {"id": "color", "value": {"fixedColor": ATTR_COLOR, "mode": "fixed"}},
]

def _frame(ref_id, properties):
    return {"matcher": {"id": "byFrameRefID", "options": ref_id}, "properties": properties}


def _attributed_targets(items, transplant):
    """Drawn series first; every attributed one adds an overlay row and its frame override.

    Both rows stay in the tooltip: the drawn one keeps the series' real colour and name, so it is
    the row the cursor highlights, and the gray overlay beside it adds the node.
    """
    drawn = _targets([line(item.name, _drawn(item) + item.top)
                      if isinstance(item, Attributed) else item for item in items])
    overlays, overrides = [], []
    for item in items:
        if not isinstance(item, Attributed):
            continue
        ref = ATTR if not overlays else f"{ATTR}{len(overlays) + 1}"
        name = item.name or ("worst" if item.pick == "max" else "best")
        overlays.append(target((item.overlay or _overlay(item, transplant)) + item.top,
                               ref_id=ref, bare=True, legend=f"{name} ⇒ {{{{instance}}}}"))
        overrides.append(_frame(ref, copy.deepcopy(ATTR_PROPS)))
    return drawn + overlays, overrides


def rename_by_regex(regex, replacement=""):
    """Transformation that rewrites series names; the boards use it to drop C++ namespaces."""
    return {"id": "renameByRegex", "options": {"regex": regex, "renamePattern": replacement}}


# Actor class labels are C++ type names; the chart shows them without their namespace.
STRIP_NAMESPACES = rename_by_regex(
    r"/^(?:ton::(?:validator::)?)?(?:consensus::\(anonymous namespace\)::)?(.*)$/", "$1")

STYLE_PROPS = {
    "unit": lambda v: ("unit", v),
    "unstack": lambda v: ("custom.stacking",
                          {"group": "A", "mode": "none"} if v is True else v),
    "axis": lambda v: ("custom.axisPlacement", v),
    "axis_label": lambda v: ("custom.axisLabel", v),
    "width": lambda v: ("custom.lineWidth", v),
    "fill": lambda v: ("custom.fillOpacity", v),
    "dash": lambda v: ("custom.lineStyle", {"dash": list(v), "fill": "dash"}),
    "bands": lambda v: ("custom.thresholdsStyle", {"mode": v}),
    "color": lambda v: ("color", {"fixedColor": v, "mode": "fixed"}),
}


def series_style(name, *, regex=False, **props):
    """Per-series field overrides; the properties land in the order they are written."""
    return {
        "matcher": {"id": "byRegexp" if regex else "byName", "options": name},
        "properties": [{"id": pid, "value": value}
                       for pid, value in (STYLE_PROPS[key](got) for key, got in props.items()
                                          if got is not None)],
    }


FLEET_SPREAD_STYLES = [
    series_style("worst (max)", width=3, color="purple"),
    series_style("p95", color="blue"),
    series_style("median", color="light-blue"),
    series_style("average", width=1, fill=0, color="#8AB8E6"),
    series_style("best (min)", width=1, fill=0, dash=(6, 4), color="green"),
]


def reference_line(name):
    """The dashed red line a panel draws at its configured target or budget.

    Never stacked: a budget is the level the stack is read against, so letting it join the stack
    would lift it by the budget and draw a total that measures nothing.
    """
    return series_style(name, color="red", dash=(10, 10), fill=0, width=1, unstack=True)


def unstacked_total(name, color, *, dash=None):
    """A total drawn as a plain line on top of the stack it sums up."""
    return series_style(name, unstack=True, fill=0, width=2, dash=dash, color=color)


def unstacked_marker(name, color, *, dash=None):
    """A moment drawn across a stack of durations: where on the axis something actually happened.

    Never stacked and never part of the total - the stack says how long, this says when, so letting
    it join would add a position on the axis to a sum of lengths.
    """
    return series_style(name, unstack=True, fill=0, width=2, dash=dash, color=color)


def standard_variables():
    """datasource / job / instance - byte-identical on every board."""
    return [
        variable_datasource(
            "datasource",
            label="Prometheus",
            description="Prometheus scraping the TON node metrics exporters",
        ),
        variable_query(
            "job",
            label="job",
            query="label_values(up, job)",
            multi=False,
            include_all=False,
            description=(
                "The scrape job to look at: one job is one network. Single-select on purpose — "
                "chain-level panels sum seqnos, block rates and collation budgets across whatever "
                "the selector spans, so a multi-job selection would merge unrelated fleets "
                "(mainnet + testnet) into one meaningless chain. Compare networks in two tabs."
            ),
        ),
        variable_query(
            "instance",
            label="instance",
            query='label_values(up{job=~"$job"}, instance)',
            description=(
                "Node filter within the selected job. Node and fleet-diagnostic panels respect it; "
                "chain-truth panels explicitly query every reporter in the job so selecting one "
                "node cannot redefine the blockchain."
            ),
        ),
    ]


def agg_variable(*, description=AGG_DESCRIPTION):
    """The node-collapse selector every agg_timeseries reads."""
    return variable_custom(
        "agg",
        label="Node aggregation",
        choices=[("worst", "1"), ("p95", "0.95"), ("median", "0.5"), ("best", "0")],
        selected="worst",
        description=description,
    )


def slot_variable(*, description=SLOT_DESCRIPTION):
    """Expected production slot; panels quote block rates as a share of it."""
    return variable_textbox("slot_seconds", label="Slot (s)", value="0.4",
                            description=description)


def drill_link(uid, *, with_instance=True):
    params = ["${datasource:queryparam}", "${job:queryparam}"]
    if with_instance:
        params.append("var-instance=${__field.labels.instance}")
    params.append("${__url_time_range}")
    return {"title": DRILL, "url": f"/d/{uid}?" + "&".join(params), "targetBlank": False}


def thresholds(*steps, base="green", style="off"):
    """Colour bands above `base`, as (colour, value) pairs."""
    return {
        "steps": [{"color": base, "value": None}] + [{"color": c, "value": v} for c, v in steps],
        "style": style,
    }


def threshold_lines(warn, crit, *, warn_color="yellow"):
    """Bands drawn as reference lines across the plot."""
    return thresholds((warn_color, warn), ("red", crit), style="line")


def of_target_bands():
    """Share-of-target tiles run the other way: red until the chain keeps up."""
    return thresholds((WARN, 0.5), ("green", 0.8), base="red")


def failure_bands():
    """The house bands for a failure or loss ratio, drawn across every board that shows one."""
    return threshold_lines(0.01, 0.05, warn_color=WARN)


def _steps(spec):
    return {"mode": "absolute", "steps": copy.deepcopy((spec or thresholds())["steps"])}


def _stat(title, lines, *, description, unit, decimals, thresholds, links, sparkline,
          w, h, x, y, id, query_scope, overrides):
    defaults = {
        "thresholds": _steps(thresholds),
        "mappings": [],
        "color": {"mode": "thresholds"},
    }
    if decimals is not None:
        defaults["decimals"] = decimals
    if links:
        defaults["links"] = links
    named = any(isinstance(item, Line) and item.name for item in lines)
    return panel(
        "stat", title, description,
        unit=unit, w=w, h=h, x=x, y=y, id=id,
        defaults=defaults,
        options=dict(STAT_OPTIONS,
                     textMode="value_and_name" if named else "auto",
                     graphMode="area" if sparkline else "none"),
        targets=_targets(lines, instant=True),
        query_scope=query_scope,
        overrides=overrides,
    )


def worst_node_stat(title, inner, *, unit, thresholds, description, id, decimals=None,
                    drill=SELF, name_labels=("instance",), sparkline=False,
                    w=6, h=4, x=None, y=None, overrides=None):
    """The single worst node for `inner`, named on the tile and linked to its drill-down."""
    return _stat(
        title if title.endswith(WORST) else title + WORST,
        [line(": ".join("{{%s}}" % label for label in name_labels), f"topk(1, {inner})")],
        description=description, unit=unit, decimals=decimals, thresholds=thresholds,
        links=[drill_link(drill)] if drill else None,
        sparkline=sparkline, w=w, h=h, x=x, y=y, id=id, overrides=overrides,
        query_scope="selected",
    )


def plain_stat(title, *lines, scope, unit, thresholds, description, id, decimals=None,
               sparkline=False, w=6, h=4, x=None, y=None, query_scope="selected",
               overrides=None):
    """A stat that deliberately is not per-node; `scope` records what it collapses over.

    Pass bare expressions for a single number, or `line()`s to show several named ones.
    """
    if not (scope or "").strip():
        raise ValueError(f"stat {title!r}: say what it aggregates over, or use worst_node_stat")
    return _stat(
        title, lines,
        description=description, unit=unit, decimals=decimals, thresholds=thresholds,
        links=None, sparkline=sparkline, w=w, h=h, x=x, y=y, id=id, overrides=overrides,
        query_scope=query_scope,
    )


WORST_FIRST = " (fleet)"  # the triage scope marker; a triage panel is fixed-worst, never ▾


class Column(NamedTuple):
    """One scorecard column: an instant per-(job, instance) query and how its cell reads."""

    name: str
    expr: str
    unit: str
    thresholds: dict
    drill: str


def column(name, expr, *, unit="short", thresholds=None, drill=None):
    """A scorecard column. `drill` is the board its cell links to; None leaves the cell inert."""
    return Column(name, expr, unit, thresholds, drill)


def _table_drill(uid):
    """A drill link for a native table cell: the row's instance comes from the frame, not a
    series label, so it is `${__data.fields.instance}` — the label ref never resolves in a cell."""
    params = ["${datasource:queryparam}", "${job:queryparam}",
              "var-instance=${__data.fields.instance}", "${__url_time_range}"]
    return {"title": DRILL, "url": f"/d/{uid}?" + "&".join(params), "targetBlank": False}


def _cell_override(name, *, unit=None, thresholds=None, links=None, hidden=False):
    """Per-column table field config: unit, a threshold-painted background, a link, or a hide."""
    props = []
    if hidden:
        props.append({"id": "custom.hidden", "value": True})
    if unit is not None:
        props.append({"id": "unit", "value": unit})
    if thresholds is not None:
        props.append({"id": "thresholds", "value": _steps(thresholds)})
        props.append({"id": "color", "value": {"mode": "thresholds"}})
        props.append({"id": "custom.cellOptions", "value": {"type": "color-background",
                                                            "mode": "basic"}})
    if links:
        props.append({"id": "links", "value": copy.deepcopy(links)})
    return {"matcher": {"id": "byName", "options": name}, "properties": props}


class SummaryColumn(NamedTuple):
    """One instant-query value column in a categorical summary table."""

    name: str
    expr: str
    unit: str
    thresholds: dict
    hidden: bool


def summary_column(name, expr, *, unit="short", thresholds=None, hidden=False):
    """A numeric column for :func:`summary_table`.

    The expression must retain the table's categorical key label. It may also retain one of the
    explicitly declared ``label_columns`` (an attributed recent maximum commonly keeps
    ``instance``, for example); the merge then carries that label into the same category row.
    """
    return SummaryColumn(name, expr, unit, thresholds, hidden)


def summary_table(title, key_label, columns, *, description, id, key_name=None,
                  label_columns=(), exclude_labels=("__name__", "job", "node"),
                  key_mappings=(), sort_by=None, sort_desc=False,
                  field_order=None,
                  query_scope="selected", w=12, h=8, x=None, y=None, overrides=None):
    """A compact instant summary, one row per value of ``key_label``.

    ``columns`` is an ordered list of :func:`summary_column` values. Each query becomes a table
    frame and must retain ``key_label``; ``label_columns`` is an ordered sequence of
    ``(source_label, display_name)`` pairs for non-numeric context carried by any query, such as
    ``("instance", "owner")``. The transforms deliberately drop each frame's scrape timestamp
    before merging on the remaining labels, then organize the result into key, values, and context.

    ``key_mappings`` is an ordered sequence (or insertion-ordered mapping) from raw key values to
    display text. It is a field value mapping rather than a transformation, so raw values remain
    available to the merge and table sort. ``field_order`` may interleave displayed numeric and
    label columns; by default all numeric columns precede label context. ``sort_by`` names a
    displayed column; omit it to retain the merged frame's order.
    """
    specs = [c if isinstance(c, SummaryColumn) else summary_column(*c) for c in columns]
    if not specs:
        raise ValueError(f"summary table {title!r}: at least one value column is required")
    if len(specs) > len(string.ascii_uppercase):
        raise ValueError(f"summary table {title!r}: at most 26 value columns are supported")
    if len({spec.name for spec in specs}) != len(specs):
        raise ValueError(f"summary table {title!r}: value column names must be unique")

    labels = list(label_columns.items() if isinstance(label_columns, dict) else label_columns)
    if len({source for source, _ in labels}) != len(labels):
        raise ValueError(f"summary table {title!r}: label columns must be unique")

    display_key = key_name or key_label
    refs = list(string.ascii_uppercase)[:len(specs)]
    targets = [target(spec.expr, ref_id=ref, instant=True, format="table")
               for spec, ref in zip(specs, refs)]

    rename = {f"Value #{ref}": spec.name for spec, ref in zip(specs, refs)}
    if display_key != key_label:
        rename[key_label] = display_key
    rename.update({source: display for source, display in labels if source != display})
    # Grafana applies ``indexByName`` to input field names before ``renameByName``. Translate the
    # caller's display-oriented order back to source fields; using display names directly silently
    # leaves a renamed label (an owner, for example) in the wrong place.
    displayed_fields = [display_key, *(spec.name for spec in specs),
                        *(display for _, display in labels)]
    if len(set(displayed_fields)) != len(displayed_fields):
        raise ValueError(f"summary table {title!r}: displayed field names must be unique")
    displayed_sources = dict(zip(
        displayed_fields,
        [key_label, *(f"Value #{ref}" for ref in refs), *(source for source, _ in labels)],
    ))
    displayed_order = (list(displayed_sources) if field_order is None else list(field_order))
    if set(displayed_order) != set(displayed_sources) or len(displayed_order) != len(displayed_sources):
        raise ValueError(
            f"summary table {title!r}: field_order must contain every displayed field exactly once"
        )
    source_order = [displayed_sources[name] for name in displayed_order]

    # Instant frames are stamped independently. Leaving Time in place makes merge use it as a join
    # key and produces one sparse row per scrape timestamp instead of one complete category row.
    drop_time = {"id": "organize", "options": {
        "excludeByName": {"Time": True}, "renameByName": {}, "indexByName": {}}}
    kept_labels = {key_label, *(source for source, _ in labels)}
    excluded = {name: True for name in exclude_labels if name not in kept_labels}
    organize = {"id": "organize", "options": {
        "excludeByName": excluded,
        "renameByName": rename,
        "indexByName": {name: i for i, name in enumerate(source_order)},
    }}

    field_overrides = [
        _cell_override(spec.name, unit=spec.unit, thresholds=spec.thresholds,
                       hidden=spec.hidden)
        for spec in specs
    ]
    mappings = list(key_mappings.items() if isinstance(key_mappings, dict) else key_mappings)
    if mappings:
        field_overrides.append({
            "matcher": {"id": "byName", "options": display_key},
            "properties": [{"id": "mappings", "value": [{"type": "value", "options": {
                raw: {"text": shown, "index": i}
                for i, (raw, shown) in enumerate(mappings)
            }}]}],
        })

    options = {
        "showHeader": True,
        "cellHeight": "sm",
        "footer": {"show": False, "reducer": ["sum"], "countRows": False, "fields": ""},
        "sortBy": ([{"displayName": sort_by, "desc": sort_desc}] if sort_by else []),
    }
    return panel(
        "table", title, description, unit="short", w=w, h=h, x=x, y=y, id=id,
        defaults={"custom": {"align": "auto", "cellOptions": {"type": "auto"},
                             "filterable": False, "inspect": False},
                  "thresholds": _steps(thresholds()), "mappings": []},
        field_overrides=field_overrides,
        options=options,
        targets=targets,
        transformations=[drop_time, {"id": "merge", "options": {}}, organize],
        query_scope=query_scope,
        overrides=overrides,
    )


def scorecard(title, columns, *, description, id, overview="ton-overview",
              w=24, h=8, x=None, y=None, overrides=None):
    """A native table, one row per (job, instance), one thresholded column per health dimension.

    `columns` is an ordered list of column()s, each an instant per-(job, instance) query. The
    first is the severity the table sorts worst-first; it stays in the frame for the sort but is
    hidden from view. Every metric cell paints its own threshold as a background and links to the
    drill-down that owns it, carrying datasource, job, the row's instance and the time range; the
    instance cell links to that node's own overview. The board supplies only the list — the
    merge/organize join that turns N instant queries into one row, the per-column overrides and the
    sort all live here, so the same JSON reads at eight nodes and at four hundred (worst first).
    """
    specs = [c if isinstance(c, Column) else column(*c) for c in columns]
    refs = list(string.ascii_uppercase)[:len(specs)]
    targets = [target(spec.expr, ref_id=ref, instant=True, format="table")
               for spec, ref in zip(specs, refs)]
    sev, metrics = specs[0], specs[1:]

    rename = {f"Value #{ref}": spec.name for spec, ref in zip(specs, refs)}
    order = [sev.name, "instance"] + [s.name for s in metrics]
    # Each instant query returns a Time column stamped with that metric's own last-scrape time, and
    # merge joins on every shared field — so an unremoved Time splits one node into a half-empty row
    # per column whenever the scrapes do not line up. Drop it from every frame first; then merge
    # joins purely on (job, instance), and the second organize names, orders and hides.
    drop_time = {"id": "organize", "options": {"excludeByName": {"Time": True},
                                               "renameByName": {}, "indexByName": {}}}
    organize = {"id": "organize", "options": {
        "excludeByName": {"job": True, "__name__": True, "node": True},
        "renameByName": rename,
        "indexByName": {name: i for i, name in enumerate(order)},
    }}

    field_overrides = [
        _cell_override(sev.name, thresholds=sev.thresholds, hidden=True),
        _cell_override("instance", links=[_table_drill(overview)]),
    ] + [_cell_override(s.name, unit=s.unit, thresholds=s.thresholds,
                        links=[_table_drill(s.drill)] if s.drill else None) for s in metrics]

    return panel(
        "table", title, description, unit="short", w=w, h=h, x=x, y=y, id=id,
        defaults={"custom": {"align": "auto", "cellOptions": {"type": "auto"},
                             "filterable": False, "inspect": False},
                  "thresholds": _steps(thresholds()), "color": {"mode": "thresholds"},
                  "mappings": []},
        field_overrides=field_overrides,
        options={"showHeader": True, "cellHeight": "sm",
                 "footer": {"show": False, "reducer": ["sum"], "countRows": False, "fields": ""},
                 "sortBy": [{"displayName": sev.name, "desc": True}]},
        targets=targets,
        transformations=[drop_time, {"id": "merge", "options": {}}, organize],
        query_scope="selected",
        overrides=overrides,
    )


class Condition(NamedTuple):
    """One Grafana triage condition over base metrics, with no rule-file dependency."""

    key: str
    label: str
    severity: str  # critical | warning | info, when red
    scope: str  # node | chain
    red: str
    warn: str
    for_s: int


def condition(key, label, *, severity, scope, red, for_s, warn=None):
    """One registry entry. The board writes the inline exprs; every triage panel reads them here."""
    if scope not in {"node", "chain"}:
        raise ValueError(f"triage condition {key!r}: invalid scope {scope!r}")
    if severity not in {"critical", "warning", "info"}:
        raise ValueError(f"triage condition {key!r}: invalid severity {severity!r}")
    if for_s <= 0:
        raise ValueError(f"triage condition {key!r}: debounce must be positive")
    return Condition(key, label, severity, scope, red, warn, for_s)


def _tagged(conditions, attr):
    """OR each condition's `attr` bool into one vector, tagged distinct by key so a later
    `sum by (job, instance)` counts firing conditions instead of collapsing equal labelsets."""
    return " or ".join(f'label_replace({getattr(c, attr)}, "sig", "{c.key}", "", "")'
                       for c in conditions if getattr(c, attr))


def node_severity(conditions):
    """Inline 0/1/2/3 per-node severity, reconstructing the retired ton:node:severity composition.

    2 if any node red bool fires, +1 if any warn does. clamp_max collapses each tier to one bit, so
    a node tripping several reds still reads 2 and count(>=2) names it once (a down node counted
    once). `warn or 0*red` defaults the warn tier to 0 on the red tier's instances — unreachable
    (up == bool 0) is present for every target and anchors the sum — rather than dropping a node
    whose warn samples aged out. Each bool is normalised `max by (job, instance)` at source, so a
    stray target label cannot leak into the sum.
    """
    node = [c for c in conditions if c.scope == "node"]
    red = f"clamp_max(sum by (job, instance) ({_tagged(node, 'red')}), 1)"
    warn = f"clamp_max(sum by (job, instance) ({_tagged(node, 'warn')}), 1)"
    return f"2 * {red} + ({warn} or 0 * {red})"


def node_condition_rows(conditions):
    """Per-(node, condition) severity series, the condition named in the `sig` label.

    One united vector: each node condition contributes one series valued 2 while its red bool
    holds, 1 while only its warn does, and 0 while healthy, tagged with the condition's human
    label. The tiers combine arithmetically (`clamp_max(red*2 + warn, 2)`) rather than with `or`:
    the red bool exists even at 0, so an `or` of separate tier branches would shadow every
    warn-only sample behind a zero red. Zeros are kept deliberately — they are what terminates a
    band when the node recovers — and the range filter keeps rows only for pairs that were
    unhealthy at some point in the visible window, so healthy pairs draw nothing at all. The
    panel keys its rows on the legend, so the condition name rides in the series identity, and
    simultaneous conditions on one node appear as parallel rows instead of fighting for a cell.
    """
    node = [c for c in conditions if c.scope == "node"]
    branches = []
    for c in node:
        # `warn or 0*red` anchors the warn tier on the red bool's labelset (the same defaulting
        # node_severity() uses): the tier sum is label-matched vector addition, and a condition
        # whose red arm covers nodes the warn expression has no sample for — mc-stale's
        # vanished-gauge arm — would otherwise drop those nodes from the sum entirely.
        if c.warn:
            tier = f"clamp_max(({c.red}) * 2 + (({c.warn}) or 0 * ({c.red})), 2)"
        else:
            tier = f"({c.red}) * 2"
        branches.append(f'label_replace({tier}, "sig", "{c.label}", "", "")')
    union = f"max by (job, instance, sig) ({' or '.join(branches)})"
    active = f"max by (job, instance, sig) (max_over_time(({union})[$__range:] @ end()))"
    return f"({union}) and on (job, instance, sig) (({active}) > 0)"


def breach_timeline(title, conditions, *, description, id, w=24, h=6, x=None, y=None,
                    overrides=None):
    """Stepped count of breachers per condition; a gap means nobody breaches it.

    One step line per condition, valued at how many are breaching it right now — nodes for a node
    condition, jobs for a chain one — and drawn only while that count is nonzero, so a healthy
    stretch is a gap rather than a zero carpet. The hover shows the exact counts; this used to be
    a state-timeline, but that panel renders numeric cells as threshold-bucket labels ("1+") in
    current Grafana, burying the count it exists to show. Fixed fleet scope, never the ▾.
    """
    targets = [target(f"count(({c.red}) == 1) > 0", ref_id=ref, legend=c.label)
               for c, ref in zip(conditions, string.ascii_uppercase)]
    custom = {"lineWidth": 2, "fillOpacity": 25, "spanNulls": False,
              "lineInterpolation": "stepAfter", "showPoints": "never",
              "stacking": {"mode": "none"},
              "hideFrom": {"legend": False, "tooltip": False, "viz": False}}
    return panel(
        "timeseries",
        title if title.endswith(WORST_FIRST) else title + WORST_FIRST,
        description,
        unit="short", w=w, h=h, x=x, y=y, id=id,
        defaults={"custom": custom, "min": 0, "decimals": 0,
                  "noValue": "no condition breached in this window"},
        options={"legend": {"showLegend": True, "displayMode": "list", "placement": "bottom"},
                 "tooltip": {"mode": "multi", "sort": "desc"}},
        targets=targets,
        query_scope="mixed",
        overrides=overrides,
    )


def node_severity_timeline(title, conditions, *, description, id, w=24, h=8, x=None, y=None,
                           overrides=None):
    """A state-timeline of the nodes that went bad, one row each, coloured by inline severity.

    The breach-by-condition timeline says which failure mode fired and when; this says which NODE
    was bad and when — the "where" that an instant scorecard loses once a breach recovers. Rows
    are node-and-condition pairs, the condition named right in the row label, so a band answers
    what was bad on which node without relying on cell text; simultaneous conditions appear as
    parallel rows. Only unhealthy stretches are plotted: an empty panel means every selected node
    stayed healthy, and the height is O(active problems), not O(nodes). Yellow is the warn tier,
    red the bad tier; the magnitudes live on the scorecard. Fixed fleet scope — no ▾.
    """
    custom = {"lineWidth": 0, "fillOpacity": 80, "spanNulls": False, "insertNulls": False,
              "hideFrom": {"legend": False, "tooltip": False, "viz": False}}
    return panel(
        "state-timeline",
        title if title.endswith(WORST_FIRST) else title + WORST_FIRST,
        description,
        unit="short", w=w, h=h, x=x, y=y, id=id,
        defaults={"custom": custom, "color": {"mode": "thresholds"},
                  # Base transparent: the healthy zeros exist to terminate a band, not to draw one.
                  "thresholds": _steps(thresholds(("#EAB839", 1), ("red", 2), base="transparent")),
                  "noValue": "every selected node stayed healthy",
                  "mappings": []},
        options={"mergeValues": True, "showValue": "never", "alignValue": "left", "rowHeight": 0.9,
                 "legend": {"showLegend": False, "displayMode": "list", "placement": "bottom"},
                 "tooltip": {"mode": "single", "sort": "none"}},
        targets=[target(node_condition_rows(conditions), ref_id="A",
                        legend="{{instance}} · {{sig}}")],
        query_scope="selected",
        overrides=overrides,
    )


SEVERITY_COLORS = (("critical", "red"), ("warning", "orange"), ("info", "blue"))
SEVERITY_RANK = {"critical": 2, "warning": 1, "info": 0}


def _active(c):
    """A registry condition's active series: red for most of its debounce window, tagged for the table.

    The subquery is `avg_over_time((<red>)[for_s:])` — red is a computed inline bool, not a bare
    selector, so it needs subquery form. Averaging instead of min_over_time keeps a FLAPPING
    condition listed: a node whose exporter answers only some scrapes never sustains continuous
    red, but red for at least half the window is still a breach; unmeasurable samples are absent
    and do not vote. The >= 0.5 comparison keeps only firing series (carrying their avg, 0.5-1);
    the clamp normalizes them to exactly 1 so the severity rank stays an exact sort key.
    """
    # The explicit 15 s resolution matters: a bare [Ns:] subquery steps at the GLOBAL
    # evaluation_interval (often 1m), which would sample a 120 s window once or twice and fire on
    # a single red point. 15 s gives every condition at least for_s/15 votes on any deployment.
    majority_red = f"clamp_max(2 * (avg_over_time(({c.red})[{c.for_s}s:15s]) >= 0.5), 1)"
    fire = f"{SEVERITY_RANK[c.severity]} * {majority_red}"
    for label, value in (("condition", c.label), ("severity", c.severity), ("scope", c.scope)):
        fire = f'label_replace({fire}, "{label}", "{value}", "", "")'
    return fire


def triage_table(title, conditions, *, description, id, overview="ton-overview",
                 w=24, h=6, x=None, y=None, overrides=None):
    """A native table of active triage conditions, reconstructed inline from base metrics.

    One union query recomputes each condition's red bool, keeps those red for most of `for_s`, and
    tags each with its name, severity and scope. This is dashboard triage state, not an alertmanager
    or paging surface. The hidden severity rank sorts critical above warning above info. A node row
    links to that node; a chain condition has no instance and therefore no node drill-down.
    """
    union = " or ".join(_active(c) for c in conditions)
    targets = [target(union, ref_id="A", instant=True, format="table")]

    order = ["sevrank", "severity", "condition", "instance", "scope"]
    # Single query, so no merge: drop Time/job/__name__, name the value sevrank (hidden), order the
    # label columns. On an empty fleet the frame has no fields, so nothing renders but the noValue
    # — no ghost header the multi-query ALERTS version left behind.
    organize = {"id": "organize", "options": {
        "excludeByName": {"Time": True, "job": True, "__name__": True},
        "renameByName": {"Value #A": "sevrank"},
        "indexByName": {name: i for i, name in enumerate(order)}}}

    sev_map = [{"type": "value", "options": {
        sev: {"color": color, "index": i} for i, (sev, color) in enumerate(SEVERITY_COLORS)}}]
    severity_cell = {"matcher": {"id": "byName", "options": "severity"}, "properties": [
        {"id": "mappings", "value": sev_map},
        {"id": "custom.cellOptions", "value": {"type": "color-background", "mode": "basic"}}]}

    return panel(
        "table", title, description, unit="short", w=w, h=h, x=x, y=y, id=id,
        defaults={"custom": {"align": "auto", "cellOptions": {"type": "auto"},
                             "filterable": False, "inspect": False},
                  "thresholds": _steps(thresholds()), "mappings": [],
                  "noValue": "no active triage conditions"},
        field_overrides=[
            _cell_override("sevrank", hidden=True),
            severity_cell,
            _cell_override("instance", links=[_table_drill(overview)]),
        ],
        options={"showHeader": True, "cellHeight": "sm",
                 "footer": {"show": False, "reducer": ["sum"], "countRows": False, "fields": ""},
                 "sortBy": [{"displayName": "sevrank", "desc": True}]},
        targets=targets,
        transformations=[organize],
        query_scope="mixed",
        overrides=overrides,
    )


def _ts_custom(*, fill, style, line_width, axis_label, stacking, log):
    return {
        "lineWidth": line_width,
        "fillOpacity": fill,
        "showPoints": "never",
        "drawStyle": "line",
        "lineInterpolation": "linear",
        "barAlignment": 0,
        "pointSize": 5,
        "gradientMode": "none",
        "spanNulls": False,
        "insertNulls": False,
        "stacking": {"group": "A", "mode": stacking},
        "axisPlacement": "auto",
        "axisLabel": axis_label,
        "axisColorMode": "text",
        "axisBorderShow": False,
        "scaleDistribution": {"log": log, "type": "log"} if log else {"type": "linear"},
        "axisCenteredZero": False,
        "hideFrom": {"legend": False, "tooltip": False, "viz": False},
        "thresholdsStyle": {"mode": style},
    }


def _timeseries(title, description, *, unit, targets, bands, legend, fill, w, h, x, y, id,
                line_width=2, axis_label="", stacking="none", log=None, color_mode=None,
                min=None, max=None, decimals=None, tooltip=None, field_overrides=(),
                transformations=(), query_scope=None, overrides=None):
    spec = bands or thresholds()
    defaults = {
        "custom": _ts_custom(fill=fill, style=spec["style"], line_width=line_width,
                             axis_label=axis_label, stacking=stacking, log=log),
        "color": {"mode": color_mode or "palette-classic"},
        "thresholds": _steps(spec),
        "mappings": [],
    }
    for key, value in (("min", min), ("max", max), ("decimals", decimals)):
        if value is not None:
            defaults[key] = value
    extra = {"transformations": copy.deepcopy(list(transformations))} if transformations else {}
    return panel(
        "timeseries", title, description,
        unit=unit, w=w, h=h, x=x, y=y, id=id,
        defaults=defaults,
        field_overrides=field_overrides,
        options={"legend": copy.deepcopy(legend),
                 "tooltip": tooltip or {"mode": "multi", "sort": "desc"}},
        targets=targets,
        query_scope=query_scope,
        overrides=overrides,
        **extra,
    )


def per_node_timeseries(title, *lines, unit, description, id, legend=LIST_LEGEND,
                        thresholds=None, min=0, max=None, decimals=None, axis_label="",
                        fill=8, line_width=2, styles=(), transformations=(),
                        w=12, h=8, x=None, y=None, overrides=None):
    """One series per node (or per node and kind): no $agg, nothing hidden."""
    return _timeseries(
        title, description, unit=unit, targets=_targets(lines),
        bands=thresholds, legend=legend, fill=fill, line_width=line_width,
        axis_label=axis_label, min=min, max=max, decimals=decimals,
        field_overrides=styles, transformations=transformations,
        w=w, h=h, x=x, y=y, id=id, query_scope="selected", overrides=overrides,
    )


def chain_timeseries(title, *lines, scope, unit, description, id, legend=LIST_LEGEND,
                     thresholds=None, min=0, max=None, decimals=None, axis_label="",
                     fill=6, line_width=2, log=None, color_mode=None, styles=(),
                     transformations=(), w=12, h=8, x=None, y=None, query_scope="selected",
                     overrides=None):
    """A fixed-aggregation panel: chain-wide series, not collapsible per node.

    Every expression here must be chain-scoped by construction - a sum, a median, a ratio
    of fleet totals - because nothing on the panel can name a node. A series that a single
    node owns belongs in fleet_timeseries, which attributes it.
    """
    items = _lines(lines)
    if not (scope or "").strip():
        raise ValueError(f"panel {title!r}: say what it aggregates over, or use agg_timeseries")
    if any(isinstance(item, Attributed) for item in items):
        raise ValueError(f"panel {title!r}: attributes a node, so it is a fleet_timeseries")
    if any("${agg" in item.expr for item in items):
        raise ValueError(f"panel {title!r}: uses ${{agg}}, so it is an agg_timeseries")
    return _timeseries(
        title, description, unit=unit, targets=_targets(lines),
        bands=thresholds, legend=legend, fill=fill, line_width=line_width, log=log,
        axis_label=axis_label, color_mode=color_mode, min=min, max=max, decimals=decimals,
        field_overrides=styles, transformations=transformations,
        w=w, h=h, x=x, y=y, id=id, query_scope=query_scope, overrides=overrides,
    )


def stacked_timeseries(title, *lines, scope, unit, description, id, legend=LIST_LEGEND,
                       thresholds=None, min=0, axis_label="", styles=(),
                       w=12, h=9, x=None, y=None, query_scope="selected", overrides=None):
    """A breakdown whose bands add up: thin lines, generous fill, stacked to the total."""
    return chain_timeseries(
        title, *lines, scope=scope, unit=unit, description=description, id=id, legend=legend,
        thresholds=thresholds, min=min, axis_label=axis_label, styles=styles,
        w=w, h=h, x=x, y=y, query_scope=query_scope,
        overrides=patch({"fieldConfig": {"defaults": {"custom": {
            "fillOpacity": 18, "lineWidth": 1, "stacking": {"group": "A", "mode": "normal"}}}}},
            overrides),
    )


def fleet_timeseries(title, *lines, unit, description, id, transplant=NODE_LABELS,
                     legend=LIST_LEGEND, thresholds=None, min=0, max=None, decimals=None,
                     axis_label="", fill=6, line_width=2, log=None, color_mode=None, styles=(),
                     transformations=(), strip_namespaces=None,
                     w=12, h=8, x=None, y=None, overrides=None):
    """Series collapsed over the fleet, each attributed() one naming the node it came from.

    The fixed-aggregation sibling of agg_timeseries: same hidden ⇒ overlay rows, no $agg
    switch. Plain line()s ride along unattributed, for the chain-scoped context a single
    node cannot own. `transplant` is the label set an overlay joins onto its panel, so a
    board whose series carry more than (job, instance) passes its own once.
    """
    items = _lines(lines)
    keys = [item.key for item in items if isinstance(item, Attributed)]
    if not keys:
        raise ValueError(f"panel {title!r}: nothing attributed(), so it is a chain_timeseries")
    targets, attribution = _attributed_targets(items, transplant)
    strip = "type" in keys if strip_namespaces is None else strip_namespaces
    return _timeseries(
        title, description + hover_note(keys), unit=unit, targets=targets, bands=thresholds,
        legend=legend, fill=fill, line_width=line_width, log=log, axis_label=axis_label,
        color_mode=color_mode, min=min, max=max, decimals=decimals,
        tooltip={"mode": "multi", "sort": "desc", "hoverProximity": 10},
        field_overrides=list(styles) + attribution,
        transformations=([STRIP_NAMESPACES] if strip else []) + list(transformations),
        w=w, h=h, x=x, y=y, id=id, query_scope="mixed", overrides=overrides,
    )


def agg_line(inner, *, key=None, name=None, pick="max", top=""):
    """One series set the $agg selector collapses over nodes, attributed to the node it came from.

    `inner` is the per-(job, instance) reading, carrying `key` too when the panel draws one line
    per key; the panel plots the selector's quantile of it across the nodes exposing it and hides
    the ⇒ node row beside it. `key=None` collapses to one line, which then needs its own `name`;
    `top` restricts the chart to the busiest keys (see the topk fragments in boards).

    `pick="min"` marks a success-type reading, where low is the bad direction. The selector then
    runs the other way - `quantile(1 - $agg)`, so "worst" is the fleet minimum and "best" its
    maximum - and the ⇒ row names the least successful node, as on any min-picked series.
    """
    quantile = "${agg}" if pick == "max" else "1 - ${agg}"
    default_name = " ".join(f"{{{{{label.strip()}}}}}" for label in key.split(",")) if key else None
    return attributed(name or default_name, inner, key=key, pick=pick, top=top,
                      shown=summed(key, f"{quantile}, {inner}", agg="quantile"))


def agg_timeseries(title_base, *lines, unit, description, id, thresholds=None, legend=TABLE_LEGEND,
                   transplant=NODE_LABELS, min=0, axis_label="", fill=6, line_width=2, log=None,
                   styles=(), w=12, h=9, x=None, y=None, strip_namespaces=None, overrides=None):
    """The ▾ panel: agg_line()s the $agg selector collapses, plus any plain line() context.

    Chain-scoped context - a budget, a target - rides along as a plain line(), exactly as on the
    fleet_timeseries this is the switch-following flavour of.
    """
    return fleet_timeseries(
        f"{title_base} (${{agg:text}} node {AGG_MARK})", *lines,
        unit=unit, description=f"{AGG_MARK} {description}", id=id, thresholds=thresholds,
        legend=legend, transplant=transplant, min=min, axis_label=axis_label, fill=fill,
        line_width=line_width, log=log, styles=styles, w=w, h=h, x=x, y=y,
        strip_namespaces=strip_namespaces, overrides=overrides,
    )


def tail_max_panel(title_base, key, inner, *, unit, description, id, top="", thresholds=None,
                   min=0, w=12, h=8, x=None, y=None, strip_namespaces=None, overrides=None):
    """The window="recent" retained-maximum companions: same collapse, worst-case reading.

    A retained maximum expires in cliffs, which can leave an overlay row briefly unjoined; the
    drawn row stays in the tooltip beside it, as on every attributed panel. One reading per key by
    construction, so unlike agg_timeseries it takes that one key and inner rather than agg_line()s.
    """
    return fleet_timeseries(
        f"{title_base}, ${{agg:text}} node {AGG_MARK} (recent)", agg_line(inner, key=key, top=top),
        unit=unit, description=TAIL_PREAMBLE + description, id=id, thresholds=thresholds,
        min=min, w=w, h=h, x=x, y=y,
        strip_namespaces=strip_namespaces, overrides=overrides,
    )


def raw_panel(spec, *, x=None, y=None):
    """Verbatim passthrough. Every use needs a comment saying why no archetype fits."""
    out = copy.deepcopy(spec)
    out.setdefault("gridPos", {}).setdefault("x", x)
    out["gridPos"].setdefault("y", y)
    return out


__all__ = [
    "AGG_MARK", "ATTR", "ATTR_COLOR", "LIST_LEGEND", "NODE", "NODE_LABELS", "SEL", "SELF", "SLOT",
    "FLEET_SPREAD_STYLES", "STRIP_NAMESPACES", "TABLE_LEGEND", "TICKS_TO_SECONDS",
    "TICKS_TO_SECONDS_OVER_RANGE", "WARN",
    "agg_line", "agg_timeseries", "agg_variable", "attributed", "breach_timeline",
    "condition", "fleet_spread", "node_severity", "node_severity_timeline", "triage_table",
    "chain_timeseries", "column", "drill_link", "failure_bands", "fleet_timeseries", "gauge",
    "histogram_quantile",
    "line", "of_target_bands", "patch", "per_node_timeseries", "plain_stat", "rate", "ratio",
    "raw_panel", "recent_max", "reference_line", "rename_by_regex", "scorecard", "sel",
    "same_reporters", "series_style", "slot_variable", "stacked_timeseries", "standard_variables", "summed",
    "summary_column", "summary_table", "table_legend", "tail_max_panel", "threshold_lines",
    "thresholds", "top_keys", "top_peak",
    "top_total", "unstacked_marker", "unstacked_total", "worker_occupancy", "worst_node_stat",
]
