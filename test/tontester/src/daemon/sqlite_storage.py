import json
import sqlite3
from datetime import datetime
from typing import Any

from .storage import MetricPoint, RunMetadata, StorageBackend


class SQLiteStorage(StorageBackend):
    def __init__(self, db_path: str | None = None):
        self.db_path = db_path or ":memory:"
        self.conn = sqlite3.connect(self.db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self._init_schema()

    def _init_schema(self) -> None:
        cursor = self.conn.cursor()

        cursor.execute("""
            CREATE TABLE IF NOT EXISTS runs (
                run_id TEXT PRIMARY KEY,
                start_time REAL NOT NULL,
                end_time REAL,
                status TEXT NOT NULL,
                metadata TEXT NOT NULL,
                node_count INTEGER NOT NULL
            )
        """)

        cursor.execute("""
            CREATE TABLE IF NOT EXISTS metrics (
                run_id TEXT NOT NULL,
                node_id TEXT NOT NULL,
                metric_name TEXT NOT NULL,
                timestamp REAL NOT NULL,
                workchain INTEGER,
                value REAL NOT NULL,
                tags TEXT,
                FOREIGN KEY (run_id) REFERENCES runs(run_id)
            )
        """)

        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_metrics_run_time
            ON metrics(run_id, timestamp)
        """)

        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_metrics_name
            ON metrics(run_id, metric_name)
        """)

        self.conn.commit()

    async def register_run(self, run_id: str, metadata: dict[str, Any]) -> None:
        cursor = self.conn.cursor()
        cursor.execute(
            """
            INSERT INTO runs (run_id, start_time, end_time, status, metadata, node_count)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                run_id,
                datetime.now().timestamp(),
                None,
                "running",
                json.dumps(metadata),
                metadata.get("node_count", 0),
            ),
        )
        self.conn.commit()

    async def update_run_status(
        self, run_id: str, status: str, end_time: datetime | None = None
    ) -> None:
        cursor = self.conn.cursor()
        cursor.execute(
            """
            UPDATE runs
            SET status = ?, end_time = ?
            WHERE run_id = ?
            """,
            (status, end_time.timestamp() if end_time else None, run_id),
        )
        self.conn.commit()

    async def write_metric(self, metric: MetricPoint) -> None:
        cursor = self.conn.cursor()
        cursor.execute(
            """
            INSERT INTO metrics (run_id, node_id, metric_name, timestamp, workchain, value, tags)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                metric.run_id,
                metric.node_id,
                metric.metric_name,
                metric.timestamp.timestamp(),
                metric.workchain,
                metric.value,
                json.dumps(metric.tags) if metric.tags else None,
            ),
        )
        self.conn.commit()

    async def list_runs(self, limit: int = 50, offset: int = 0) -> list[RunMetadata]:
        cursor = self.conn.cursor()
        cursor.execute(
            """
            SELECT run_id, start_time, end_time, status, metadata, node_count
            FROM runs
            ORDER BY start_time DESC
            LIMIT ? OFFSET ?
            """,
            (limit, offset),
        )

        runs: list[RunMetadata] = []
        for row in cursor.fetchall():
            runs.append(
                RunMetadata(
                    run_id=row["run_id"],
                    start_time=datetime.fromtimestamp(row["start_time"]),
                    end_time=datetime.fromtimestamp(row["end_time"]) if row["end_time"] else None,
                    status=row["status"],
                    metadata=json.loads(row["metadata"]),
                    node_count=row["node_count"],
                )
            )
        return runs

    async def get_run_metadata(self, run_id: str) -> RunMetadata | None:
        cursor = self.conn.cursor()
        cursor.execute(
            """
            SELECT run_id, start_time, end_time, status, metadata, node_count
            FROM runs
            WHERE run_id = ?
            """,
            (run_id,),
        )

        row = cursor.fetchone()
        if not row:
            return None

        return RunMetadata(
            run_id=row["run_id"],
            start_time=datetime.fromtimestamp(row["start_time"]),
            end_time=datetime.fromtimestamp(row["end_time"]) if row["end_time"] else None,
            status=row["status"],
            metadata=json.loads(row["metadata"]),
            node_count=row["node_count"],
        )

    async def query_metrics(
        self,
        run_id: str,
        metric_names: list[str],
        start_time: datetime | None,
        end_time: datetime | None,
        workchain: int | None = None,
    ) -> list[dict[str, Any]]:
        cursor = self.conn.cursor()

        query = """
            SELECT timestamp, metric_name, AVG(value) as value
            FROM metrics
            WHERE run_id = ? AND metric_name IN ({})
        """.format(",".join("?" * len(metric_names)))

        params: list[Any] = [run_id, *metric_names]

        if start_time:
            query += " AND timestamp >= ?"
            params.append(start_time.timestamp())

        if end_time:
            query += " AND timestamp <= ?"
            params.append(end_time.timestamp())

        if workchain is not None:
            query += " AND workchain = ?"
            params.append(workchain)

        query += " GROUP BY timestamp, metric_name ORDER BY timestamp"

        cursor.execute(query, params)

        results: list[dict[str, Any]] = []
        for row in cursor.fetchall():
            results.append(
                {
                    "timestamp": datetime.fromtimestamp(row["timestamp"]).isoformat(),
                    "metric_name": row["metric_name"],
                    "value": row["value"],
                }
            )

        return results

    def close(self) -> None:
        self.conn.close()
