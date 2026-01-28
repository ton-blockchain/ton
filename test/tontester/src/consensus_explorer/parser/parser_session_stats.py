import dataclasses
from pathlib import Path
from typing import final, override, Callable

from tonapi.ton_api import (
    Consensus_stats_events,
    Consensus_stats_timestampedEvent,
    Consensus_stats_id,
    Consensus_simplex_stats_voted,
    Consensus_simplex_stats_certObserved,
    Consensus_simplex_skipVote,
    TypeConsensus_stats_Event,
    Consensus_stats_collateStarted,
    Consensus_stats_validationStarted,
    Consensus_stats_validationFinished,
    Consensus_stats_collateFinished,
    Consensus_stats_collatedEmpty,
    Consensus_stats_candidateReceived,
    Consensus_simplex_notarizeVote,
    Consensus_simplex_finalizeVote,
)


from .parser_base import Parser
from ..models import ConsensusData, SlotData, EventData

type slot_id_type = tuple[str, int]


@dataclasses.dataclass
class VoteData:
    t_ms: float
    v_id: int
    weight: int


TARGET_TO_LABEL = {
    Consensus_stats_candidateReceived: "candidate_received",
    Consensus_stats_collateStarted: "collate_started",
    Consensus_stats_collateFinished: "collate_finished",
    Consensus_stats_collatedEmpty: "collate_finished",
    Consensus_stats_validationStarted: "validate_started",
    Consensus_stats_validationFinished: "validate_finished",
}


