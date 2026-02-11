from datetime import datetime, timezone
from typing import final

import plotly.graph_objects as go  # pyright: ignore[reportMissingTypeStubs]

from ..models import ConsensusData, EventData, SlotData


def to_datetime(t_ms: float) -> datetime:
    return datetime.fromtimestamp(t_ms / 1000, tz=timezone.utc)


def to_relative(t_ms: float, base_ms: float) -> float:
    return round(t_ms - base_ms, 6)


class DataFilter:
    def __init__(self, data: ConsensusData):
        self._data: ConsensusData = data

    def filter_slots(
        self, valgroup_id: str, slot_from: int, slot_to: int, show_empty: bool
    ) -> list[SlotData]:
        return [
            s
            for s in self._data.slots
            if s.valgroup_id == valgroup_id
            and slot_from <= s.slot <= slot_to
            and (show_empty or not s.is_empty)
        ]

    def filter_events(
        self,
        valgroup_id: str | None = None,
        slot: int | None = None,
        slots: set[int] | None = None,
        labels: set[str] | None = None,
        kinds: set[str] | None = None,
        has_validator: bool | None = None,
    ) -> list[EventData]:
        result: list[EventData] = []
        for e in self._data.events:
            if valgroup_id and e.valgroup_id != valgroup_id:
                continue
            if slot is not None and e.slot != slot:
                continue
            if slots and e.slot not in slots:
                continue
            if labels and e.label not in labels:
                continue
            if kinds and e.kind not in kinds:
                continue
            if has_validator is not None:
                if has_validator and e.validator is None:
                    continue
                if not has_validator and e.validator is not None:
                    continue
            result.append(e)
        return result

    def get_slot(self, valgroup_id: str, slot: int) -> SlotData | None:
        for s in self._data.slots:
            if s.valgroup_id == valgroup_id and s.slot == slot:
                return s
        return None

    @staticmethod
    def group_events_by_label(events: list[EventData]) -> dict[str, list[EventData]]:
        result: dict[str, list[EventData]] = {}
        for e in events:
            result.setdefault(e.label, []).append(e)
        return result


@final
class SummaryFigureBuilder:
    def __init__(self, valgroup_id: str, slot_dict: dict[int, SlotData]):
        self._valgroup_id: str = valgroup_id
        self._fig = go.Figure()
        self._slot_dict = slot_dict

    def build(
        self,
        segments: list[EventData],
        markers: list[EventData],
        slot_from: int,
        slot_to: int,
    ) -> go.Figure:
        self._add_bars(segments)
        self._add_markers(markers)
        self._configure_layout(slot_from, slot_to)
        return self._fig

    def _add_bars(self, segments: list[EventData]) -> None:
        events_by_label = DataFilter.group_events_by_label(segments)

        for label in sorted(events_by_label.keys()):
            events = events_by_label[label]
            _ = self._fig.add_trace(  # pyright: ignore[reportUnknownMemberType]
                go.Bar(
                    orientation="h",
                    y=[str(e.slot) for e in events],
                    base=[to_datetime(e.t_ms) for e in events],
                    x=[e.t1_ms - e.t_ms if e.t1_ms else 0 for e in events],
                    name=label,
                    marker=dict(color=events[0].get_color()),
                    customdata=[
                        [
                            self._valgroup_id,
                            e.slot,
                            e.t1_ms - e.t_ms if e.t1_ms else 0,
                            self._slot_dict[e.slot].block_id(),
                        ]
                        for e in events
                    ],
                    hovertemplate=f"valgroup={self._valgroup_id}<br>slot=%{{customdata[1]}}<br>segment={label}<br>start=%{{base|%H:%M:%S.%f}}<br>dt=%{{customdata[2]:.3f}}ms<br>block_id=%{{customdata[3]}}<extra></extra>",
                )
            )

    def _add_markers(self, markers: list[EventData]) -> None:
        markers_by_label: dict[str, list[EventData]] = {}
        for m in markers:
            markers_by_label.setdefault(m.label, []).append(m)

        for label in sorted(markers_by_label.keys()):
            events = markers_by_label[label]
            _ = self._fig.add_trace(  # pyright: ignore[reportUnknownMemberType]
                go.Scatter(
                    x=[to_datetime(e.t_ms) for e in events],
                    y=[str(e.slot) for e in events],
                    mode="markers",
                    marker=dict(
                        # size=11,
                        symbol=[e.get_symbol() for e in events],
                        color=events[0].get_color(),
                    ),
                    name=label,
                    legendgroup=f"m:{label}",
                    customdata=[
                        [
                            self._valgroup_id,
                            e.slot,
                            to_datetime(e.t_ms).strftime("%H:%M:%S.%f"),
                            self._slot_dict[e.slot].block_id(),
                        ]
                        for e in events
                    ],
                    hovertemplate=f"valgroup={self._valgroup_id}<br>slot=%{{customdata[1]}}<br>marker={label}<br>t=%{{customdata[2]}}<br>block_id=%{{customdata[3]}}<extra></extra>",
                )
            )

    def _configure_layout(self, slot_from: int, slot_to: int) -> None:
        _ = self._fig.update_layout(  # pyright: ignore[reportUnknownMemberType]
            title=dict(
                text=f"Summary — valgroup ({self._valgroup_id}) · slots from {slot_from} to {slot_to}",
                xanchor="center",
                yanchor="top",
                x=0.5,
                font=dict(size=14),
            ),
            height=600,
            barmode="overlay",
            bargap=0.25,
            xaxis=dict(title="Time (UTC)", type="date"),
            yaxis=dict(
                title="Slot",
                type="category",
                categoryorder="array",
                categoryarray=[str(s) for s in range(slot_to, slot_from - 1, -1)],
                tickmode="auto",
                nticks=25,
            ),
            dragmode="pan",
        )


