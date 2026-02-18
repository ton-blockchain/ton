import dataclasses
import gzip
import io
import logging
import os
import re
from itertools import islice
from pathlib import Path
from typing import Callable, final, override

from tonapi.ton_api import (
    Consensus_simplex_finalizeVote,
    Consensus_simplex_notarizeVote,
    Consensus_simplex_skipVote,
    Consensus_simplex_stats_certObserved,
    Consensus_simplex_stats_voted,
    Consensus_stats_candidateReceived,
    Consensus_stats_collatedEmpty,
    Consensus_stats_collateFinished,
    Consensus_stats_collateStarted,
    Consensus_stats_empty,
    Consensus_stats_events,
    Consensus_stats_id,
    Consensus_stats_timestampedEvent,
    Consensus_stats_validationFinished,
    Consensus_stats_validationStarted,
    TypeConsensus_stats_Event,
)

from ..models import ConsensusData, EventData, SlotData
from .parser_base import GroupParser

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


def open_stats_file(path: Path) -> io.TextIOWrapper:
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", errors="ignore")
    return open(path, "r", encoding="utf-8", errors="ignore")


@final
class ParserSessionStats(GroupParser):
    def __init__(
        self,
        logs_path: list[Path],
        hostname_regex: str,
        with_cache: bool = True,
        target_group_hash: bytes | None = None,
    ):
        self._logs_path = logs_path
        self._target_group_hash = target_group_hash
        self._hostname_regex = re.compile(hostname_regex)
        self._slots: dict[slot_id_type, SlotData] = {}
        self._collated: dict[slot_id_type, dict[str, EventData]] = {}
        self._votes: dict[slot_id_type, dict[str, list[VoteData]]] = {}
        self._total_weights: dict[str, int] = {}
        self._total_validators: dict[str, int] = {}
        self._seen_validators: dict[str, set[int]] = {}
        self._slot_events: dict[slot_id_type, dict[int, dict[str, EventData]]] = {}
        self._events: list[EventData] = []

        self.with_cache = with_cache
        self._raw_events: dict[Path, dict[bytes, list[Consensus_stats_timestampedEvent]]] = {}
        self._cache_result: ConsensusData | None = None
        self._cache_lines: dict[Path, int] = {}
        self._cache_mtime: dict[Path, float] = {}

    def _get_create_slot(self, slot: int, v_group: str) -> SlotData:
        slot_id = (v_group, slot)

        if slot_id not in self._slots:
            self._slots[slot_id] = SlotData(
                valgroup_id=v_group,
                slot=slot,
                is_empty=False,
                slot_start_est_ms=float("inf"),
                block_id_ext=None,
                candidate_id=None,
                parent_block=None,
                collator=None,
            )

        return self._slots[slot_id]

    def _parse_stats_event(
        self,
        event: TypeConsensus_stats_Event,
        t_ms: float,
        v_group: str,
        v_id: int,
        get_slot_leader: Callable[[int], int],
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
        slot_data.collator = get_slot_leader(slot)

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

        if isinstance(event, Consensus_stats_candidateReceived):
            if event.block is not None and not isinstance(event.block, Consensus_stats_empty):
                assert event.block.id is not None
                slot_data.block_id_ext = f"({event.block.id.workchain},{self._shard_to_hex(event.block.id.shard)},{event.block.id.seqno}):{event.block.id.root_hash.hex().upper()}:{event.block.id.file_hash.hex().upper()}"
            slot_data.candidate_id = str(event.id)
            slot_data.parent_block = str(event.parent)

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
            ev = EventData(
                valgroup_id=v_group,
                slot=slot,
                label="skip_observed",
                kind="local",
                validator=v_id,
                t_ms=t_ms,
            )
            self._events.append(ev)
            self._slot_events.setdefault((v_group, slot), {}).setdefault(v_id, {})[
                "skip_observed"
            ] = ev

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
            self._slot_events.setdefault((v_group, slot), {}).setdefault(v_id, {})[label] = (
                EventData(
                    valgroup_id=v_group,
                    slot=slot,
                    label=label,
                    kind="local",
                    validator=v_id,
                    t_ms=t_ms,
                )
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

        if isinstance(vote, Consensus_simplex_notarizeVote) and get_slot_leader(slot + 1) == v_id:
            self._events.append(
                EventData(
                    valgroup_id=v_group,
                    slot=slot,
                    label="notarize_observed_by_next_leader",
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
        slot_data = self._get_create_slot(slot, v_group)
        slot_data.slot_start_est_ms = min(t_ms, slot_data.slot_start_est_ms)
        vote_type = {
            Consensus_simplex_notarizeVote: "notarize_vote",
            Consensus_simplex_finalizeVote: "finalize_vote",
            Consensus_simplex_skipVote: "skip_vote",
        }[type(vote)]
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
            val_total = self._total_validators.get(slot_id[0])
            val_has = len(self._seen_validators.get(slot_id[0], set()))
            events_min_observed: dict[str, float | None] = {
                "skip_observed": None,
                "notarize_observed": None,
                "finalize_observed": None,
            }
            if (
                val_total is not None and val_has < val_total and self._slot_events.get(slot_id)
            ):  # missing logs for some validators in the group; infer reached events by min observed
                for label in events_min_observed:
                    min_observed = float("inf")
                    for events in self._slot_events[slot_id].values():
                        e = events.get(label)
                        if e:
                            min_observed = min(min_observed, e.t_ms)
                    if min_observed != float("inf"):
                        events_min_observed[label] = min_observed
                for label in events_min_observed:
                    value = events_min_observed[label]
                    if value is None:
                        continue

            events_reached_time: dict[str, float | None] = {
                "skip": None,
                "notarize": None,
                "finalize": None,
            }

            total_weight = self._total_weights[slot_id[0]]
            weight_threshold = (total_weight * 2) // 3 + 1

            for label in events_reached_time:
                if slot_id in self._votes and f"{label}_vote" in self._votes[slot_id]:
                    events_reached_time[label] = self._process_vote_threshold(
                        votes=self._votes[slot_id][f"{label}_vote"],
                        weight_threshold=weight_threshold,
                    )

            notarize_reached = None
            for label in events_reached_time:
                # if we have logs not for all validators, we assume reach events as a better estimate
                t = events_reached_time.get(label) or float("inf")
                t = min(t, events_min_observed.get(f"{label}_observed") or float("inf"))
                if t == float("inf"):
                    continue
                if label == "notarize":
                    notarize_reached = t
                phase_start = None
                if label == "notarize":
                    phase_start = collate_end
                if label == "finalize":
                    phase_start = notarize_reached

                self._events.append(
                    EventData(
                        valgroup_id=slot_data.valgroup_id,
                        slot=slot_data.slot,
                        label=f"{label}_reached",
                        kind="reached",
                        t_ms=t,
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
                            t1_ms=t,
                        )
                    )

    @staticmethod
    def _process_vote_threshold(
        votes: list[VoteData],
        weight_threshold: int,
    ) -> float | None:
        current_weight = 0
        sorted_votes = sorted(votes, key=lambda x: x.t_ms)
        for vote in sorted_votes:
            current_weight += vote.weight
            if current_weight >= weight_threshold:
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
        if event_id is None:
            return

        v_group = f"{event_id.workchain},{self._shard_to_hex(event_id.shard)}.{event_id.cc_seqno}"

        v_id = event_id.idx
        v_weight = event_id.weight
        self._total_weights[v_group] = event_id.total_weight
        self._total_validators[v_group] = event_id.total_validators
        self._seen_validators.setdefault(v_group, set()).add(v_id)

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
                self._parse_stats_event(ev, t_ms, v_group, v_id, get_slot_leader)

    def _extract_hostname(self, path: Path) -> str:
        m = self._hostname_regex.search(str(path))
        if m is None:
            return path.name
        return m.group(1)

    def _clear_state(self) -> None:
        self._slots = {}
        self._collated = {}
        self._votes = {}
        self._total_weights = {}
        self._total_validators = {}
        self._seen_validators = {}
        self._slot_events = {}
        self._events = []

    def parse(self) -> ConsensusData:
        files_changed = False

        if self.with_cache:
            for log_file in self._logs_path:
                current_mtime = os.stat(log_file).st_mtime
                cached_mtime = self._cache_mtime.get(log_file)
                if cached_mtime is not None and current_mtime == cached_mtime:
                    continue
                self._cache_mtime[log_file] = current_mtime
                files_changed = True
        if not files_changed and self._cache_result:
            return self._cache_result

        merged: dict[str, dict[bytes, list[Consensus_stats_timestampedEvent]]] = {}

        for log_file in self._logs_path:
            hostname = self._extract_hostname(log_file)
            try:
                with open_stats_file(log_file) as f:
                    events_by_groups: dict[bytes, list[Consensus_stats_timestampedEvent]] = {}
                    start_line = 0
                    if self.with_cache and log_file in self._cache_lines:
                        start_line = self._cache_lines[log_file]
                    current_line = start_line
                    for line in islice(f, start_line, None):
                        current_line += 1
                        if not line.startswith('{"@type":"consensus.stats.events"'):
                            continue

                        events = Consensus_stats_events.from_json(line)

                        if (
                            self._target_group_hash is not None
                            and events.id != self._target_group_hash
                        ):
                            continue

                        events_by_groups.setdefault(events.id, []).extend(events.events)

                    if self.with_cache:
                        self._cache_lines[log_file] = current_line
                        # merge old events with new ones
                        old_events = self._raw_events.get(log_file, {})
                        events_by_groups = {
                            k: old_events.get(k, []) + events_by_groups.get(k, [])
                            for k in set(old_events) | set(events_by_groups)
                        }
                        self._raw_events[log_file] = events_by_groups

                    host_groups = merged.setdefault(hostname, {})
                    for group_hash, group_events in events_by_groups.items():
                        host_groups.setdefault(group_hash, []).extend(group_events)
            except OSError | FileNotFoundError | gzip.BadGzipFile:
                logging.warning(f"Failed to read log file {log_file}", stack_info=True)

        for host_groups in merged.values():
            for events in host_groups.values():
                self._process_group_events(events)

        self._infer_slot_phases()
        self._infer_slot_events()

        result = ConsensusData(slots=list(self._slots.values()), events=self._events)

        if self.with_cache:
            self._cache_result = result

        self._clear_state()

        return result

    @override
    def list_groups(self) -> list[str]:
        data = self.parse()
        return sorted(set(s.valgroup_id for s in data.slots))

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        data = self.parse()
        if self._target_group_hash is not None:
            return data
        return ConsensusData(
            slots=[s for s in data.slots if s.valgroup_id == valgroup_name],
            events=[e for e in data.events if e.valgroup_id == valgroup_name],
        )
