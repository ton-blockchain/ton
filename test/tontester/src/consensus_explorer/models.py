from dataclasses import dataclass


@dataclass(frozen=True)
class GroupInfo:
    valgroup_hash: bytes
    catchain_seqno: int
    workchain: int
    shard: int

    @property
    def valgroup_name(self) -> str:
        shard_hex = f"{self.shard & 0xFFFFFFFFFFFFFFFF:016x}"
        return f"{self.workchain},{shard_hex}.{self.catchain_seqno}"


type GroupId = bytes | GroupInfo


@dataclass
class SlotData:
    valgroup_id: str
    slot: int
    is_empty: bool
    slot_start_est_ms: float
    block_id_ext: str | None
    candidate_id: str | None
    parent_block: str | None
    collator: int | str | None

    def block_id(self) -> str | None:
        return self.block_id_ext.split(":")[0] if self.block_id_ext else None


@dataclass
class EventData:
    valgroup_id: str
    slot: int
    label: str
    kind: str
    t_ms: float
    validator: int | str | None = None
    t1_ms: float | None = None

    def get_color(self) -> str | None:
        from .visualizer.style import COLOR_MAP

        return COLOR_MAP.get(self.label)

    def get_symbol(self) -> str:
        from .visualizer.style import SYMBOL_MAP

        return SYMBOL_MAP.get(self.kind, "circle")


@dataclass
class ConsensusData:
    slots: list[SlotData]
    events: list[EventData]
