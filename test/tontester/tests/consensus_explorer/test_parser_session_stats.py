# pyright: reportPrivateUsage=false

import shutil
from pathlib import Path

from consensus_explorer.cached_parser import CachedGroupParser
from consensus_explorer.file_index import FileIndex
from consensus_explorer.parser.parser_session_stats import ParserSessionStats
from consensus_explorer.visualizer.figure_builder import FigureBuilder
from tonapi.ton_api import (
    Consensus_candidateId,
    Consensus_stats_collateFinished,
    Consensus_stats_collateStarted,
)

# Vendored session-stats logs from a 2-node local network (see test_basic.py).
# Shard valgroup "0,8000000000000000.0" carries masterchain crosslink markers.
_DATA_DIR = Path(__file__).resolve().parent / "data"
_SHARD_VALGROUP = "0,8000000000000000.0"


def _stats_logs() -> list[Path]:
    return sorted(_DATA_DIR.glob("session-logs*"))


def test_parser_adds_shard_markers_from_masterchain_shard_configuration():
    parser = ParserSessionStats(_stats_logs(), r"^(.*)$", with_cache=False)

    data = parser.parse()

    mc_finalize_markers = [e for e in data.events if e.label == "mc_ref_finalized"]
    mc_collate_markers = [e for e in data.events if e.label == "mc_ref_collate_started"]
    assert mc_finalize_markers
    assert mc_collate_markers

    source_finalize_events = {
        (e.valgroup_id, e.slot, e.t_ms) for e in data.events if e.label == "finalize_reached"
    }
    source_collate_events = {
        (e.valgroup_id, e.slot, e.t_ms) for e in data.events if e.label == "collate"
    }

    for marker in mc_finalize_markers:
        assert marker.valgroup_id.startswith("0,")
        assert marker.source_valgroup_id is not None
        assert marker.source_valgroup_id.startswith("-1,")
        assert marker.source_slot is not None
        assert marker.source_block_id is not None
        assert (
            marker.source_valgroup_id,
            marker.source_slot,
            marker.t_ms,
        ) in source_finalize_events

    for marker in mc_collate_markers:
        assert marker.valgroup_id.startswith("0,")
        assert marker.source_valgroup_id is not None
        assert marker.source_valgroup_id.startswith("-1,")
        assert marker.source_slot is not None
        assert marker.source_block_id is not None
        assert (
            marker.source_valgroup_id,
            marker.source_slot,
            marker.t_ms,
        ) in source_collate_events

    assert {(e.valgroup_id, e.slot) for e in mc_finalize_markers} & {
        (e.valgroup_id, e.slot) for e in mc_collate_markers
    }


def test_figure_builder_includes_crosslink_markers():
    parser = ParserSessionStats(_stats_logs(), r"^(.*)$", with_cache=False)
    data = parser.parse()
    builder = FigureBuilder(data)

    summary = builder.build_summary(_SHARD_VALGROUP, 0, 50, False)

    # plotly ships no type stubs, so Figure.data traces are untyped (see figure_builder.py)
    summary_names = {  # pyright: ignore[reportUnknownVariableType]
        trace.name  # pyright: ignore[reportUnknownMemberType, reportAttributeAccessIssue]
        for trace in summary.data  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]
    }

    assert "mc_ref_collate_started" in summary_names
    assert "mc_ref_finalized" in summary_names


def test_cached_parser_keeps_crosslink_markers_in_stats_dir_mode(tmp_path: Path):
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    for src in _DATA_DIR.glob("session-logs*"):
        _ = shutil.copy(src, stats_dir / src.name)

    file_index = FileIndex(stats_dir, stats_dir / "index.db")
    conn = file_index._connect()
    try:
        file_index._initial_scan(conn)
    finally:
        conn.close()

    parser = CachedGroupParser(file_index, r"^(.*)$")
    data = parser.parse_group(_SHARD_VALGROUP)

    labels = {e.label for e in data.events}
    assert "mc_ref_collate_started" in labels
    assert "mc_ref_finalized" in labels


def test_collation_attached_to_finished_slot_when_slots_differ():
    """When collation starts at slot x but finishes at slot y,
    both collate_started and collate_finished should be attached to slot y."""
    parser = ParserSessionStats([], r"^(.*)$", with_cache=False)
    v_group = "test_group"
    v_id = 0
    target_slot = 10  # slot x: where collation was initiated
    finished_slot = 11  # slot y: where collation actually finished

    # Simulate collateStarted for target_slot
    parser._parse_stats_event(
        Consensus_stats_collateStarted(target_slot=target_slot),
        t_ms=1000.0,
        v_group=v_group,
        v_id=v_id,
    )

    # collate_started should initially be under target_slot
    assert (v_group, target_slot) in parser._slot_events
    assert "collate_started" in parser._slot_events[(v_group, target_slot)][v_id]
    assert (v_group, target_slot) in parser._collated
    assert "collate_started" in parser._collated[(v_group, target_slot)]

    # Simulate collateFinished at a different slot
    parser._parse_stats_event(
        Consensus_stats_collateFinished(
            target_slot=target_slot,
            id=Consensus_candidateId(slot=finished_slot, hash=b"\x00" * 32),
        ),
        t_ms=2000.0,
        v_group=v_group,
        v_id=v_id,
    )

    finished_slot_id = (v_group, finished_slot)

    # Both events should now be under finished_slot
    assert "collate_started" in parser._slot_events[finished_slot_id][v_id]
    assert "collate_finished" in parser._slot_events[finished_slot_id][v_id]
    assert parser._slot_events[finished_slot_id][v_id]["collate_started"].slot == finished_slot
    assert parser._slot_events[finished_slot_id][v_id]["collate_finished"].slot == finished_slot

    assert "collate_started" in parser._collated[finished_slot_id]
    assert "collate_finished" in parser._collated[finished_slot_id]
    assert parser._collated[finished_slot_id]["collate_started"].slot == finished_slot

    # collate_started should be removed from target_slot
    target_slot_id = (v_group, target_slot)
    assert "collate_started" not in parser._slot_events.get(target_slot_id, {}).get(v_id, {})
    assert "collate_started" not in parser._collated.get(target_slot_id, {})

    # Collator should be set on the finished slot
    assert parser._slots[finished_slot_id].collator == v_id


def test_collation_same_slot_unchanged():
    """When collation starts and finishes at the same slot, no relocation needed."""
    parser = ParserSessionStats([], r"^(.*)$", with_cache=False)
    v_group = "test_group"
    v_id = 0
    slot = 10

    parser._parse_stats_event(
        Consensus_stats_collateStarted(target_slot=slot),
        t_ms=1000.0,
        v_group=v_group,
        v_id=v_id,
    )
    parser._parse_stats_event(
        Consensus_stats_collateFinished(
            target_slot=slot,
            id=Consensus_candidateId(slot=slot, hash=b"\x00" * 32),
        ),
        t_ms=2000.0,
        v_group=v_group,
        v_id=v_id,
    )

    slot_id = (v_group, slot)
    assert "collate_started" in parser._slot_events[slot_id][v_id]
    assert "collate_finished" in parser._slot_events[slot_id][v_id]
    assert parser._slot_events[slot_id][v_id]["collate_started"].slot == slot
    assert parser._slot_events[slot_id][v_id]["collate_finished"].slot == slot
