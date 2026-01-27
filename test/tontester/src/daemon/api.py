from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .storage import RunMetadata, StorageBackend


class TestInfo(BaseModel):
    fileName: str
    fileDateTime: str
    description: str
    gitBranch: str
    gitCommitId: str
    nodes: str
    startDateTime: str
    startTimeISO: str
    endTimeISO: str
    bpsMc: str
    spbMc: str
    duration: str
    configuration: dict[str, str]
    operations: list[str]


def run_to_test_info(run: RunMetadata) -> TestInfo:
    return TestInfo(
        fileName=f"{run.run_id}.txt",
        fileDateTime=run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
        description=run.metadata.description,
        gitBranch=run.metadata.git_branch,
        gitCommitId=run.metadata.git_commit_id,
        nodes=str(run.node_count),
        startDateTime=run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
        startTimeISO=run.start_time.isoformat() + "Z",
        endTimeISO=run.end_time.isoformat() + "Z" if run.end_time else "",
        bpsMc="",
        spbMc="",
        duration="",
        configuration={},
        operations=[],
    )


def create_app(storage: StorageBackend, frontend_dir: str) -> FastAPI:
    app = FastAPI(title="TON Dashboard Daemon")

    @app.get("/api/tests")
    async def list_tests() -> list[TestInfo]:  # pyright: ignore[reportUnusedFunction]
        runs = await storage.list_runs(limit=100)
        return [run_to_test_info(run) for run in runs]

    @app.get("/api/tests/{run_id}")
    async def get_test(run_id: str) -> TestInfo:  # pyright: ignore[reportUnusedFunction]
        run = await storage.get_run_metadata(run_id)
        if not run:
            raise HTTPException(status_code=404, detail="Run not found")
        return run_to_test_info(run)

    @app.get("/health")
    async def health() -> dict[str, str]:  # pyright: ignore[reportUnusedFunction]
        return {"status": "ok"}

    app.mount("/", StaticFiles(directory=frontend_dir, html=True), name="frontend")

    return app
