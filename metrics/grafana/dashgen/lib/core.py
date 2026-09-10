"""Grafana JSON primitives: plain dicts in, plain dicts out.

Everything here is generic Grafana/Prometheus plumbing. House style lives in
conventions.py, per-dashboard content one level up.
"""

import copy
import json
import re

__all__ = [
    "ALL", "BUILTIN_ANNOTATION", "DATASOURCE", "GRID_WIDTH", "JOB", "JOB_SEL", "NODE",
    "PLUGIN_VERSION", "REQUIRES", "RUN_ANNOTATION", "SCHEMA_VERSION", "SEL", "SELF_UID",
    "dashboard", "ds",
    "flatten", "nav_link", "pack", "panel", "patch", "ratio", "row", "sel", "target",
    "validate_dashboard", "variable_custom", "variable_datasource", "variable_query",
    "variable_textbox",
]

GRID_WIDTH = 24
SELF_UID = "${__self__}"  # placeholder for "this board", resolved by dashboard()
PLUGIN_VERSION = "12.0.0"
SCHEMA_VERSION = 39
JOB = {"job": "~$job"}  # all reporters in the selected network; never narrowed by Instance
NODE = {**JOB, "instance": "~$instance"}  # reporters selected by the dashboard's node filter
DATASOURCE = {"type": "prometheus", "uid": "${datasource}"}

REQUIRES = [
    {"type": "grafana", "id": "grafana", "name": "Grafana", "version": "10.0.0"},
    {"type": "datasource", "id": "prometheus", "name": "Prometheus", "version": "1.0.0"},
]

BUILTIN_ANNOTATION = {
    "builtIn": 1,
    "datasource": {"type": "grafana", "uid": "-- Grafana --"},
    "enable": True,
    "hide": True,
    "iconColor": "rgba(0, 211, 255, 1)",
    "name": "Annotations & Alerts",
    "type": "dashboard",
}

# Benchmark and local-net runs post Grafana org annotations tagged `ton-run`; every board shows
# them, so any panel can be read against "which run was this".
RUN_ANNOTATION = {
    "name": "Runs",
    "enable": True,
    "hide": False,
    "iconColor": "light-purple",
    "datasource": {"type": "grafana", "uid": "-- Grafana --"},
    "target": {"type": "tags", "matchAny": False, "tags": ["ton-run"], "limit": 100},
}

def ds():
    return dict(DATASOURCE)


MATCHERS = (("!~", "!~"), ("!", "!="), ("~", "=~"))


def sel(**labels):
    """Label matchers in the order written; '~', '!' and '!~' prefixes pick the operator.

    A list value repeats the label (`chain=["master", "~$chain"]`). Callers choose `**NODE`
    for the selected reporters or `**JOB` for chain truth across every reporter in the job.
    """
    def one(name, value):
        for prefix, op in MATCHERS:
            if value.startswith(prefix):
                return f'{name}{op}"{value[len(prefix):]}"'
        return f'{name}="{value}"'

    pairs = ((n, v) for n, vs in labels.items() for v in (vs if isinstance(vs, list) else [vs]))
    return "{" + ",".join(one(n, v) for n, v in pairs) + "}"


SEL = sel(**NODE)
JOB_SEL = sel(**JOB)


def ratio(numerator, denominator):
    """Guarded division: no series where the denominator is zero or absent."""
    return f"({numerator}) / ({denominator} > 0)"


def patch(base, overrides):
    """Deep merge: dicts merge key-wise, every other value replaces."""
    if not overrides:
        return base
    out = copy.deepcopy(base)
    for key, value in overrides.items():
        old = out.get(key)
        mergeable = isinstance(value, dict) and isinstance(old, dict)
        out[key] = patch(old, value) if mergeable else copy.deepcopy(value)
    return out


def nav_link(*, title="TON dashboards", tag="ton"):
    """The dashboards drop-down every board carries in its links bar."""
    return {
        "title": title,
        "type": "dashboards",
        "tags": [tag],
        "asDropdown": True,
        "includeVars": True,
        "keepTime": True,
        "icon": "external link",
        "targetBlank": False,
    }


