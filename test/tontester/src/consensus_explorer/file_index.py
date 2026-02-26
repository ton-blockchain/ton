import gzip
import sqlite3
import threading
from pathlib import Path
from typing import Protocol, TypedDict, cast, final

from tonapi.ton_api import Consensus_stats_events, Consensus_stats_id
from watchfiles._rust_notify import RustNotify

from .models import GroupData, GroupInfo, UnnamedGroupInfo
from .parser.parser_session_stats import open_stats_file


class FileIndexCallback(Protocol):
    def on_files_changed(self, changed_groups: set[bytes]) -> None: ...


@final
class FileIndex:
    def __init__(self, stats_dir: Path, db_path: Path):
        self._stats_dir = stats_dir
        self._db_path = db_path
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
        """)

    def install_callback(self, callback: FileIndexCallback) -> None:
        self._callback = callback

    def __enter__(self) -> "FileIndex":
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

    @staticmethod
    def _scan_file_for_groups(path: Path) -> set[GroupData]:
        groups: set[GroupData] = set()
        try:
            with open_stats_file(path) as f:
                for line in f:
                    if not line.startswith('{"@type":"consensus.stats.events"'):
                        continue
                    parsed = Consensus_stats_events.from_json(line)
                    valgroup_hash = parsed.id
                    found_id = False
                    min_ts = float("inf")
                    for te in parsed.events:
                        min_ts = min(min_ts, te.ts)
                        if isinstance(te.event, Consensus_stats_id):
                            groups.add(
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
                        groups.add(
                            UnnamedGroupInfo(
                                valgroup_hash=valgroup_hash,
                                group_start_est=min_ts,
                            )
                        )
        except (OSError, gzip.BadGzipFile):
            pass
        return groups

    def _index_file(
        self, path: Path, conn: sqlite3.Connection, file_idx: int, file_count: int
    ) -> set[bytes]:
        path = path.resolve()
        file_name = str(path)

        mtime = path.stat().st_mtime

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
        groups = self._scan_file_for_groups(path)

        if existing:
            _ = cursor.execute(
                "UPDATE files SET mtime = ? WHERE file_id = ?",
                (mtime, file_id),
            )
            _ = cursor.execute(
                "DELETE FROM group_files WHERE file_id = ?",
                (file_id,),
            )
        else:
            _ = cursor.execute(
                "INSERT INTO files (file_name, mtime) VALUES (?, ?)",
                (file_name, mtime),
            )
            file_id = cursor.lastrowid
            assert file_id is not None

        changed_hashes: set[bytes] = set()
        for group_id_val in groups:
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
                result = notify.watch(100, 50, 60_000, self._stop_event)
                if isinstance(result, str):
                    if result in ("stop", "signal"):
                        break
                    continue

                changed_hashes: set[bytes] = set()
                for i, (change_type, path_str) in enumerate(result):
                    path = Path(path_str)
                    if change_type in (1, 2):
                        changed_hashes |= self._index_file(path, conn, i, len(result))
                    elif change_type == 3:
                        changed_hashes |= self._remove_file(path, conn)

                if changed_hashes and self._callback is not None:
                    self._callback.on_files_changed(changed_hashes)
        finally:
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
            disk_files.add(str(path.resolve()))
            changed_hashes |= self._index_file(path, conn, i, len(paths))

        for file_name in db_files - disk_files:
            changed_hashes |= self._remove_file(Path(file_name), conn)

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
