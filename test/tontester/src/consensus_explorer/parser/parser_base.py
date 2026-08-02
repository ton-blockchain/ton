from abc import ABC, abstractmethod
from collections.abc import Sequence

from ..models import ConsensusData, EventData, GroupData, SlotData


class GroupParser(ABC):
    @abstractmethod
    def list_groups(self) -> list[GroupData]:
        pass

    @abstractmethod
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        pass

    def parse_groups(self, valgroup_names: Sequence[str]) -> dict[str, ConsensusData]:
        """Override when groups share log files, to read each file once per batch."""
        return {name: self.parse_group(name) for name in valgroup_names}

    def group_revision(self, valgroup_name: str) -> int | None:
        """Token to key cached derived results on. None means changes can't be
        detected, so nothing derived may be cached."""
        _ = valgroup_name
        return None


def split_by_group(data: ConsensusData, valgroup_names: Sequence[str]) -> dict[str, ConsensusData]:
    """Split a multi-group parse result into one ``ConsensusData`` per group."""
    groups: dict[str, list[GroupData]] = {name: [] for name in valgroup_names}
    slots: dict[str, list[SlotData]] = {name: [] for name in valgroup_names}
    events: dict[str, list[EventData]] = {name: [] for name in valgroup_names}

    for g in data.groups:
        bucket_groups = groups.get(g.valgroup_name)
        if bucket_groups is not None:
            bucket_groups.append(g)
    for s in data.slots:
        bucket_slots = slots.get(s.valgroup_id)
        if bucket_slots is not None:
            bucket_slots.append(s)
    for e in data.events:
        bucket_events = events.get(e.valgroup_id)
        if bucket_events is not None:
            bucket_events.append(e)

    return {
        name: ConsensusData(groups=groups[name], slots=slots[name], events=events[name])
        for name in groups
    }
