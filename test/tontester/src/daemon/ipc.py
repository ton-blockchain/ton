import asyncio
import json
from pathlib import Path
from typing import Any

from .storage import StorageBackend


class IPCServer:
    def __init__(self, socket_path: Path, storage: StorageBackend):
        self.socket_path = socket_path
        self.storage = storage
        self._server: asyncio.Server | None = None
        self._active_connections: set[asyncio.Task[None]] = set()

    async def handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        run_id: str | None = None
        try:
            data = await reader.read(1024 * 1024)
            if not data:
                return

            hello = json.loads(data.decode())

            if hello.get("command") == "ping":
                response = {"status": "ok", "message": "pong"}
                writer.write(json.dumps(response).encode())
                await writer.drain()
                writer.close()
                await writer.wait_closed()
                return

            run_id = hello.get("run_id")
            metadata = hello.get("metadata", {})

            if not run_id:
                return

            await self.storage.register_run(run_id, metadata)

            response = {"status": "ok", "run_id": run_id}
            writer.write(json.dumps(response).encode())
            await writer.drain()

            await reader.read()

        except Exception:
            pass
        finally:
            if run_id:
                await self.storage.update_run_status(run_id, "completed")

            writer.close()
            await writer.wait_closed()

    async def start(self) -> None:
        if self.socket_path.exists():
            self.socket_path.unlink()

        self.socket_path.parent.mkdir(parents=True, exist_ok=True)

        self._server = await asyncio.start_unix_server(
            self.handle_client,
            path=str(self.socket_path),
        )

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        if self.socket_path.exists():
            self.socket_path.unlink()


class IPCClient:
    def __init__(self, socket_path: Path):
        self.socket_path = socket_path
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def connect_and_register(self, run_id: str, metadata: dict[str, Any]) -> None:
        self._reader, self._writer = await asyncio.open_unix_connection(str(self.socket_path))

        hello = {"run_id": run_id, "metadata": metadata}
        self._writer.write(json.dumps(hello).encode())
        await self._writer.drain()

        response_data = await self._reader.read(1024 * 1024)
        response: dict[str, Any] = json.loads(response_data.decode())

        if response.get("status") != "ok":
            raise RuntimeError(f"Failed to register run: {response}")

    async def disconnect(self) -> None:
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()
            self._reader = None
            self._writer = None

    async def ping(self) -> dict[str, Any]:
        reader, writer = await asyncio.open_unix_connection(str(self.socket_path))
        try:
            ping_msg = {"command": "ping"}
            writer.write(json.dumps(ping_msg).encode())
            await writer.drain()

            data = await reader.read(1024)
            response: dict[str, Any] = json.loads(data.decode())
            return response
        finally:
            writer.close()
            await writer.wait_closed()