@final
class DetailFigureBuilder:
    def __init__(self, valgroup_id: str, slot: SlotData, time_mode: str):
        self._valgroup_id: str = valgroup_id
        self._slot: SlotData = slot
        self._time_mode: str = time_mode
        self._fig = go.Figure()

    def build(
        self,
        events: list[EventData],
        markers: list[EventData],
    ) -> go.Figure:
        self._add_baseline_markers(markers)
        self._add_validator_events(events)
        self._configure_layout(events)
        return self._fig

    def _add_baseline_markers(self, markers: list[EventData]) -> None:
        for m in markers:
            x = (
                to_datetime(m.t_ms)
                if self._time_mode == "abs"
                else to_relative(m.t_ms, self._slot.slot_start_est_ms)
            )

            _ = self._fig.add_vline(x=x, line_width=1, line_dash="dot")  # pyright: ignore[reportUnknownMemberType]
            _ = self._fig.add_trace(  # pyright: ignore[reportUnknownMemberType]
                go.Scatter(
                    x=[x],
                    y=["__slot__"],
                    mode="markers",
                    marker=dict(
                        size=10,
                        symbol=m.get_symbol(),
                        color=m.get_color(),
                    ),
                    name=m.label,
                    legendgroup=f"slot:{m.label}",
                    showlegend=True,
                    customdata=[[m.label, x]],
                    hovertemplate="slot: %{customdata[0]}<br>"
                    + (
                        "t=%{customdata[1]|%H:%M:%S.%f}<br>"
                        if self._time_mode == "abs"
                        else "t=%{customdata[1]}ms<br>"
                    )
                    + "<extra></extra>",
                )
            )

    def _add_validator_events(self, events: list[EventData]) -> None:
        events_by_label = DataFilter.group_events_by_label(events)

        for label in sorted(events_by_label.keys()):
            if label not in (
                "block_validation",
                "finalization",
                "collation",
                "skip_observed",
                "candidate_received",
            ):
                continue
            label_events = events_by_label[label]

            if self._time_mode == "abs":
                base = [to_datetime(e.t_ms) for e in label_events]
                x = [e.t1_ms - e.t_ms if e.t1_ms else 0 for e in label_events]
            else:
                base = [to_relative(e.t_ms, self._slot.slot_start_est_ms) for e in label_events]
                x = [e.t1_ms - e.t_ms if e.t1_ms else 0 for e in label_events]

            kwargs = dict(
                name=label,
                legendgroup=f"ev:{label}",
                customdata=[
                    [
                        self._valgroup_id,
                        self._slot.slot,
                        e.validator,
                        label,
                        e.kind,
                        e.t1_ms - e.t_ms if e.t1_ms else 0,
                        b,
                    ]
                    for e, b in zip(label_events, base)
                ],
            )

            if label not in ("skip_observed", "candidate_received"):
                _ = self._fig.add_trace(  # pyright: ignore[reportUnknownMemberType]
                    go.Bar(
                        orientation="h",
                        base=base,
                        x=x,
                        y=[e.validator for e in label_events],
                        marker=dict(color=label_events[0].get_color()),
                        hovertemplate=(
                            f"valgroup={self._valgroup_id}<br>slot={self._slot.slot}<br>"
                            + f"validator=%{{customdata[2]}}<br>event={label} (kind=%{{customdata[4]}})<br>"
                            + (
                                "start=%{base|%H:%M:%S.%f}<br>"
                                if self._time_mode == "abs"
                                else "start=%{base}ms<br>"
                            )
                            + "dt=%{customdata[5]:.3f}ms<extra></extra>"
                        ),
                        **kwargs,
                    )
                )
            else:
                _ = self._fig.add_trace(  # pyright: ignore[reportUnknownMemberType]
                    go.Scatter(
                        x=base,
                        y=[e.validator for e in label_events],
                        mode="markers",
                        marker=dict(
                            size=10,
                            symbol=label_events[0].get_symbol(),
                            color=label_events[0].get_color(),
                        ),
                        hovertemplate=(
                            f"valgroup={self._valgroup_id}<br>slot={self._slot.slot}<br>"
                            + f"validator=%{{customdata[2]}}<br>event={label} (kind=%{{customdata[4]}})<br>"
                            + (
                                "t=%{customdata[6]|%H:%M:%S.%f}<br><extra></extra>"
                                if self._time_mode == "abs"
                                else "t=%{x}ms<br><extra></extra>"
                            )
                        ),
                        **kwargs,
                    )
                )

    def _configure_layout(
        self,
        events: list[EventData],
    ) -> None:
        title = f"Detail — valgroup ({self._valgroup_id}) slot {self._slot.slot}"
        if self._slot.is_empty:
            title += " · empty"
        if self._slot.collator is not None:
            title += f" · collator={self._slot.collator}"
        if self._slot.block_id_ext:
            title += f"<br>block={self._slot.block_id_ext}"

        validators = sorted({e.validator for e in events if e.validator is not None})
        x_title = "t - slot_start_est (ms)" if self._time_mode == "rel" else "Time (UTC)"

        _ = self._fig.update_layout(  # pyright: ignore[reportUnknownMemberType]
            title=dict(
                text=title,
                xanchor="center",
                yanchor="top",
                x=0.5,
                font=dict(size=14),
            ),
            height=820,
            hovermode="closest",
            barmode="overlay",
            xaxis=dict(
                title=x_title,
                type="date" if self._time_mode == "abs" else "linear",
                rangeslider=dict(visible=True) if self._time_mode == "abs" else None,
            ),
            yaxis=dict(
                title="Validator",
                type="category",
                categoryorder="array",
                categoryarray=["__slot__"] + validators,
            ),
            margin=dict(l=130, r=20, t=60, b=55),
            dragmode="pan",
        )