def target(expr, *, ref_id="A", legend=None, instant=False, range_=None, bare=False, **extra):
    """A Prometheus query. bare=True drops the editor keys (the overlay-target shape)."""
    out = {"refId": ref_id, "expr": expr, "datasource": ds()}
    if not bare:
        out["editorMode"] = "code"
        out["range"] = (not instant) if range_ is None else range_
        out["instant"] = instant
    if legend is not None:
        out["legendFormat"] = legend
    out.update({k: v for k, v in extra.items() if v is not None})
    return out


def panel(type, title, description, *, unit, w, h, targets, id, x=None, y=None,
          defaults=None, field_overrides=(), options=None, overrides=None, query_scope=None,
          **extra):
    """Panel skeleton. `description` is positional because a panel without one ships blind."""
    if not (description or "").strip():
        raise ValueError(f"panel {title!r} has no description")
    body = {
        "id": id,
        "type": type,
        "title": title,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "datasource": ds(),
        "fieldConfig": {
            "defaults": {"unit": unit, **(defaults or {})},
            "overrides": copy.deepcopy(list(field_overrides)),
        },
        "options": copy.deepcopy(options or {}),
        "targets": list(targets),
        "pluginVersion": PLUGIN_VERSION,
        "transparent": False,
        "description": description,
    }
    if query_scope is not None:
        if query_scope not in {"selected", "job", "mixed"}:
            raise ValueError(f"panel {title!r}: invalid query scope {query_scope!r}")
        body["_queryScope"] = query_scope
    body.update(copy.deepcopy(extra))
    return patch(body, overrides)


def row(title, *, id, panels=(), collapsed=False, repeat=None):
    out = {
        "collapsed": collapsed,
        "gridPos": {"h": 1, "w": GRID_WIDTH, "x": None, "y": None},
        "id": id,
        "panels": list(panels),
        "title": title,
        "type": "row",
    }
    if repeat:
        out["repeat"] = repeat
    return out


def flatten(panels):
    """Grafana stores an expanded row's panels as siblings; boards nest them for readability."""
    out = []
    for p in panels:
        if p.get("type") == "row" and not p["collapsed"] and p["panels"]:
            out.append(dict(p, panels=[]))
            out.extend(flatten(p["panels"]))
        else:
            out.append(p)
    return out


def _pack_in_place(panels, top):
    """Flow panels left to right, wrapping at 24 columns; an explicit x or y always wins.

    A row starts below everything placed so far, so a hand-positioned panel that
    reaches further down still pushes the next row out of its way.
    """
    y, x, band, floor = top, 0, 0, top
    for p in panels:
        g = p["gridPos"]
        if p["type"] == "row":
            g["x"], g["y"] = 0, floor
            if p["collapsed"]:
                _pack_in_place(p["panels"], floor + 1)
            y, x, band, floor = floor + 1, 0, 0, floor + 1
            continue
        if g["x"] is None and x + g["w"] > GRID_WIDTH:
            y, x, band = y + band, 0, 0
        x = x if g["x"] is None else g["x"]
        g["x"], g["y"] = x, y if g["y"] is None else g["y"]
        x, band = x + g["w"], max(band, g["h"])
        floor = max(floor, g["y"] + g["h"])
    return panels


def pack(panels, top=0):
    """Return a packed deep copy, leaving reusable module-level panel specifications untouched."""
    return _pack_in_place(copy.deepcopy(list(panels)), top)


_REF_ID = re.compile(r"[A-Z][A-Z0-9]*")
_LABEL_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_LEGEND_LABEL = re.compile(r"{{([^{}]+)}}")
_VARIABLE = re.compile(
    r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)(?::[^}]*)?\}|([A-Za-z_][A-Za-z0-9_]*))"
)


def _walk_panels(panels):
    for item in panels:
        yield item
        yield from _walk_panels(item.get("panels", []))


def _without_descriptions(value):
    """The variable linter checks executable/configuration strings, not prose examples."""
    if isinstance(value, dict):
        return {key: _without_descriptions(item) for key, item in value.items()
                if key != "description"}
    if isinstance(value, list):
        return [_without_descriptions(item) for item in value]
    return value


