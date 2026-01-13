import dataclasses
import re
from datetime import datetime
from pathlib import Path
from typing import final, override

from .parser_base import Parser
from ..models import ConsensusData, SlotData, EventData

type slot_id_type = tuple[str, int]


@dataclasses.dataclass
class VoteData:
    vote: str
    t_ms: float
    v_id: int
    weight: int


TARGET_TO_LABEL = {
    "CandidateReceived": "candidate_received",
    "CollateStarted": "collate_started",
    "CollateFinished": "collate_finished",
    "ValidateStarted": "validate_started",
    "ValidateFinished": "validate_finished",
    "NotarObserved": "notarize_observed",
    "FinalObserved": "finalize_observed",
}


@final
class ParserLogs(Parser):
    def __init__(self, logs_path: list[Path]):
        self._logs_path = logs_path
        self._slots: dict[slot_id_type, SlotData] = {}
        self._collated: dict[slot_id_type, dict[str, EventData]] = {}
        self._votes: dict[slot_id_type, dict[str, list[VoteData]]] = {}
        self._total_weights: dict[str, int] = {}
        self._slot_leaders: dict[slot_id_type, int] = {}
        self._slot_events: dict[slot_id_type, dict[int, dict[str, EventData]]] = {}
        self._events: list[EventData] = []

    def _parse_stats_target_reached(
        self,
        line: str,
        t_ms: float,
        v_group: str,
        v_id: int,
    ):
        stats_match = re.search(r"target=(\w+),\s*slot=(\d+),\s*timestamp=([\d.]+)", line)
        if not stats_match:
            return

        target = stats_match.group(1)
        slot = int(stats_match.group(2))
        slot_id = (v_group, slot)

        if slot_id not in self._slots:
            self._slots[slot_id] = SlotData(
                valgroup_id=v_group,
                slot=slot,
                is_empty=False,
                slot_start_est_ms=t_ms,
                block_id_ext=None,
                collator=None,
            )

        self._slots[slot_id].slot_start_est_ms = min(t_ms, self._slots[slot_id].slot_start_est_ms)

        if target == "CollateStarted":
            self._slots[slot_id].collator = v_id

        if target == "FinalObserved" and self._slot_leaders.get((v_group, slot + 1)) == v_id:
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

        if target in TARGET_TO_LABEL:
            label = TARGET_TO_LABEL[target]
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

            if label == "candidate_received":
                self._events.append(ev)

            if label in ("collate_started", "collate_finished"):
                self._collated.setdefault(slot_id, {})[label] = ev

    def _parse_skip_vote(self, line: str, t_ms: float, v_group: str, v_id: int):
        slot_match = re.search(r"slot=(\d+)", line)
        assert slot_match is not None

        slot = int(slot_match.group(1))
        slot_id = (v_group, slot)

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

        if slot_id not in self._slots:
            self._slots[slot_id] = SlotData(
                valgroup_id=v_group,
                slot=slot,
                is_empty=True,
                slot_start_est_ms=t_ms,
                block_id_ext=None,
                collator=None,
            )
        else:
            self._slots[slot_id].is_empty = True

    def _parse_publish_event(
        self, line: str, t_ms: float, v_group: str, v_id: int, v_weights: dict[str, int]
    ):
        if "BroadcastVote" in line and "SkipVote" not in line:
            slot_match = re.search(r"id=\{(\d+)", line)
            assert slot_match is not None
            slot = int(slot_match.group(1))
            slot_id = (v_group, slot)

            vote_match = re.search(r"vote=(\w+)", line)
            assert vote_match is not None
            vote = vote_match.group(1)

            self._votes.setdefault(slot_id, {}).setdefault(vote, []).append(
                VoteData(vote=vote, t_ms=t_ms, v_id=v_id, weight=v_weights[v_group])
            )

        elif "OurLeaderWindowStarted" in line:
            start_slot_match = re.search(r"start_slot=(\d+)", line)
            assert start_slot_match is not None
            start_slot = int(start_slot_match.group(1))

            end_slot_match = re.search(r"end_slot=(\d+)", line)
            assert end_slot_match is not None
            end_slot = int(end_slot_match.group(1))

            for s in range(start_slot, end_slot):
                self._slot_leaders[(v_group, s)] = v_id
        elif "BlockFinalized" in line and not "BlockFinalizedInMasterchain" in line:
            slot_match = re.search(r"candidate=Candidate\{id=\{(\d+)", line)
            assert slot_match is not None
            slot = int(slot_match.group(1))
            slot_id = (v_group, slot)
            block_id_match = re.search(r"(\([^)]+\):[A-F0-9]+:[A-F0-9]+)", line)
            assert block_id_match is not None
            block_id = block_id_match.group(0)
            self._slots[slot_id].block_id_ext = block_id

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
                and "NotarizeVote" in self._votes[slot_id]
                and collate_end is not None
            ):
                notarize_reached = self._process_vote_threshold(
                    slot_data=slot_data,
                    votes=self._votes[slot_id]["NotarizeVote"],
                    weight_threshold=weight_threshold,
                    label="notarize",
                    phase_start=collate_end,
                )

            if (
                slot_id in self._votes
                and "FinalizeVote" in self._votes[slot_id]
                and notarize_reached is not None
            ):
                _ = self._process_vote_threshold(
                    slot_data=slot_data,
                    votes=self._votes[slot_id]["FinalizeVote"],
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
        phase_start: float,
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
    def _extract_timestamp(line: str) -> float | None:
        i = 0
        for _ in range(4):
            i = line.find('[', i) + 1
        j = line.find(']', i)
        timestamp_str = line[i:j]
        dt = datetime.fromisoformat(timestamp_str)
        return dt.timestamp() * 1000

    @staticmethod
    def _extract_valgroup(line: str) -> str | None:
        if "valgroup" not in line:
            return None
        valgroup_match = re.search(r"valgroup\(([^)]+)\)\.(\d+)", line)
        if not valgroup_match:
            return None
        v_group = f"{valgroup_match.group(1)}.{valgroup_match.group(2)}"
        return v_group

    def _parse_validator_info(
        self,
        line: str,
        v_group: str,
        v_groups: dict[str, int],
        v_weights: dict[str, int],
    ):
        if "We are validator" not in line:
            return

        validator_match = re.search(r"We are validator (\d+)", line)
        assert validator_match is not None
        v_groups[v_group] = int(validator_match.group(1))

        weight_match = re.search(r"with weight (\d+)", line)
        assert weight_match is not None
        v_weights[v_group] = int(weight_match.group(1))

        total_weight_match = re.search(r"out of (\d+)", line)
        assert total_weight_match is not None
        self._total_weights[v_group] = int(total_weight_match.group(1))

    def _process_log_line(
        self,
        line: str,
        v_group: str,
        t_ms: float,
        v_groups: dict[str, int],
        v_weights: dict[str, int],
    ):
        v_id = v_groups.get(v_group)
        if v_id is None:
            return

        if "StatsTargetReached" in line:
            self._parse_stats_target_reached(line, t_ms, v_group, v_id)
        elif "Obtained certificate for SkipVote" in line:
            self._parse_skip_vote(line, t_ms, v_group, v_id)
        elif "Published event" in line:
            self._parse_publish_event(line, t_ms, v_group, v_id, v_weights)

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
            with open(log_file, "r", encoding="utf-8") as f:

                v_groups: dict[str, int] = {}
                v_weights: dict[str, int] = {}

                for line in f:
                    if ("Published event" not in line) and ("StatsTargetReached" not in line) and ("Obtained certificate for SkipVote" not in line) and ("We are validator" not in line):
                        continue
                    v_group = self._extract_valgroup(line)
                    if v_group is None:
                        continue

                    t_ms = self._extract_timestamp(line)
                    if t_ms is None:
                        continue

                    self._parse_validator_info(line, v_group, v_groups, v_weights)

                    self._process_log_line(line, v_group, t_ms, v_groups, v_weights)

        self._infer_slot_phases()
        self._infer_slot_events()

        result = ConsensusData(slots=list(self._slots.values()), events=self._events)

        self._clear_state()

        return result
