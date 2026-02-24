import threading
from datetime import datetime, timezone
from typing import cast, final
from urllib.parse import parse_qs, urlencode

import plotly.graph_objects as go  # pyright: ignore[reportMissingTypeStubs]
from dash import Dash, Input, NoUpdate, Output, State, callback_context, dcc, html, no_update
from dash.exceptions import PreventUpdate

from ..models import ConsensusData, SlotData, GroupData
from ..parser import GroupParser
from ..validator_set_info import ValidatorSetInfoProvider
from .figure_builder import FigureBuilder


@final
class DashApp:
    def __init__(
        self, parser: GroupParser, vset_info_provider: ValidatorSetInfoProvider | None = None
    ):
        self._parser = parser
        self._vset_provider = vset_info_provider
        self._current_group: str | None = None
        self._data: ConsensusData | None = None
        self._builder: FigureBuilder | None = None
        self._app: Dash = Dash(__name__)
        self._update_lock = threading.Lock()

    def _load_group(self, group: str) -> None:
        with self._update_lock:
            if self._current_group != group or self._builder is None:
                self._current_group = group
                self._data = self._parser.parse_group(group)
                self._builder = FigureBuilder(self._data)

    @classmethod
    def _normalize_time_seconds(cls, value: str | None) -> float | None:
        if value is None:
            return None

        raw_value = value.strip()
        if not raw_value:
            return None

        try:
            return float(raw_value)
        except ValueError:
            pass

        iso_value = f"{raw_value[:-1]}+00:00" if raw_value.endswith("Z") else raw_value
        try:
            parsed_dt = datetime.fromisoformat(iso_value)
        except ValueError:
            return None

        if parsed_dt.tzinfo is None:
            parsed_dt = parsed_dt.replace(tzinfo=timezone.utc)

        return parsed_dt.timestamp()

    def _filter_groups_by_time(
        self,
        groups: list[GroupData],
        time_from: str | None,
        time_until: str | None,
    ) -> list[GroupData]:
        start_from = self._normalize_time_seconds(time_from)
        start_until = self._normalize_time_seconds(time_until)

        filtered: list[GroupData] = []
        for group in groups:
            if start_from is not None and group.group_start_est < start_from:
                continue
            if start_until is not None and group.group_start_est > start_until:
                continue
            filtered.append(group)

        return filtered

    def update_data(
        self,
        href: str | None,
        time_from: str | None,
        time_until: str | None,
    ):
        valgroups = self._parser.list_groups()
        if time_from is not None or time_until is not None:
            valgroups = self._filter_groups_by_time(valgroups, time_from, time_until)
        valgroups_names = sorted([g.valgroup_name for g in valgroups])
        options = [{"label": g, "value": g} for g in valgroups_names]

        url_params = self._parse_url_params(href)
        if "valgroup_id" in url_params and url_params["valgroup_id"] in valgroups_names:
            value = str(url_params["valgroup_id"])
        # elif current_group and current_group in valgroups:
        #     value = current_group
        else:
            value = valgroups_names[0] if valgroups_names else ""

        return options, value

    @staticmethod
    def _parse_url_params(href: str | None) -> dict[str, str | int]:
        if not href or "?" not in href:
            return {}
        query_string = href.split("?", 1)[1]
        params = parse_qs(query_string)
        result: dict[str, str | int] = {}
        if "valgroup_id" in params:
            result["valgroup_id"] = params["valgroup_id"][0]
        if "slot" in params:
            try:
                result["slot"] = int(params["slot"][0])
            except (TypeError, ValueError):
                pass
        return result

    @staticmethod
    def _build_url_search(valgroup_id: str | None, slot: int | None) -> str:
        params: dict[str, str] = {}
        if valgroup_id:
            params["valgroup_id"] = valgroup_id
        if slot is not None:
            params["slot"] = str(slot)
        return f"?{urlencode(params)}" if params else ""

    def _normalize_slot_range(
        self,
        group: str,
        slot_from: int | None,
        slot_to: int | None,
    ) -> tuple[int, int]:
        slot_from = slot_from or 0
        slot_to = slot_to or slot_from

        self._load_group(group)
        assert self._data is not None
        max_slot = max((s.slot for s in self._data.slots), default=0)
        slot_from = max(0, min(int(slot_from), max_slot))
        slot_to = max(0, min(int(slot_to), max_slot))
        if slot_to < slot_from:
            slot_to = slot_from

        return slot_from, slot_to

    @classmethod
    def _selected_from_url(cls, href: str | None) -> dict[str, str | int] | None:
        url_params = cls._parse_url_params(href)
        valgroup_id = url_params.get("valgroup_id")
        slot = url_params.get("slot")
        if isinstance(valgroup_id, str) and isinstance(slot, int):
            return {"valgroup_id": valgroup_id, "slot": slot}
        return None

    def _update_group_validator_set(self, group: str | None) -> str:
        if not group or not self._vset_provider:
            return ""
        self._load_group(group)
        assert self._data is not None
        return self._vset_provider.get_validator_set_text(group, self._data.slots)

    def _update_group_stats(self, group: str | None) -> str:
        if not group:
            return "group stats: none"

        self._load_group(group)
        assert self._data is not None

        group_events = [e for e in self._data.events if e.valgroup_id == group]
        group_slots = {s.slot: s for s in self._data.slots if s.valgroup_id == group}

        skip_slots = {e.slot for e in group_events if e.label == "skip_observed"}
        finalized_block_slots = sorted(
            slot
            for slot in {e.slot for e in group_events if e.label == "finalize_reached"}
            if slot in group_slots and not group_slots[slot].is_empty
        )

        min_candidate_received_by_slot: dict[int, float] = {}
        for e in group_events:
            if e.label != "candidate_received":
                continue
            known_ts = min_candidate_received_by_slot.get(e.slot)
            if known_ts is None or e.t_ms < known_ts:
                min_candidate_received_by_slot[e.slot] = e.t_ms

        finalized_with_candidate_received = [
            (slot, min_candidate_received_by_slot[slot])
            for slot in finalized_block_slots
            if slot in min_candidate_received_by_slot
        ]
        delta_entries = [
            (prev_slot, cur_slot, cur_t - prev_t)
            for (prev_slot, prev_t), (cur_slot, cur_t) in zip(
                finalized_with_candidate_received, finalized_with_candidate_received[1:]
            )
        ]

        avg_delta = (sum(x[2] for x in delta_entries) / len(delta_entries)) if delta_entries else None
        min_delta_slots = min(delta_entries, key=lambda x: x[2]) if delta_entries else None
        max_delta_slots = max(delta_entries, key=lambda x: x[2]) if delta_entries else None

        min_slot_text = (
            f"{min_delta_slots[0]} -> {min_delta_slots[1]}" if min_delta_slots else "n/a"
        )
        max_slot_text = (
            f"{max_delta_slots[0]} -> {max_delta_slots[1]}" if max_delta_slots else "n/a"
        )

        return "\n".join(
            [
                f"valgroup = {group}",
                (
                    "slots with skip_observed"
                    f" ({len(skip_slots)}) = {skip_slots}"
                ),
                "delta between minimum candidate_received for neighboring finalized blocks:",
                f"min = {round(min_delta_slots[2], 3) if min_delta_slots else 'n/a'} ms for slots {min_slot_text}",
                f"avg = {avg_delta:.3f} ms" if avg_delta is not None else "avg = n/a",
                f"max = {round(max_delta_slots[2], 3) if max_delta_slots else 'n/a'} ms for slots {max_slot_text}",
            ]
        )

    def run(self, debug: bool = True, host: str = "127.0.0.1", port: int = 8050) -> None:
        self._setup_layout()
        self._setup_callbacks()
        self._app.run(debug=debug, host=host, port=port, use_reloader=False)  # pyright: ignore[reportUnknownMemberType]

    def _setup_layout(self) -> None:
        self._app.title = "Consensus Explorer"
        self._app.layout = html.Div(
            [
                dcc.Location(id="url", refresh=False),
                html.Div(
                    [
                        html.H1(
                            "Consensus Explorer",
                            # style={'textAlign': 'center'},
                        ),
                        html.Div(
                            "Click a slot in Summary to open per-validator Detail.",
                            # style={"color": "#555", "fontSize": "13px"},
                        ),
                    ],
                    style={"padding": "12px 16px", "borderBottom": "1px solid #ddd"},
                ),
                html.Div(
                    [
                        html.Div(
                            [
                                "Valgroup: ",
                                dcc.Dropdown(
                                    id="group",
                                    clearable=False,
                                ),
                            ],
                            style={"flex": "auto", "minWidth": 0},
                        ),
                        html.Label(
                            [
                                "Time from: ",
                                dcc.Input(
                                    id="time-from",
                                    type="text",
                                    debounce=True,
                                    placeholder="unix or ISO",
                                    style={"width": "100px"},
                                ),
                            ]
                        ),
                        html.Label(
                            [
                                "Time until: ",
                                dcc.Input(
                                    id="time-until",
                                    type="text",
                                    debounce=True,
                                    placeholder="unix or ISO",
                                    style={"width": "100px"},
                                ),
                            ]
                        ),
                        html.Label(
                            [
                                "Slot from: ",
                                dcc.Input(
                                    id="slot-from",
                                    type="number",
                                    value=0,
                                    min=0,
                                    step=1,
                                    style={"width": "110px"},
                                ),
                            ]
                        ),
                        html.Label(
                            [
                                "Slot to: ",
                                dcc.Input(
                                    id="slot-to",
                                    type="number",
                                    value=1000,
                                    min=0,
                                    step=1,
                                    style={"width": "110px"},
                                ),
                            ]
                        ),
                        html.Label(
                            [
                                dcc.Checklist(
                                    id="show-empty",
                                    options=[{"label": " show empty slots", "value": "yes"}],
                                    value=["yes"],
                                    style={"marginLeft": "8px"},
                                )
                            ]
                        ),
                        html.Div(
                            id="selection",
                            style={
                                "marginLeft": "12px",
                                "fontSize": "12px",
                                "border": "1px solid #ddd",
                                "borderRadius": "999px",
                                "padding": "2px 10px",
                            },
                        ),
                    ],
                    style={
                        "display": "flex",
                        "gap": "12px",
                        "flexWrap": "wrap",
                        "alignItems": "center",
                        "padding": "12px 16px",
                    },
                ),
                html.Div(
                    [
                        html.Pre(
                            id="group-stats",
                            children="group stats: none",
                            style={
                                "margin": "0",
                                "fontSize": "14px",
                                "border": "1px solid #ddd",
                                "borderRadius": "4px",
                                "padding": "8px 10px",
                                "whiteSpace": "pre-wrap",
                                "overflowX": "auto",
                            },
                        )
                    ],
                    style={
                        "margin": "0 16px 10px 16px",
                    },
                ),
                html.Div(
                    [
                        html.Pre(
                            id="group-validator-set",
                            children="validator set info: none",
                            style={
                                "margin": "0",
                                "fontSize": "14px",
                                "border": "1px solid #ddd",
                                "borderRadius": "4px",
                                "padding": "8px 10px",
                                "whiteSpace": "pre-wrap",
                                "overflowX": "auto",
                            },
                        )
                    ],
                    style={
                        "margin": "0 16px 10px 16px",
                    },
                ),
                dcc.Store(id="selected", data={"valgroup_id": "", "slot": 0}),
                dcc.Graph(id="summary", config={"scrollZoom": True, "displaylogo": False}),
                html.Hr(),
                html.Div(
                    id="detail-slot-meta",
                    children="detail slot metadata: none",
                    style={
                        "margin": "0 16px 8px 16px",
                        "fontSize": "14px",
                        "fontFamily": "monospace",
                        "border": "1px solid #ddd",
                        "borderRadius": "4px",
                        "padding": "8px 10px",
                        "whiteSpace": "pre-wrap",
                        "wordBreak": "break-all",
                    },
                ),
                html.Div(
                    [
                        html.Label(
                            [
                                "Detail time mode: ",
                                dcc.RadioItems(
                                    id="time-mode",
                                    options=[
                                        {"label": " absolute", "value": "abs"},
                                        {"label": " relative", "value": "rel"},
                                    ],
                                    value="abs",
                                    inline=True,
                                ),
                            ]
                        ),
                        html.Div(
                            [
                                html.Button(
                                    "← Previous Slot",
                                    id="prev-slot-btn",
                                    style={
                                        "padding": "6px 12px",
                                        "cursor": "pointer",
                                        "backgroundColor": "#f0f0f0",
                                        "border": "1px solid #ccc",
                                        "borderRadius": "4px",
                                    },
                                ),
                                html.Button(
                                    "Next Slot →",
                                    id="next-slot-btn",
                                    style={
                                        "padding": "6px 12px",
                                        "cursor": "pointer",
                                        "backgroundColor": "#f0f0f0",
                                        "border": "1px solid #ccc",
                                        "borderRadius": "4px",
                                    },
                                ),
                            ],
                            style={
                                "display": "flex",
                                "gap": "8px",
                                "marginLeft": "auto",
                            },
                        ),
                    ],
                    style={
                        "display": "flex",
                        "gap": "16px",
                        "flexWrap": "wrap",
                        "alignItems": "center",
                        "padding": "0 16px 10px 16px",
                    },
                ),
                dcc.Graph(id="detail", config={"scrollZoom": True, "displaylogo": False}),
                html.Div(style={"height": "12px"}),
            ],
            style={"maxWidth": "1500px", "margin": "0 auto"},
        )

    @staticmethod
    def _format_slot_meta(
        valgroup_id: str,
        slot: int,
        slot_data: SlotData | None,
    ) -> str:
        if not valgroup_id:
            return "detail slot metadata: none"

        if slot_data is None:
            return f"valgroup = {valgroup_id} slot = {slot} not found"

        parts = [
            f"valgroup = {slot_data.valgroup_id}",
            f"slot = {slot_data.slot}",
            f"empty = {'yes' if slot_data.is_empty else 'no'}",
        ]
        if slot_data.collator is not None:
            parts.append(f"collator = {slot_data.collator}")
        if slot_data.block_id_ext:
            parts.append(f"block = {slot_data.block_id_ext}")
        if slot_data.candidate_id:
            parts.append(f"candidate_id = {slot_data.candidate_id}")
        if slot_data.parent_block:
            parts.append(f"parent_block = {slot_data.parent_block}")

        return "\n".join(parts)

    def _update_selection_from_click(
        self,
        clickData: dict[str, list[dict[str, int | dict[str, int] | list[str | int]]]] | None,
        group: str,
    ) -> dict[str, str | int]:
        if not clickData:
            raise PreventUpdate

        cd = clickData["points"][0].get("customdata")
        if cd and isinstance(cd, list) and len(cd) >= 2 and cd[0] == group:
            return {"valgroup_id": group, "slot": int(cd[1])}

        raise PreventUpdate

    def _update_selected(
        self,
        href: str | None,
        group: str | None,
        clickData: dict[str, list[dict[str, int | dict[str, int] | list[str | int]]]] | None,
        prev_clicks: int | None,
        next_clicks: int | None,
        selected: dict[str, str | int] | None,
    ) -> dict[str, str | int]:
        if not group:
            raise PreventUpdate

        selected = selected or {"valgroup_id": "", "slot": 0}

        ctx = callback_context
        triggered_id = cast(str, ctx.triggered_id)
        assert isinstance(triggered_id, str)

        if triggered_id == "summary":
            return self._update_selection_from_click(clickData, group)

        if triggered_id in ("prev-slot-btn", "next-slot-btn"):
            return self._navigate_slot(prev_clicks, next_clicks, selected)

        url_selected = self._selected_from_url(href)
        if url_selected and url_selected["valgroup_id"] == group and url_selected != selected:
            return {"valgroup_id": group, "slot": int(url_selected["slot"])}

        if selected.get("valgroup_id") != group:
            return {"valgroup_id": group, "slot": 0}

        raise PreventUpdate

    def _update_summary(
        self,
        group: str | None,
        slot_from: int | None,
        slot_to: int | None,
        time_from: str | None,
        time_until: str | None,
        show_empty_v: list[str] | None,
    ) -> go.Figure:
        if not group:
            raise PreventUpdate

        slot_from, slot_to = self._normalize_slot_range(group, slot_from, slot_to)
        show_empty = "yes" in (show_empty_v or [])
        start_from = self._normalize_time_seconds(time_from)
        start_until = self._normalize_time_seconds(time_until)
        assert self._builder is not None
        return self._builder.build_summary(
            group, slot_from, slot_to, show_empty, start_from, start_until
        )

    def _update_detail(
        self,
        selected: dict[str, str | int],
        time_mode: str,
    ) -> tuple[go.Figure, str, str, str | NoUpdate]:
        valgroup_id = selected["valgroup_id"]
        assert isinstance(valgroup_id, str)
        slot = int(selected["slot"])

        self._load_group(valgroup_id)
        assert self._builder is not None
        fig = self._builder.build_detail(valgroup_id, slot, time_mode)
        slot_data = self._builder.get_slot(valgroup_id, slot)
        slot_meta = self._format_slot_meta(valgroup_id, slot, slot_data)
        ctx = callback_context
        triggered_id = cast(str | None, ctx.triggered_id)

        if not valgroup_id or triggered_id in ("time-mode", "time-from", "time-until"):
            return fig, f"selected: {valgroup_id} slot {slot}", slot_meta, no_update

        return (
            fig,
            f"selected: {valgroup_id} slot {slot}",
            slot_meta,
            self._build_url_search(valgroup_id, slot),
        )

    def _navigate_slot(
        self,
        _prev: int | None,
        _next: int | None,
        selected: dict[str, str | int],
    ) -> dict[str, str | int]:
        ctx = callback_context
        if not ctx.triggered:
            raise PreventUpdate

        triggered_id = cast(str, ctx.triggered_id)
        assert isinstance(triggered_id, str)
        direction = -1 if triggered_id == "prev-slot-btn" else 1

        current_idx = int(selected["slot"])
        new_idx = current_idx + direction

        if 0 <= new_idx:
            return {"valgroup_id": selected["valgroup_id"], "slot": new_idx}

        raise PreventUpdate

    def _setup_callbacks(self) -> None:
        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("group", "options"),
            Output("group", "value"),
            Input("url", "href"),
            Input("time-from", "value"),
            Input("time-until", "value"),
        )(self.update_data)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("group-stats", "children"),
            Input("group", "value"),
        )(self._update_group_stats)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("group-validator-set", "children"),
            Input("group", "value"),
        )(self._update_group_validator_set)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("selected", "data"),
            Input("url", "href"),
            Input("group", "value"),
            Input("summary", "clickData"),
            Input("prev-slot-btn", "n_clicks"),
            Input("next-slot-btn", "n_clicks"),
            State("selected", "data"),
        )(self._update_selected)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("summary", "figure"),
            Input("group", "value"),
            Input("slot-from", "value"),
            Input("slot-to", "value"),
            Input("time-from", "value"),
            Input("time-until", "value"),
            Input("show-empty", "value"),
        )(self._update_summary)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("detail", "figure"),
            Output("selection", "children"),
            Output("detail-slot-meta", "children"),
            Output("url", "search"),
            Input("selected", "data"),
            Input("time-mode", "value"),
        )(self._update_detail)