def _validate_layout(panels, context, *, parent_row=None):
    rectangles = []
    for item in panels:
        label = f"{context}: panel {item.get('id')} {item.get('title')!r}"
        grid = item.get("gridPos", {})
        for key in ("x", "y", "w", "h"):
            if not isinstance(grid.get(key), int):
                raise ValueError(f"{label}: gridPos.{key} must be an integer")
        if grid["w"] <= 0 or grid["h"] <= 0:
            raise ValueError(f"{label}: grid size must be positive")
        if grid["x"] < 0 or grid["x"] + grid["w"] > GRID_WIDTH or grid["y"] < 0:
            raise ValueError(f"{label}: grid position is outside the {GRID_WIDTH}-column canvas")
        if parent_row is not None and grid["y"] < parent_row["gridPos"]["y"] + 1:
            raise ValueError(f"{label}: collapsed-row child starts above its row")
        rectangles.append((item, grid))

    for i, (left, a) in enumerate(rectangles):
        for right, b in rectangles[i + 1:]:
            overlaps = (a["x"] < b["x"] + b["w"] and b["x"] < a["x"] + a["w"]
                        and a["y"] < b["y"] + b["h"] and b["y"] < a["y"] + a["h"])
            if overlaps:
                raise ValueError(
                    f"{context}: panel {left['id']} {left['title']!r} overlaps "
                    f"panel {right['id']} {right['title']!r}"
                )

    for item in panels:
        nested = item.get("panels", [])
        if nested:
            _validate_layout(nested, f"{context} / row {item['title']!r}", parent_row=item)


def validate_dashboard(board, *, require_scope=False):
    """Reject semantic dashboard mistakes that Grafana otherwise accepts silently."""
    uid = board.get("uid", "<unknown>")
    panels = list(_walk_panels(board.get("panels", [])))

    ids = set()
    for item in panels:
        panel_id = item.get("id")
        if not isinstance(panel_id, int) or panel_id <= 0:
            raise ValueError(f"{uid}: panel {item.get('title')!r} has invalid id {panel_id!r}")
        if panel_id in ids:
            raise ValueError(f"{uid}: duplicate panel id {panel_id}")
        ids.add(panel_id)

        refs = set()
        for query in item.get("targets", []):
            ref = query.get("refId")
            if not isinstance(ref, str) or _REF_ID.fullmatch(ref) is None:
                raise ValueError(f"{uid}: panel {panel_id} has invalid refId {ref!r}")
            if ref in refs:
                raise ValueError(f"{uid}: panel {panel_id} has duplicate refId {ref!r}")
            refs.add(ref)
            legend = query.get("legendFormat", "")
            for label in _LEGEND_LABEL.findall(legend):
                if _LABEL_NAME.fullmatch(label) is None:
                    raise ValueError(
                        f"{uid}: panel {panel_id} legend contains invalid label {label!r}"
                    )

        scope = item.get("_queryScope")
        if require_scope and item.get("targets") and scope is None:
            raise ValueError(f"{uid}: panel {panel_id} has no declared query scope")
        if scope == "job" and any("$instance" in query.get("expr", "")
                                  for query in item.get("targets", [])):
            raise ValueError(f"{uid}: job-scoped panel {panel_id} is narrowed by $instance")

    variables = board.get("templating", {}).get("list", [])
    names = [variable.get("name") for variable in variables]
    if any(not isinstance(name, str) or _LABEL_NAME.fullmatch(name) is None for name in names):
        raise ValueError(f"{uid}: variable names must be Prometheus-style identifiers")
    if len(names) != len(set(names)):
        raise ValueError(f"{uid}: duplicate dashboard variable")
    for variable in variables:
        if not str(variable.get("description", "")).strip():
            raise ValueError(f"{uid}: variable {variable['name']!r} has no description")
        if variable.get("type") == "custom":
            selected = variable.get("current", {}).get("value")
            options = {option.get("value") for option in variable.get("options", [])}
            if selected not in options:
                raise ValueError(f"{uid}: variable {variable['name']!r} selects an absent option")

    declared = set(names)
    serialized = json.dumps(_without_descriptions(board))
    referenced = {left or right for left, right in _VARIABLE.findall(serialized)
                  if not (left or right).startswith("__")}
    missing = referenced - declared
    if missing:
        raise ValueError(f"{uid}: undeclared dashboard variable(s): {', '.join(sorted(missing))}")

    _validate_layout(board.get("panels", []), uid)


