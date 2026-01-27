from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime
from typing import Any


@dataclass
class RunMetadata:
    run_id: str
    start_time: datetime
    end_time: datetime | None
    status: str
    metadata: dict[str, Any]
    node_count: int


@dataclass
class MetricPoint:
    run_id: str
    node_id: str
    metric_name: str
    timestamp: datetime
    value: float
    workchain: int | None
    tags: dict[str, Any] | None


class StorageBackend(ABC):
    @abstractmethod
    async def register_run(self, run_id: str, metadata: dict[str, Any]) -> None:
        pass

    @abstractmethod
    async def update_run_status(
        self, run_id: str, status: str, end_time: datetime | None = None
    ) -> None:
        pass

    @abstractmethod
    async def write_metric(self, metric: MetricPoint) -> None:
        pass

    @abstractmethod
    async def list_runs(self, limit: int = 50, offset: int = 0) -> list[RunMetadata]:
        pass

    @abstractmethod
    async def get_run_metadata(self, run_id: str) -> RunMetadata | None:
        pass

    @abstractmethod
    async def query_metrics(
        self,
        run_id: str,
        metric_names: list[str],
        start_time: datetime | None,
        end_time: datetime | None,
        workchain: int | None = None,
    ) -> list[dict[str, Any]]:
        pass