class FigureBuilder:
    def __init__(self, data: ConsensusData):
        self._filter: DataFilter = DataFilter(data)

    def build_summary(
        self,
        valgroup_id: str,
        slot_from: int,
        slot_to: int,
        show_empty: bool,
    ) -> go.Figure:
        slots = self._filter.filter_slots(valgroup_id, slot_from, slot_to, show_empty)
        slot_set = {s.slot for s in slots}
        slot_dict = {s.slot: s for s in slots}

        segments = self._filter.filter_events(
            valgroup_id=valgroup_id,
            slots=slot_set,
            kinds={"phase"},
        )

        markers = self._filter.filter_events(
            valgroup_id=valgroup_id,
            slots=slot_set,
            has_validator=False,
            kinds={"estimate", "observed"},
        )

        builder = SummaryFigureBuilder(valgroup_id, slot_dict)
        return builder.build(segments, markers, slot_from, slot_to)

    def build_detail(
        self,
        valgroup_id: str,
        slot: int,
        time_mode: str,
    ) -> go.Figure:
        slot_data = self._filter.get_slot(valgroup_id, slot)
        if not slot_data:
            return go.Figure().update_layout(title="No slot selected")  # pyright: ignore[reportUnknownMemberType]

        events = self._filter.filter_events(
            valgroup_id=valgroup_id,
            slot=slot,
            has_validator=True,
        )

        if not events:
            return go.Figure().update_layout(  # pyright: ignore[reportUnknownMemberType]
                title=f"{valgroup_id} slot {slot}: no events"
            )

        markers = self._filter.filter_events(
            valgroup_id=valgroup_id,
            slot=slot,
            has_validator=False,
            kinds={"observed", "reached"},
        )

        builder = DetailFigureBuilder(valgroup_id, slot_data, time_mode)
        return builder.build(events, markers)
