from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime

from pydantic import BaseModel


class TestMetadata(BaseModel):
    description: str = ""
    git_branch: str = ""
    git_commit_id: str = ""


@dataclass
class RunMetadata:
    run_id: str
    start_time: datetime
    end_time: datetime | None
    status: str
    metadata: TestMetadata
    node_count: int


class StorageBackend(ABC):
    @abstractmethod
    async def register_run(self, run_id: str, metadata: TestMetadata) -> None:
        pass

    @abstractmethod
    async def update_run_status(
        self, run_id: str, status: str, end_time: datetime | None = None
    ) -> None:
        pass

    @abstractmethod
    async def list_runs(self, limit: int = 50, offset: int = 0) -> list[RunMetadata]:
        pass

    @abstractmethod
    async def get_run_metadata(self, run_id: str) -> RunMetadata | None:
        pass