def _strip_internal_panel_fields(panels):
    for item in panels:
        item.pop("_queryScope", None)
        _strip_internal_panel_fields(item.get("panels", []))


def variable_datasource(name, *, label, description, query="prometheus"):
    return {
        "current": {},
        "description": description,
        "hide": 0,
        "includeAll": False,
        "label": label,
        "multi": False,
        "name": name,
        "options": [],
        "query": query,
        "refresh": 1,
        "regex": "",
        "skipUrlSync": False,
        "type": "datasource",
    }


def variable_query(name, *, label, query, description=None, refresh=2, sort=1, multi=True,
                   include_all=True, extra=None):
    out = {
        "name": name,
        "label": label,
        "type": "query",
        "datasource": ds(),
        "query": {"qryType": 1, "query": query,
                  "refId": "PrometheusVariableQueryEditor-VariableQuery"},
        "refresh": refresh,
        "sort": sort,
        "multi": multi,
        "includeAll": include_all,
        "current": {"text": ["All"], "value": ["$__all"]} if include_all else {},
        "options": [],
        "regex": "",
        "hide": 0,
        "skipUrlSync": False,
    }
    if description is not None:
        out["description"] = description
    return patch(out, extra)


ALL = ("All", "$__all")


def variable_custom(name, *, label, choices, selected, description, include_all=False,
                    all_value=None, multi=False, extra=None):
    """choices: (text, value) pairs, or bare strings when the text is the value."""
    pairs = [(c, c) if isinstance(c, str) else c for c in choices]
    plain = all(text == value for text, value in pairs)
    query = (",".join(t for t, _ in pairs) if plain
             else ", ".join(f"{t} : {v}" for t, v in pairs))
    options = ([ALL] if include_all else []) + pairs
    out = {
        "name": name,
        "label": label,
        "type": "custom",
        "query": query,
        "options": [{"text": t, "value": v, "selected": t == selected} for t, v in options],
        "current": {"text": selected, "value": dict(options)[selected]},
        "includeAll": include_all,
        "multi": multi,
        "hide": 0,
        "description": description,
    }
    if all_value is not None:
        out["allValue"] = all_value
    return patch(out, extra)


def variable_textbox(name, *, label, value, description):
    return {
        "name": name,
        "label": label,
        "type": "textbox",
        "description": description,
        "query": value,
        "current": {"selected": True, "text": value, "value": value},
        "hide": 0,
        "options": [{"selected": True, "text": value, "value": value}],
        "skipUrlSync": False,
    }


def dashboard(uid, title, tags, variables, panels, *, refresh, version,
              time=("now-30m", "now"), timezone="browser", graph_tooltip=1,
              links=None, annotations=(BUILTIN_ANNOTATION, RUN_ANNOTATION),
              schema_version=SCHEMA_VERSION,
              editable=True, live_now=False, fiscal_year_start_month=0, week_start="", extra=None):
    board = patch({
        "uid": uid,
        "title": title,
        "tags": list(tags),
        "timezone": timezone,
        "editable": editable,
        "schemaVersion": schema_version,
        "refresh": refresh,
        "time": {"from": time[0], "to": time[1]},
        "graphTooltip": graph_tooltip,
        "liveNow": live_now,
        "fiscalYearStartMonth": fiscal_year_start_month,
        "weekStart": week_start,
        "version": version,
        "__requires": copy.deepcopy(REQUIRES),
        "annotations": {"list": copy.deepcopy(list(annotations))},
        "links": copy.deepcopy(list(links)) if links is not None else [nav_link()],
        "templating": {"list": copy.deepcopy(list(variables))},
        "panels": pack(flatten(panels)),
    }, extra)
    board = json.loads(json.dumps(board).replace(SELF_UID, uid))
    validate_dashboard(board, require_scope=True)
    _strip_internal_panel_fields(board["panels"])
    return board
