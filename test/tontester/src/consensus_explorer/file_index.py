import json
import logging
import sqlite3
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol, TypedDict, cast, final

from tonapi.ton_api import (
    Consensus_stats_events,
    Consensus_stats_id,
    ValidatorStats_collatedBlock,
    ValidatorStats_validatedBlock,
)
from watchfiles._rust_notify import RustNotify

from tl import ModelError

from .models import GroupData, GroupInfo, UnnamedGroupInfo
from .parser.parser_session_stats import open_stats_file

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class McShardRef:
    mc_seqno: int
    shard_workchain: int
    shard: int
    top_seqno: int
    collate_start_ms: float


@dataclass
class FileScanResult:
    groups: set[GroupData] = field(default_factory=set)
    # (workchain, shard) -> (seqno_min, seqno_max)
    block_ranges: dict[tuple[int, int], tuple[int, int]] = field(default_factory=dict)
    mc_shard_refs: list[McShardRef] = field(default_factory=list)


class FileIndexCallback(Protocol):
    def on_files_changed(self, changed_groups: set[bytes]) -> None: ...


@final
class FileIndex:
    def __init__(self, stats_dir: Path, db_path: Path, sudo_helper: str | None = None):
        self._stats_dir = stats_dir
        self._db_path = db_path
        self._sudo_helper = sudo_helper
        self._callback: FileIndexCallback | None = None
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()

        with self._connect() as conn:
            self._create_tables(conn)

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(str(self._db_path))
        conn.row_factory = sqlite3.Row
        _ = conn.execute("PRAGMA journal_mode=WAL")
        return conn

    @staticmethod
    def _create_tables(conn: sqlite3.Connection) -> None:
        _ = conn.executescript("""
            CREATE TABLE IF NOT EXISTS groups (
                group_id INTEGER PRIMARY KEY AUTOINCREMENT,
                valgroup_hash BLOB NOT NULL UNIQUE,
                catchain_seqno INTEGER,
                workchain INTEGER,
                shard INTEGER,
                group_start_est REAL
            );
            CREATE TABLE IF NOT EXISTS files (
                file_id INTEGER PRIMARY KEY AUTOINCREMENT,
                file_name TEXT UNIQUE NOT NULL,
                mtime REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS group_files (
                group_id INTEGER NOT NULL REFERENCES groups(group_id),
                file_id INTEGER NOT NULL REFERENCES files(file_id),
                PRIMARY KEY (group_id, file_id)
            );
            CREATE TABLE IF NOT EXISTS block_ranges (
                file_id INTEGER NOT NULL REFERENCES files(file_id),
                workchain INTEGER NOT NULL,
                shard INTEGER NOT NULL,
                seqno_start INTEGER NOT NULL,
                seqno_end INTEGER NOT NULL,
                PRIMARY KEY (file_id, workchain, shard)
            );
            CREATE TABLE IF NOT EXISTS mc_shard_refs (
                file_id INTEGER NOT NULL REFERENCES files(file_id),
                mc_seqno INTEGER NOT NULL,
                shard_workchain INTEGER NOT NULL,
                shard INTEGER NOT NULL,
                top_seqno INTEGER NOT NULL,
                collate_start_ms REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_mc_shard_refs_shard
                ON mc_shard_refs(shard_workchain, shard);
            CREATE INDEX IF NOT EXISTS idx_block_ranges_shard
                ON block_ranges(workchain, shard);
        """)

    def install_callback(self, callback: FileIndexCallback) -> None:
        self._callback = callback

    def __enter__(self) -> FileIndex:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join()
            self._thread = None

    def _scan_file(self, path: Path) -> FileScanResult:
        result = FileScanResult()
        try:
            f = open_stats_file(path)
        except PermissionError:
            if self._sudo_helper is None:
                raise
            logger.info("Permission denied for %s, retrying with sudo helper", path)
            f = open_stats_file(path, sudo_helper=self._sudo_helper)
        with f:
            for line in f:
                if line.startswith('{"@type":"consensus.stats.events"'):
                    try:
                        parsed = Consensus_stats_events.from_json(line)
                    except (json.decoder.JSONDecodeError, ModelError) as exc:
                        logger.debug("Failed to parse consensus stats line in %s: %s", path, exc)
                        continue
                    valgroup_hash = parsed.id
                    found_id = False
                    min_ts = float("inf")
                    for te in parsed.events:
                        min_ts = min(min_ts, te.ts)
                        if isinstance(te.event, Consensus_stats_id):
                            result.groups.add(
                                GroupInfo(
                                    valgroup_hash=valgroup_hash,
                                    catchain_seqno=te.event.cc_seqno,
                                    workchain=te.event.workchain,
                                    shard=te.event.shard,
                                    group_start_est=min_ts,
                                )
                            )
                            found_id = True
                            break
                    if not found_id:
                        result.groups.add(
                            UnnamedGroupInfo(
                                valgroup_hash=valgroup_hash,
                                group_start_est=min_ts,
                            )
                        )
                elif line.startswith('{"@type":"validatorStats.collatedBlock"'):
                    try:
                        collated = ValidatorStats_collatedBlock.from_json(line)
                    except (json.decoder.JSONDecodeError, ModelError) as exc:
                        logger.debug("Failed to parse collatedBlock in %s: %s", path, exc)
                        continue
                    if collated.block_id is None:
                        continue
                    bid = collated.block_id
                    wc = bid.workchain
                    shard = bid.shard
                    seqno = bid.seqno
                    self._update_block_range(result, wc, shard, seqno)
                    # Extract MC shard configuration for crosslinks
                    if (
                        wc == -1
                        and collated.block_stats is not None
                        and collated.block_stats.shard_configuration
                    ):
                        collate_start_ms = (collated.collated_at - collated.total_time) * 1000
                        for sb in collated.block_stats.shard_configuration:
                            result.mc_shard_refs.append(
                                McShardRef(
                                    mc_seqno=seqno,
                                    shard_workchain=sb.workchain,
                                    shard=sb.shard,
                                    top_seqno=sb.seqno,
                                    collate_start_ms=collate_start_ms,
                                )
                            )
                elif line.startswith('{"@type":"validatorStats.validatedBlock"'):
                    try:
                        validated = ValidatorStats_validatedBlock.from_json(line)
                    except (json.decoder.JSONDecodeError, ModelError) as exc:
                        logger.debug("Failed to parse validatedBlock in %s: %s", path, exc)
                        continue
                    if validated.block_id is None:
                        continue
                    bid = validated.block_id
                    self._update_block_range(
                        result,
                        bid.workchain,
                        bid.shard,
                        bid.seqno,
                    )
        return result

    @staticmethod
    def _update_block_range(result: FileScanResult, workchain: int, shard: int, seqno: int) -> None:
        key = (workchain, shard)
        if key in result.block_ranges:
            old_start, old_end = result.block_ranges[key]
            result.block_ranges[key] = (min(old_start, seqno), max(old_end, seqno))
        else:
            result.block_ranges[key] = (seqno, seqno)

    def _index_file(
        self, path: Path, conn: sqlite3.Connection, file_idx: int, file_count: int
    ) -> set[bytes]:
        path = path.resolve()
        file_name = str(path)

        if path.exists() and not path.is_file():
            logger.info("Skipping non-file path in stats dir %s", path)
            return self._remove_file(path, conn)

        try:
            mtime = path.stat().st_mtime
        except FileNotFoundError as exc:
            logger.info("File disappeared before indexing %s: %s", path, exc)
            return self._remove_file(path, conn)

        class FileRow(TypedDict):
            file_id: int
            mtime: float

        cursor = conn.cursor()
        _ = cursor.execute(
            "SELECT file_id, mtime FROM files WHERE file_name = ?",
            (file_name,),
        )
        existing = cast(list[FileRow], cursor.fetchall())
        file_id = existing[0]["file_id"] if existing else None

        if existing and existing[0]["mtime"] == mtime:
            return set()

        print(f"Indexing file {path} ({file_idx} out of {file_count})", flush=True)
        try:
            scan = self._scan_file(path)
        except Exception as exc:
            logger.warning("Failed to index %s, will retry on restart: %s", path, exc)
            return set()

        if existing:
            _ = cursor.execute(
                "UPDATE files SET mtime = ? WHERE file_id = ?",
                (mtime, file_id),
            )
            _ = cursor.execute(
                "DELETE FROM group_files WHERE file_id = ?",
                (file_id,),
            )
            _ = cursor.execute(
                "DELETE FROM block_ranges WHERE file_id = ?",
                (file_id,),
            )
            _ = cursor.execute(
                "DELETE FROM mc_shard_refs WHERE file_id = ?",
                (file_id,),
            )
        else:
            _ = cursor.execute(
                "INSERT INTO files (file_name, mtime) VALUES (?, ?)",
                (file_name, mtime),
            )
            file_id = cursor.lastrowid
            assert file_id is not None

        # Store block ranges
        for (wc, shard), (seqno_start, seqno_end) in scan.block_ranges.items():
            _ = cursor.execute(
                "INSERT INTO block_ranges (file_id, workchain, shard, seqno_start, seqno_end) VALUES (?, ?, ?, ?, ?)",
                (file_id, wc, shard, seqno_start, seqno_end),
            )

        # Store MC shard refs
        for ref in scan.mc_shard_refs:
            _ = cursor.execute(
                "INSERT INTO mc_shard_refs (file_id, mc_seqno, shard_workchain, shard, top_seqno, collate_start_ms) VALUES (?, ?, ?, ?, ?, ?)",
                (
                    file_id,
                    ref.mc_seqno,
                    ref.shard_workchain,
                    ref.shard,
                    ref.top_seqno,
                    ref.collate_start_ms,
                ),
            )

        changed_hashes: set[bytes] = set()
        for group_id_val in scan.groups:
            if isinstance(group_id_val, GroupInfo):
                valgroup_hash = group_id_val.valgroup_hash
                _ = cursor.execute(
                    """
                    INSERT INTO groups (valgroup_hash, catchain_seqno, workchain, shard, group_start_est)
                    VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(valgroup_hash) DO UPDATE SET
                    catchain_seqno = COALESCE(excluded.catchain_seqno, catchain_seqno),
                    workchain = COALESCE(excluded.workchain, workchain),
                    shard = COALESCE(excluded.shard, shard),
                    group_start_est = MIN(COALESCE(group_start_est, excluded.group_start_est), excluded.group_start_est)
                    """,
                    (
                        valgroup_hash,
                        group_id_val.catchain_seqno,
                        group_id_val.workchain,
                        group_id_val.shard,
                        group_id_val.group_start_est,
                    ),
                )
            else:
                valgroup_hash = group_id_val.valgroup_hash
                _ = cursor.execute(
                    """
                    INSERT INTO groups (valgroup_hash, group_start_est)
                    VALUES (?, ?)
                    ON CONFLICT(valgroup_hash) DO UPDATE SET
                    group_start_est = MIN(COALESCE(group_start_est, excluded.group_start_est), excluded.group_start_est)
                    """,
                    (valgroup_hash, group_id_val.group_start_est),
                )

            class GroupRow(TypedDict):
                group_id: int

            _ = cursor.execute(
                "SELECT group_id FROM groups WHERE valgroup_hash = ?",
                (valgroup_hash,),
            )
            row = cast(GroupRow, cursor.fetchone())
            db_group_id = row["group_id"]
            _ = cursor.execute(
                "INSERT OR IGNORE INTO group_files (group_id, file_id) VALUES (?, ?)",
                (db_group_id, file_id),
            )
            changed_hashes.add(valgroup_hash)

        conn.commit()
        return changed_hashes

    def _remove_file(self, path: Path, conn: sqlite3.Connection) -> set[bytes]:
        print(f"Removing file {path}", flush=True)
        path = path.resolve()
        file_name = str(path)
        cursor = conn.cursor()

        class FileRow(TypedDict):
            file_id: int

        _ = cursor.execute("SELECT file_id FROM files WHERE file_name = ?", (file_name,))
        rows = cast(list[FileRow], cursor.fetchall())
        if not rows:
            return set()
        file_id = rows[0]["file_id"]

        class HashRow(TypedDict):
            valgroup_hash: bytes

        _ = cursor.execute(
            """
            SELECT g.valgroup_hash
            FROM groups g
            JOIN group_files gf ON g.group_id = gf.group_id
            WHERE gf.file_id = ?
            """,
            (file_id,),
        )
        changed_hashes = {row["valgroup_hash"] for row in cast(list[HashRow], cursor.fetchall())}

        _ = cursor.execute("DELETE FROM group_files WHERE file_id = ?", (file_id,))
        _ = cursor.execute("DELETE FROM block_ranges WHERE file_id = ?", (file_id,))
        _ = cursor.execute("DELETE FROM mc_shard_refs WHERE file_id = ?", (file_id,))
        _ = cursor.execute("DELETE FROM files WHERE file_id = ?", (file_id,))
        _ = cursor.execute(
            """
            DELETE FROM groups
            WHERE NOT EXISTS (
                SELECT 1
                FROM group_files gf
                WHERE gf.group_id = groups.group_id
            )
            """
        )
        conn.commit()
        return changed_hashes

    def _run(self) -> None:
        conn = self._connect()

        notify = RustNotify([str(self._stats_dir)], False, False, 0, False, False)

        try:
            self._initial_scan(conn)
            print("Initial indexing complete, watching for changes", flush=True)

            while not self._stop_event.is_set():
                try:
                    result = notify.watch(100, 50, 60_000, self._stop_event)
                except Exception as exc:
                    logger.exception("File index watcher failed: %s", exc)
                    continue
                if isinstance(result, str):
                    if result in ("stop", "signal"):
                        break
                    continue

                changed_hashes: set[bytes] = set()
                for i, (change_type, path_str) in enumerate(result):
                    path = Path(path_str)
                    try:
                        if change_type in (1, 2):
                            changed_hashes |= self._index_file(path, conn, i, len(result))
                        elif change_type == 3:
                            changed_hashes |= self._remove_file(path, conn)
                    except Exception as exc:
                        logger.exception(
                            "Failed to process file change %s for %s: %s",
                            change_type,
                            path,
                            exc,
                        )

                if changed_hashes and self._callback is not None:
                    self._callback.on_files_changed(changed_hashes)
        finally:
            notify.close()
            conn.close()

    def _initial_scan(self, conn: sqlite3.Connection) -> None:
        class DbFileRow(TypedDict):
            file_name: str

        cursor = conn.cursor()
        _ = cursor.execute("SELECT file_name FROM files")
        db_files = {row["file_name"] for row in cast(list[DbFileRow], cursor.fetchall())}

        disk_files: set[str] = set()
        changed_hashes: set[bytes] = set()
        paths = list(self._stats_dir.iterdir())
        for i, path in enumerate(paths):
            if not path.is_file():
                logger.info("Skipping non-file path during initial scan %s", path)
                continue
            disk_files.add(str(path.resolve()))
            try:
                changed_hashes |= self._index_file(path, conn, i, len(paths))
            except Exception as exc:
                logger.exception("Failed to index file during initial scan %s: %s", path, exc)

        for file_name in db_files - disk_files:
            try:
                changed_hashes |= self._remove_file(Path(file_name), conn)
            except Exception as exc:
                logger.exception(
                    "Failed to remove stale file during initial scan %s: %s",
                    file_name,
                    exc,
                )

        if changed_hashes and self._callback is not None:
            self._callback.on_files_changed(changed_hashes)

    def get_files_for_group(self, valgroup_hash: bytes) -> list[Path]:
        class Row(TypedDict):
            file_name: str

        with self._connect() as conn:
            cursor = conn.cursor()
            _ = cursor.execute(
                """
                SELECT f.file_name
                FROM files f
                JOIN group_files gf ON f.file_id = gf.file_id
                JOIN groups g ON g.group_id = gf.group_id
                WHERE g.valgroup_hash = ?
                """,
                (valgroup_hash,),
            )
            return [Path(row["file_name"]) for row in cast(list[Row], cursor.fetchall())]

    def get_crosslink_files_for_group(self, valgroup_hash: bytes) -> list[Path]:
        """Find additional files needed for crosslink events for the given group.

        Crosslinks are generated from pairs of consecutive MC blocks
        (mc_seqno N, mc_seqno N+1) whose shard_configuration for the group's
        shard yields a finalized range [x+1, y] intersecting the group's shard
        seqno range. For every such pair, both MC blocks' files must be loaded
        so the parser has both shard_configuration entries available.
        For MC groups: returns empty list.
        """

        class GroupRow(TypedDict):
            workchain: int | None
            shard: int | None

        class RangeRow(TypedDict):
            seqno_start: int | None
            seqno_end: int

        class RefRow(TypedDict):
            mc_seqno: int
            top_seqno: int
            file_name: str

        with self._connect() as conn:
            cursor = conn.cursor()

            _ = cursor.execute(
                "SELECT workchain, shard FROM groups WHERE valgroup_hash = ?",
                (valgroup_hash,),
            )
            group_row = cast(GroupRow | None, cursor.fetchone())
            if group_row is None or group_row["workchain"] is None or group_row["shard"] is None:
                return []
            workchain = group_row["workchain"]
            shard = group_row["shard"]
            if workchain == -1:
                return []

            # Get seqno range across all files for this group's shard
            _ = cursor.execute(
                """
                SELECT MIN(br.seqno_start) AS seqno_start, MAX(br.seqno_end) AS seqno_end
                FROM block_ranges br
                JOIN group_files gf ON br.file_id = gf.file_id
                JOIN groups g ON g.group_id = gf.group_id
                WHERE g.valgroup_hash = ? AND br.workchain = ? AND br.shard = ?
                """,
                (valgroup_hash, workchain, shard),
            )
            range_row = cast(RangeRow | None, cursor.fetchone())
            if range_row is None or range_row["seqno_start"] is None:
                return []
            seqno_start = range_row["seqno_start"]
            seqno_end = range_row["seqno_end"]

            # Fetch all MC shard refs for this shard, with their files,
            # ordered by mc_seqno so we can scan consecutive pairs.
            _ = cursor.execute(
                """
                SELECT msr.mc_seqno, msr.top_seqno, f.file_name
                FROM mc_shard_refs msr
                JOIN files f ON f.file_id = msr.file_id
                WHERE msr.shard_workchain = ? AND msr.shard = ?
                ORDER BY msr.mc_seqno
                """,
                (workchain, shard),
            )
            refs = cast(list[RefRow], cursor.fetchall())
            if not refs:
                return []

            # Group files per mc_seqno and keep a representative top_seqno
            # (all rows for the same mc_seqno should agree on top_seqno for
            # a given shard, since there's one MC block per mc_seqno).
            by_mc_seqno: dict[int, tuple[int, set[str]]] = {}
            for row in refs:
                mc_seqno = row["mc_seqno"]
                top_seqno = row["top_seqno"]
                file_name = row["file_name"]
                entry = by_mc_seqno.get(mc_seqno)
                if entry is None:
                    by_mc_seqno[mc_seqno] = (top_seqno, {file_name})
                else:
                    entry[1].add(file_name)

            sorted_mc_seqnos = sorted(by_mc_seqno.keys())
            needed_files: set[str] = set()
            for i in range(1, len(sorted_mc_seqnos)):
                prev_seqno = sorted_mc_seqnos[i - 1]
                curr_seqno = sorted_mc_seqnos[i]
                if curr_seqno != prev_seqno + 1:
                    continue
                prev_top, prev_files = by_mc_seqno[prev_seqno]
                curr_top, curr_files = by_mc_seqno[curr_seqno]
                if curr_top <= prev_top:
                    continue
                # Range finalized by this pair: [prev_top + 1, curr_top]
                if prev_top + 1 > seqno_end or curr_top < seqno_start:
                    continue
                needed_files.update(prev_files)
                needed_files.update(curr_files)

            return [Path(f) for f in needed_files]

    def get_group_hashes_in_files(self, paths: list[Path]) -> set[bytes]:
        """Return valgroup hashes for all groups found in the given files."""
        if not paths:
            return set()

        class Row(TypedDict):
            valgroup_hash: bytes

        with self._connect() as conn:
            cursor = conn.cursor()
            placeholders = ",".join("?" * len(paths))
            _ = cursor.execute(
                f"""
                SELECT DISTINCT g.valgroup_hash
                FROM groups g
                JOIN group_files gf ON g.group_id = gf.group_id
                JOIN files f ON f.file_id = gf.file_id
                WHERE f.file_name IN ({placeholders})
                """,
                [str(p) for p in paths],
            )
            return {row["valgroup_hash"] for row in cast(list[Row], cursor.fetchall())}

    def get_all_groups(self) -> list[GroupData]:
        class Row(TypedDict):
            valgroup_hash: bytes
            catchain_seqno: int | None
            workchain: int | None
            shard: int | None
            group_start_est: float

        with self._connect() as conn:
            cursor = conn.cursor()
            _ = cursor.execute(
                "SELECT valgroup_hash, catchain_seqno, workchain, shard, group_start_est FROM groups"
            )
            result: list[GroupData] = []
            for row in cast(list[Row], cursor.fetchall()):
                if (
                    row["catchain_seqno"] is not None
                    and row["workchain"] is not None
                    and row["shard"] is not None
                ):
                    result.append(
                        GroupInfo(
                            valgroup_hash=row["valgroup_hash"],
                            catchain_seqno=row["catchain_seqno"],
                            workchain=row["workchain"],
                            shard=row["shard"],
                            group_start_est=row["group_start_est"],
                        )
                    )
                else:
                    result.append(UnnamedGroupInfo(row["valgroup_hash"], row["group_start_est"]))
            return result
