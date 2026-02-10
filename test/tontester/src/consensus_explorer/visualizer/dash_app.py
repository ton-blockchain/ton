from typing import cast, final
from urllib.parse import parse_qs, urlencode

from dash import Dash, dcc, html, Input, Output, State, callback_context, no_update, NoUpdate
import plotly.graph_objects as go  # pyright: ignore[reportMissingTypeStubs]
from dash.exceptions import PreventUpdate

from .figure_builder import FigureBuilder
from ..parser import Parser


@final
class DashApp:
    def __init__(self, parser: Parser):
        self._parser = parser
        self._data = self._parser.parse()
        self._builder: FigureBuilder = FigureBuilder(self._data)
        self._app: Dash = Dash(__name__)

    def update_data(self, href: str | None):
        self._data = self._parser.parse()
        self._builder = FigureBuilder(self._data)
        valgroups = sorted(set(s.valgroup_id for s in self._data.slots))
        options = [{"label": g, "value": g} for g in valgroups]

        url_params = self._parse_url_params(href)
        if "valgroup_id" in url_params and url_params["valgroup_id"] in valgroups:
            value = str(url_params["valgroup_id"])
        else:
            value = valgroups[0] if valgroups else ""

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

        group_slots = [s for s in self._data.slots if s.valgroup_id == group]
        max_slot = max((s.slot for s in group_slots), default=0)
        slot_from = max(0, min(int(slot_from), max_slot))
        slot_to = max(0, min(int(slot_to), max_slot))
        if slot_to < slot_from:
            slot_to = slot_from

        return slot_from, slot_to

    def _pick_default_selected_for_group(
        self,
        group: str,
        slot_from: int | None,
        slot_to: int | None,
        show_empty_v: list[str] | None,
    ) -> dict[str, str | int]:
        slot_from, slot_to = self._normalize_slot_range(group, slot_from, slot_to)
        show_empty = "yes" in (show_empty_v or [])

        slots = [
            s
            for s in self._data.slots
            if s.valgroup_id == group
            and slot_from <= s.slot <= slot_to
            and (show_empty or not s.is_empty)
        ]

        if not slots:
            return {"valgroup_id": group, "slot": slot_from}

        non_empty = [s for s in slots if not s.is_empty]
        pick = non_empty[0].slot if non_empty else slots[0].slot
        return {"valgroup_id": group, "slot": pick}

    @classmethod
    def _selected_from_url(cls, href: str | None) -> dict[str, str | int] | None:
        url_params = cls._parse_url_params(href)
        valgroup_id = url_params.get("valgroup_id")
        slot = url_params.get("slot")
        if isinstance(valgroup_id, str) and isinstance(slot, int):
            return {"valgroup_id": valgroup_id, "slot": slot}
        return None

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
                        html.Label(
                            [
                                "Valgroup: ",
                                dcc.Dropdown(
                                    id="group",
                                    clearable=False,
                                    style={"minWidth": "220px"},
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
                                    value=80,
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
                dcc.Store(id="selected", data={"valgroup_id": "", "slot": 0}),
                dcc.Graph(id="summary", config={"scrollZoom": True, "displaylogo": False}),
                html.Hr(),
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
            picked = self._pick_default_selected_for_group(group, slot_from, slot_to, show_empty_v)
            if picked != selected:
                return picked

        raise PreventUpdate

    def _update_summary(
        self,
        group: str | None,
        slot_from: int | None,
        slot_to: int | None,
        show_empty_v: list[str] | None,
    ) -> go.Figure:
        if not group:
            raise PreventUpdate

        slot_from, slot_to = self._normalize_slot_range(group, slot_from, slot_to)
        show_empty = "yes" in (show_empty_v or [])
        return self._builder.build_summary(group, slot_from, slot_to, show_empty)

    def _update_detail(
        self,
        selected: dict[str, str | int],
        time_mode: str,
    ) -> tuple[go.Figure, str, str | NoUpdate]:
        valgroup_id = selected["valgroup_id"]
        assert isinstance(valgroup_id, str)
        slot = int(selected["slot"])

        fig = self._builder.build_detail(valgroup_id, slot, time_mode)
        ctx = callback_context
        triggered_id = cast(str | None, ctx.triggered_id)

        if not valgroup_id or triggered_id == "time-mode":
            return fig, f"selected: {valgroup_id} slot {slot}", no_update

        return (
            fig,
            f"selected: {valgroup_id} slot {slot}",
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
        )(self.update_data)

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
            Input("show-empty", "value"),
        )(self._update_summary)

        self._app.callback(  # pyright: ignore[reportUnknownMemberType]
            Output("detail", "figure"),
            Output("selection", "children"),
            Output("url", "search"),
            Input("selected", "data"),
            Input("time-mode", "value"),
        )(self._update_detail)
