import json
import sqlite3
from datetime import datetime
from typing import TypedDict, cast, final, override

from tl import JSONSerializable

from .storage import RunMetadata, StorageBackend, TestMetadata


@final
class SQLiteStorage(StorageBackend):
    db_path: str
    conn: sqlite3.Connection

    def __init__(self, db_path: str | None = None):
        self.db_path = db_path or ":memory:"
        self.conn = sqlite3.connect(self.db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self._init_schema()

    def _init_schema(self) -> None:
        cursor = self.conn.cursor()

        _ = cursor.execute("""
            CREATE TABLE IF NOT EXISTS runs (
                run_id TEXT PRIMARY KEY,
                start_time REAL NOT NULL,
                end_time REAL,
                status TEXT NOT NULL,
                metadata TEXT NOT NULL,
                node_count INTEGER NOT NULL
            )
        """)

        _ = cursor.execute("""
            CREATE TABLE IF NOT EXISTS metrics (
                run_id TEXT NOT NULL,
                node_id TEXT NOT NULL,
                metric_name TEXT NOT NULL,
                timestamp REAL NOT NULL,
                workchain INTEGER,
                value REAL NOT NULL,
                FOREIGN KEY (run_id) REFERENCES runs(run_id)
            )
        """)

        _ = cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_metrics_run_time
            ON metrics(run_id, timestamp)
        """)

        _ = cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_metrics_name
            ON metrics(run_id, metric_name)
        """)

        self.conn.commit()

    @override
    async def register_run(self, run_id: str, metadata: TestMetadata) -> None:
        cursor = self.conn.cursor()
        _ = cursor.execute(
            """
            INSERT INTO runs (run_id, start_time, end_time, status, metadata, node_count)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                run_id,
                datetime.now().timestamp(),
                None,
                "running",
                metadata.model_dump_json(),
                0,
            ),
        )
        self.conn.commit()

    @override
    async def update_run_status(
        self, run_id: str, status: str, end_time: datetime | None = None
    ) -> None:
        cursor = self.conn.cursor()
        _ = cursor.execute(
            """
            UPDATE runs
            SET status = ?, end_time = ?
            WHERE run_id = ?
            """,
            (status, end_time.timestamp() if end_time else None, run_id),
        )
        self.conn.commit()

    @override
    async def list_runs(self, limit: int = 50, offset: int = 0) -> list[RunMetadata]:
        cursor = self.conn.cursor()
        _ = cursor.execute(
            """
            SELECT run_id, start_time, end_time, status, metadata, node_count
            FROM runs
            ORDER BY start_time DESC
            LIMIT ? OFFSET ?
            """,
            (limit, offset),
        )

        class SelectRow(TypedDict):
            run_id: str
            start_time: float
            end_time: float | None
            status: str
            metadata: str
            node_count: int

        runs: list[RunMetadata] = []
        for row in cast(list[SelectRow], cursor.fetchall()):
            metadata_json = cast(JSONSerializable, json.loads(row["metadata"]))
            if not isinstance(metadata_json, dict):
                continue
            metadata = TestMetadata.model_validate(metadata_json)

            runs.append(
                RunMetadata(
                    run_id=row["run_id"],
                    start_time=datetime.fromtimestamp(row["start_time"]),
                    end_time=datetime.fromtimestamp(row["end_time"]) if row["end_time"] else None,
                    status=row["status"],
                    metadata=metadata,
                    node_count=row["node_count"],
                )
            )
        return runs

    @override
    async def get_run_metadata(self, run_id: str) -> RunMetadata | None:
        cursor = self.conn.cursor()
        _ = cursor.execute(
            """
            SELECT run_id, start_time, end_time, status, metadata, node_count
            FROM runs
            WHERE run_id = ?
            """,
            (run_id,),
        )

        class SelectRow(TypedDict):
            run_id: str
            start_time: float
            end_time: float | None
            status: str
            metadata: str
            node_count: int

        row = cast(SelectRow, cursor.fetchone())
        if not row:
            return None

        metadata_json = cast(JSONSerializable, json.loads(row["metadata"]))
        if not isinstance(metadata_json, dict):
            return None
        metadata = TestMetadata.model_validate(metadata_json)

        return RunMetadata(
            run_id=row["run_id"],
            start_time=datetime.fromtimestamp(row["start_time"]),
            end_time=datetime.fromtimestamp(row["end_time"]) if row["end_time"] else None,
            status=row["status"],
            metadata=metadata,
            node_count=row["node_count"],
        )

    def close(self) -> None:
        self.conn.close()
