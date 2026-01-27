from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles

from .storage import StorageBackend


def create_app(storage: StorageBackend, frontend_dir: str) -> FastAPI:
    app = FastAPI(title="TON Dashboard Daemon")

    @app.get("/api/tests")
    async def list_tests() -> list[dict[str, Any]]:
        runs = await storage.list_runs(limit=100)

        result: list[dict[str, Any]] = []
        for run in runs:
            result.append(
                {
                    "fileName": f"{run.run_id}.txt",
                    "fileDateTime": run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
                    "description": run.metadata.get("description", "No description"),
                    "gitBranch": run.metadata.get("git_branch", ""),
                    "gitCommitId": run.metadata.get("git_commit_id", ""),
                    "nodes": str(run.node_count),
                    "startDateTime": run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
                    "startTimeISO": run.start_time.isoformat() + "Z",
                    "endTimeISO": run.end_time.isoformat() + "Z" if run.end_time else "",
                    "bpsMc": "",
                    "spbMc": "",
                    "duration": "",
                    "configuration": {},
                    "operations": [],
                }
            )

        return result

    @app.get("/api/tests/{run_id}")
    async def get_test(run_id: str) -> dict[str, Any]:
        run = await storage.get_run_metadata(run_id)
        if not run:
            raise HTTPException(status_code=404, detail="Run not found")

        return {
            "fileName": f"{run.run_id}.txt",
            "fileDateTime": run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
            "description": run.metadata.get("description", "No description"),
            "gitBranch": run.metadata.get("git_branch", ""),
            "gitCommitId": run.metadata.get("git_commit_id", ""),
            "nodes": str(run.node_count),
            "startDateTime": run.start_time.strftime("%Y-%m-%d %H:%M:%S"),
            "startTimeISO": run.start_time.isoformat() + "Z",
            "endTimeISO": run.end_time.isoformat() + "Z" if run.end_time else "",
            "bpsMc": "",
            "spbMc": "",
            "duration": "",
            "configuration": run.metadata.get("configuration", {}),
            "operations": [],
        }

    @app.get("/health")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    app.mount("/", StaticFiles(directory=frontend_dir, html=True), name="frontend")

    return app