@final
class ParserSessionStats(Parser):
    def __init__(self, logs_path: list[Path]):
        self._logs_path = logs_path
        self._slots: dict[slot_id_type, SlotData] = {}
        self._collated: dict[slot_id_type, dict[str, EventData]] = {}
        self._votes: dict[slot_id_type, dict[str, list[VoteData]]] = {}
        self._total_weights: dict[str, int] = {}
        self._slot_leaders: dict[slot_id_type, int] = {}
        self._slot_events: dict[slot_id_type, dict[int, dict[str, EventData]]] = {}
        self._events: list[EventData] = []

    def _get_create_slot(self, slot: int, v_group: str) -> SlotData:
        slot_id = (v_group, slot)

        if slot_id not in self._slots:
            self._slots[slot_id] = SlotData(
                valgroup_id=v_group,
                slot=slot,
                is_empty=False,
                slot_start_est_ms=float("inf"),
                block_id_ext=None,
                collator=None,
            )

        return self._slots[slot_id]

    def _parse_stats_event(
        self,
        event: TypeConsensus_stats_Event,
        t_ms: float,
        v_group: str,
        v_id: int,
    ):
        if not isinstance(event, tuple(TARGET_TO_LABEL.keys())):
            return
        if isinstance(event, (Consensus_stats_collateStarted, Consensus_stats_collateFinished)):
            slot = event.target_slot
        else:
            assert event.id is not None, f"id is None for {event}"
            slot = event.id.slot
        assert slot is not None, f"Slot is None for {event}"

        slot_id = (v_group, slot)

        slot_data = self._get_create_slot(slot, v_group)

        slot_data.slot_start_est_ms = min(t_ms, slot_data.slot_start_est_ms)

        if isinstance(event, Consensus_stats_collateStarted):
            slot_data.collator = v_id

        label = TARGET_TO_LABEL[type(event)]
        ev = EventData(
            valgroup_id=v_group,
            slot=slot,
            label=label,
            kind="local",
            t_ms=t_ms,
            validator=v_id,
            t1_ms=None,
        )
        self._slot_events.setdefault(slot_id, {}).setdefault(v_id, {})[label] = ev

        if isinstance(event, Consensus_stats_collateFinished):
            assert event.block is not None
            slot_data.block_id_ext = f"({event.block.workchain},{self._shard_to_hex(event.block.shard)},{event.block.seqno}):{event.block.root_hash.hex().upper()}:{event.block.file_hash.hex().upper()}"

        if label == "candidate_received":
            self._events.append(ev)

        if label in ("collate_started", "collate_finished"):
            self._collated.setdefault(slot_id, {})[label] = ev

    def _parse_cert_observed(
        self,
        event: Consensus_simplex_stats_certObserved,
        t_ms: float,
        v_group: str,
        v_id: int,
        get_slot_leader: Callable[[int], int],
    ):
        assert event.vote is not None
        vote = event.vote
        if isinstance(vote, Consensus_simplex_skipVote):
            slot = vote.slot
            self._events.append(
                EventData(
                    valgroup_id=v_group,
                    slot=slot,
                    label="skip_observed",
                    kind="local",
                    validator=v_id,
                    t_ms=t_ms,
                )
            )

            slot_data = self._get_create_slot(slot, v_group)
            slot_data.is_empty = True
        else:
            assert vote.id is not None
            slot = vote.id.slot

        label = (
            "notarize_observed"
            if isinstance(vote, Consensus_simplex_notarizeVote)
            else "finalize_observed"
        )
        self._slot_events.setdefault((v_group, slot), {}).setdefault(v_id, {})[label] = EventData(
            valgroup_id=v_group,
            slot=slot,
            label=label,
            kind="local",
            validator=v_id,
            t_ms=t_ms,
        )

        if isinstance(vote, Consensus_simplex_finalizeVote) and get_slot_leader(slot + 1) == v_id:
            self._events.append(
                EventData(
                    valgroup_id=v_group,
                    slot=slot,
                    label="finalize_observed_by_next_leader",
                    kind="observed",
                    t_ms=t_ms,
                    t1_ms=None,
                )
            )

    def _parse_vote_event(
        self,
        event: Consensus_simplex_stats_voted,
        t_ms: float,
        v_group: str,
        v_id: int,
        v_weight: int,
    ):
        vote = event.vote
        assert vote is not None
        if isinstance(vote, Consensus_simplex_skipVote):
            slot = vote.slot
        else:
            assert vote.id is not None
            slot = vote.id.slot
        slot_id = (v_group, slot)
        vote_type = {Consensus_simplex_notarizeVote: "notarize_vote", Consensus_simplex_finalizeVote: "finalize_vote", Consensus_simplex_skipVote: "skip_vote"}[type(vote)]
        self._votes.setdefault(slot_id, {}).setdefault(vote_type, []).append(
            VoteData(t_ms=t_ms, v_id=v_id, weight=v_weight)
        )
        self._events.append(
            EventData(
                valgroup_id=v_group,
                slot=slot,
                label=vote_type,
                kind="local",
                t_ms=t_ms,
                validator=v_id,
                t1_ms=None,
            )
        )

    def _infer_slot_events(self) -> None:
        for s in self._slot_events.values():
            for events in s.values():
                for start_event_name, end_event_name, label in (
                    ("collate_started", "collate_finished", "collation"),
                    ("validate_started", "validate_finished", "block_validation"),
                    ("notarize_observed", "finalize_observed", "finalization"),
                ):
                    if start_event_name not in events or end_event_name not in events:
                        continue
                    e = events[start_event_name]
                    end_event = events[end_event_name]
                    self._events.append(
                        EventData(
                            valgroup_id=e.valgroup_id,
                            slot=e.slot,
                            validator=e.validator,
                            label=label,
                            kind=e.kind,
                            t_ms=e.t_ms,
                            t1_ms=end_event.t_ms,
                        )
                    )
                    if (
                        start_event_name == "collate_started"
                    ):  # if collation was less than 1 ms add points collate started / ended to detailgraph
                        if end_event.t_ms - e.t_ms <= 1:
                            self._events.append(e)
                            self._events.append(end_event)

    def _infer_slot_phases(self):
        for slot_id, slot_data in self._slots.items():
            self._events.append(
                EventData(
                    valgroup_id=slot_data.valgroup_id,
                    slot=slot_data.slot,
                    label="slot_start_est",
                    kind="estimate",
                    t_ms=slot_data.slot_start_est_ms,
                )
            )

            collate_start = None
            collate_end = None
            if (
                slot_id in self._collated
                and "collate_started" in self._collated[slot_id]
                and "collate_finished" in self._collated[slot_id]
            ):
                collate_start = self._collated[slot_id]["collate_started"].t_ms
                collate_end = self._collated[slot_id]["collate_finished"].t_ms
                self._events.append(
                    EventData(
                        valgroup_id=slot_data.valgroup_id,
                        slot=slot_data.slot,
                        label="collate",
                        kind="phase",
                        t_ms=collate_start,
                        t1_ms=collate_end,
                    )
                )

            total_weight = self._total_weights[slot_id[0]]
            weight_threshold = (total_weight * 2) // 3 + 1

            notarize_reached = None
            if (
                slot_id in self._votes
                and "skip_vote" in self._votes[slot_id]
            ):
                _ = self._process_vote_threshold(
                    slot_data=slot_data,
                    votes=self._votes[slot_id]["skip_vote"],
                    weight_threshold=weight_threshold,
                    label="skip",
                    phase_start=None,
                )
            if (
                slot_id in self._votes
                and "notarize_vote" in self._votes[slot_id]
            ):
                notarize_reached = self._process_vote_threshold(
                    slot_data=slot_data,
                    votes=self._votes[slot_id]["notarize_vote"],
                    weight_threshold=weight_threshold,
                    label="notarize",
                    phase_start=collate_end,
                )

            if (
                slot_id in self._votes
                and "finalize_vote" in self._votes[slot_id]
            ):
                _ = self._process_vote_threshold(
                    slot_data=slot_data,
                    votes=self._votes[slot_id]["finalize_vote"],
                    weight_threshold=weight_threshold,
                    label="finalize",
                    phase_start=notarize_reached,
                )

    def _process_vote_threshold(
        self,
        slot_data: SlotData,
        votes: list[VoteData],
        weight_threshold: int,
        label: str,
        phase_start: float | None,
    ) -> float | None:
        current_weight = 0
        sorted_votes = sorted(votes, key=lambda x: x.t_ms)

        for vote in sorted_votes:
            current_weight += vote.weight
            if current_weight >= weight_threshold:
                self._events.append(
                    EventData(
                        valgroup_id=slot_data.valgroup_id,
                        slot=slot_data.slot,
                        label=f"{label}_reached",
                        kind="reached",
                        t_ms=vote.t_ms,
                        validator=None,
                        t1_ms=None,
                    )
                )
                if phase_start is not None:
                    self._events.append(
                        EventData(
                            valgroup_id=slot_data.valgroup_id,
                            slot=slot_data.slot,
                            label=label,
                            kind="phase",
                            t_ms=phase_start,
                            t1_ms=vote.t_ms,
                        )
                    )
                return vote.t_ms

        return None

    @staticmethod
    def _shard_to_hex(sh: int) -> str:
        return f"{sh & 0xFFFFFFFFFFFFFFFF:016x}"

    def _process_group_events(
        self,
        events: list[Consensus_stats_timestampedEvent],
    ):
        event_id: Consensus_stats_id | None = None
        for e in events:
            if isinstance(e.event, Consensus_stats_id):
                event_id = e.event
                break
        assert event_id, "Event ID not found in events list"

        v_group = f"{event_id.workchain},{self._shard_to_hex(event_id.shard)}.{event_id.cc_seqno}"

        v_id = event_id.idx
        v_weight = event_id.weight
        self._total_weights[v_group] = event_id.total_weight

        def get_slot_leader(slot: int):
            return slot // event_id.slots_per_leader_window % event_id.total_validators

        for e in events:
            ev = e.event
            assert ev is not None
            t_ms = e.ts * 1000
            if isinstance(ev, Consensus_simplex_stats_voted):
                self._parse_vote_event(ev, t_ms, v_group, v_id, v_weight)
            elif isinstance(ev, Consensus_simplex_stats_certObserved):
                self._parse_cert_observed(ev, t_ms, v_group, v_id, get_slot_leader)
            else:
                self._parse_stats_event(ev, t_ms, v_group, v_id)

    def _clear_state(self) -> None:
        self._slots = {}
        self._collated = {}
        self._votes = {}
        self._total_weights = {}
        self._slot_leaders = {}
        self._slot_events = {}
        self._events = []

    @override
    def parse(self) -> ConsensusData:
        for log_file in self._logs_path:
            with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
                events_by_groups: dict[bytes, list[Consensus_stats_timestampedEvent]] = {}

                for line in f:
                    if not line.startswith('{"@type":"consensus.stats.events"'):
                        continue

                    events = Consensus_stats_events.from_json(line)

                    events_by_groups.setdefault(events.id, []).extend(events.events)

                for events in events_by_groups.values():
                    self._process_group_events(events)

        self._infer_slot_phases()
        self._infer_slot_events()

        result = ConsensusData(slots=list(self._slots.values()), events=self._events)

        self._clear_state()

        return result
