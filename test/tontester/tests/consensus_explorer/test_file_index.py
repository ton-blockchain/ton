# pyright: reportPrivateUsage=false

import json
from pathlib import Path

import pytest
from consensus_explorer.file_index import FileIndex, FileScanResult
from consensus_explorer.models import GroupData, UnnamedGroupInfo
from tonapi.ton_api import (
    Consensus_stats_events,
    Consensus_stats_id,
    Consensus_stats_timestampedEvent,
)


def _group(group_hash: bytes) -> UnnamedGroupInfo:
    return UnnamedGroupInfo(valgroup_hash=group_hash, group_start_est=1.0)


def test_remove_file_deletes_groups_without_remaining_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    db_path = tmp_path / "index.db"

    file_a = stats_dir / "a.log"
    file_b = stats_dir / "b.log"
    _ = file_a.write_text("first")
    _ = file_b.write_text("second")

    groups_by_file: dict[Path, set[GroupData]] = {
        file_a.resolve(): {_group(b"g-only-a"), _group(b"g-shared")},
        file_b.resolve(): {_group(b"g-shared")},
    }

    def fake_scan(path: Path) -> FileScanResult:
        return FileScanResult(groups=groups_by_file[path.resolve()])

    index = FileIndex(stats_dir, db_path)
    monkeypatch.setattr(index, "_scan_file", fake_scan)

    with index._connect() as conn:
        _ = index._index_file(file_a, conn, 0, 2)
        _ = index._index_file(file_b, conn, 1, 2)

    with index._connect() as conn:
        changed_hashes = index._remove_file(file_a, conn)

    assert changed_hashes == {b"g-only-a", b"g-shared"}
    assert {group.valgroup_hash for group in index.get_all_groups()} == {b"g-shared"}
    assert index.get_files_for_group(b"g-only-a") == []
    assert index.get_files_for_group(b"g-shared") == [file_b.resolve()]


def test_index_file_removes_deleted_file_if_it_disappears_before_reindex(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    db_path = tmp_path / "index.db"

    file_a = stats_dir / "a.log"
    _ = file_a.write_text("first")

    def fake_scan(_path: Path) -> FileScanResult:
        return FileScanResult(groups={_group(b"g-only-a")})

    index = FileIndex(stats_dir, db_path)
    monkeypatch.setattr(index, "_scan_file", fake_scan)

    with index._connect() as conn:
        _ = index._index_file(file_a, conn, 0, 1)

    file_a.unlink()

    with index._connect() as conn:
        changed_hashes = index._index_file(file_a, conn, 0, 1)

    assert changed_hashes == {b"g-only-a"}
    assert index.get_all_groups() == []
    assert index.get_files_for_group(b"g-only-a") == []


def test_index_file_skips_directory_and_removes_stale_entry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    db_path = tmp_path / "index.db"

    file_a = stats_dir / "a.log"
    _ = file_a.write_text("first")

    def fake_scan(_path: Path) -> FileScanResult:
        return FileScanResult(groups={_group(b"g-only-a")})

    index = FileIndex(stats_dir, db_path)
    monkeypatch.setattr(index, "_scan_file", fake_scan)

    with index._connect() as conn:
        _ = index._index_file(file_a, conn, 0, 1)

    file_a.unlink()
    file_a.mkdir()

    with index._connect() as conn:
        changed_hashes = index._index_file(file_a, conn, 0, 1)

    assert changed_hashes == {b"g-only-a"}
    assert index.get_all_groups() == []
    assert index.get_files_for_group(b"g-only-a") == []


def test_initial_scan_skips_directories(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    db_path = tmp_path / "index.db"

    file_a = stats_dir / "a.log"
    _ = file_a.write_text("first")
    ignored_dir = stats_dir / "nested"
    ignored_dir.mkdir()

    groups_by_file: dict[Path, set[GroupData]] = {file_a.resolve(): {_group(b"g-only-a")}}

    def fake_scan(path: Path) -> FileScanResult:
        return FileScanResult(groups=groups_by_file[path.resolve()])

    index = FileIndex(stats_dir, db_path)
    monkeypatch.setattr(index, "_scan_file", fake_scan)

    with index._connect() as conn:
        index._initial_scan(conn)

    assert {group.valgroup_hash for group in index.get_all_groups()} == {b"g-only-a"}
    assert index.get_files_for_group(b"g-only-a") == [file_a.resolve()]


def test_scan_file_skips_model_errors(tmp_path: Path):
    db_path = tmp_path / "index.db"
    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()
    log_file = tmp_path / "broken.log"
    valid_group_hash = b"g" * 32
    valid_line = json.dumps(
        Consensus_stats_events(
            id=valid_group_hash,
            events=[
                Consensus_stats_timestampedEvent(
                    ts=1.0,
                    event=Consensus_stats_id(
                        workchain=0,
                        shard=0,
                        cc_seqno=1,
                        idx=0,
                        total_validators=1,
                        weight=1,
                        total_weight=1,
                        slots_per_leader_window=1,
                    ),
                )
            ],
        ).to_dict(),
        separators=(",", ":"),
    )
    _ = log_file.write_text(
        f'{{"@type":"consensus.stats.events","id":1,"events":[]}}\n{valid_line}\n'
    )

    index = FileIndex(stats_dir, db_path)
    scan = index._scan_file(log_file)

    assert {group.valgroup_hash for group in scan.groups} == {valid_group_hash}
