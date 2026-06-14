# pyright: reportPrivateUsage=false

import asyncio
from typing import final, override

import pytest
from consensus_explorer.models import (
    ConsensusData,
    GroupData,
    GroupInfo,
    SlotData,
    UnnamedGroupInfo,
)
from consensus_explorer.parser.parser_base import GroupParser
from consensus_explorer.parser.parser_session_stats import ParserSessionStats
from consensus_explorer.visualizer.dash_app import DashApp


@final
class DummyParser(GroupParser):
    def __init__(self, groups: list[GroupData]):
        self._groups = groups

    @override
    def list_groups(self) -> list[GroupData]:
        return self._groups

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        raise AssertionError(f"parse_group should not be called in this test: {valgroup_name}")


@final
class UpdatingParser(GroupParser):
    def __init__(self, group: GroupData, data_sequence: list[ConsensusData]):
        self._group = group
        self._data_sequence = data_sequence
        self._index = 0

    @override
    def list_groups(self) -> list[GroupData]:
        return [self._group]

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        data = self._data_sequence[min(self._index, len(self._data_sequence) - 1)]
        self._index += 1
        return data


@final
class StaticDataParser(GroupParser):
    def __init__(self, group: GroupData, data: ConsensusData):
        self._group = group
        self._data = data

    @override
    def list_groups(self) -> list[GroupData]:
        return [self._group]

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        return self._data


@final
class FakeHeader:
    def to_dict(self) -> dict[str, str]:
        return {"id": "removed"}


@final
class RecordingTonlibClient:
    def __init__(self, fail_transactions: bool = False):
        self.fail_transactions = fail_transactions
        self.loop: asyncio.AbstractEventLoop | None = None
        self.closed = False

    async def init(self) -> None:
        self.loop = asyncio.get_running_loop()

    async def get_block_transactions(self, _block: object) -> list[int]:
        if self.fail_transactions:
            raise RuntimeError("boom")
        return [1, 2]

    async def get_block_header(self, _block: object) -> FakeHeader:
        return FakeHeader()

    async def aclose(self) -> None:
        self.closed = True


@final
class RecordingTonlibFactory:
    def __init__(self, fail_transactions: bool = False):
        self.fail_transactions = fail_transactions
        self.created: list[RecordingTonlibClient] = []

    def __call__(self) -> RecordingTonlibClient:
        client = RecordingTonlibClient(fail_transactions=self.fail_transactions)
        self.created.append(client)
        return client


def _slot_data(block_id_ext: str) -> SlotData:
    return SlotData(
        valgroup_id="group",
        slot=1,
        is_empty=False,
        slot_start_est_ms=0.0,
        block_id_ext=block_id_ext,
        candidate_id=None,
        parent_block=None,
        collator=None,
    )


def test_update_data_sorts_base64_groups_first_and_named_groups_by_cc_seqno():
    base64_a = UnnamedGroupInfo(valgroup_hash=b"a", group_start_est=1.0)
    base64_b = UnnamedGroupInfo(valgroup_hash=b"b", group_start_est=2.0)
    named_cc_20 = GroupInfo(
        valgroup_hash=b"g20",
        catchain_seqno=20,
        workchain=0,
        shard=0x8000000000000000,
        group_start_est=3.0,
    )
    named_cc_3 = GroupInfo(
        valgroup_hash=b"g3",
        catchain_seqno=3,
        workchain=-1,
        shard=0x4000000000000000,
        group_start_est=4.0,
    )
    named_cc_11 = GroupInfo(
        valgroup_hash=b"g11",
        catchain_seqno=11,
        workchain=0,
        shard=0xC000000000000000,
        group_start_est=5.0,
    )

    app = DashApp(DummyParser([named_cc_20, base64_b, named_cc_3, base64_a, named_cc_11]))

    options, value = app.update_data(href=None, time_from=None, time_until=None)

    assert [option["value"] for option in options] == [
        base64_a.valgroup_name,
        base64_b.valgroup_name,
        named_cc_3.valgroup_name,
        named_cc_11.valgroup_name,
        named_cc_20.valgroup_name,
    ]
    assert value == base64_a.valgroup_name


def test_load_group_refreshes_same_selected_group_when_parser_data_changes():
    group = UnnamedGroupInfo(valgroup_hash=b"a", group_start_est=1.0)
    initial_data = ConsensusData(groups=[group], slots=[], events=[])
    updated_data = ConsensusData(groups=[group], slots=[], events=[])
    parser = UpdatingParser(group, [initial_data, updated_data])
    app = DashApp(parser)

    app._load_group(group.valgroup_name)
    first_builder = app._builder

    app._load_group(group.valgroup_name)

    assert app._data is updated_data
    assert app._builder is not first_builder


def test_update_group_stats_includes_group_start_est():
    group = UnnamedGroupInfo(valgroup_hash=b"a", group_start_est=1234.5)
    app = DashApp(StaticDataParser(group, ConsensusData(groups=[group], slots=[], events=[])))

    stats = app._update_group_stats(group.valgroup_name)

    assert "group start estimate = 1234.5" in stats


def test_parser_session_stats_parse_group_reuses_filtered_result_for_same_snapshot(
    monkeypatch: pytest.MonkeyPatch,
):
    group = UnnamedGroupInfo(valgroup_hash=b"a", group_start_est=1.0)
    base_data = ConsensusData(groups=[group], slots=[], events=[])
    parser = ParserSessionStats([], r"^(.*)$")
    monkeypatch.setattr(parser, "parse", lambda: base_data)

    first = parser.parse_group(group.valgroup_name)
    second = parser.parse_group(group.valgroup_name)

    assert first == second


def test_format_slot_meta_uses_fresh_tonlib_client_for_each_lookup(monkeypatch: pytest.MonkeyPatch):
    factory = RecordingTonlibFactory()
    app = DashApp(DummyParser([]))
    monkeypatch.setattr(app, "_tonlib_factory", factory)
    slot_data = _slot_data("(0,8000000000000000,1):" + "00" * 32 + ":" + "11" * 32)

    first = app._format_slot_meta("group", 1, slot_data)
    second = app._format_slot_meta("group", 1, slot_data)

    assert "tonlib error" not in first
    assert "tonlib error" not in second
    assert "transactions count = 2" in first
    assert "header = {}" in first
    assert len(factory.created) == 2
    assert factory.created[0].closed
    assert factory.created[1].closed
    assert factory.created[0].loop is not factory.created[1].loop


def test_format_slot_meta_closes_tonlib_client_when_block_fetch_fails(
    monkeypatch: pytest.MonkeyPatch,
):
    factory = RecordingTonlibFactory(fail_transactions=True)
    app = DashApp(DummyParser([]))
    monkeypatch.setattr(app, "_tonlib_factory", factory)
    slot_data = _slot_data("(0,8000000000000000,1):" + "22" * 32 + ":" + "33" * 32)

    meta = app._format_slot_meta("group", 1, slot_data)

    assert "tonlib error: boom" in meta
    assert len(factory.created) == 1
    assert factory.created[0].closed
