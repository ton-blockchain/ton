import asyncio
import json
from pathlib import Path
from typing import cast, final

from pydantic import BaseModel

from tl import JSONSerializable

from .storage import StorageBackend, TestMetadata


class PingResponse(BaseModel):
    status: str
    message: str


class RegisterResponse(BaseModel):
    status: str
    run_id: str


@final
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

            hello = cast(JSONSerializable, json.loads(data.decode()))
            if not isinstance(hello, dict):
                return

            command = hello.get("command")
            if command == "ping":
                response = PingResponse(status="ok", message="pong")
                writer.write(response.model_dump_json().encode())
                await writer.drain()
                writer.close()
                await writer.wait_closed()
                return

            run_id_raw = hello.get("run_id")
            if not isinstance(run_id_raw, str):
                return
            run_id = run_id_raw

            metadata_raw = hello.get("metadata", {})
            if not isinstance(metadata_raw, dict):
                return

            metadata = TestMetadata.model_validate(metadata_raw)

            await self.storage.register_run(run_id, metadata)

            response = RegisterResponse(status="ok", run_id=run_id)
            writer.write(response.model_dump_json().encode())
            await writer.drain()

            _ = await reader.read()

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


@final
class IPCClient:
    def __init__(self, socket_path: Path):
        self.socket_path = socket_path
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def connect_and_register(self, run_id: str, metadata: TestMetadata) -> None:
        self._reader, self._writer = await asyncio.open_unix_connection(str(self.socket_path))

        hello = {"run_id": run_id, "metadata": metadata.model_dump()}
        self._writer.write(json.dumps(hello).encode())
        await self._writer.drain()

        response_data = await self._reader.read(1024 * 1024)
        response = RegisterResponse.model_validate_json(response_data)

        if response.status != "ok":
            raise RuntimeError(f"Failed to register run: {response}")

    async def disconnect(self) -> None:
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()
            self._reader = None
            self._writer = None

    async def ping(self) -> PingResponse:
        reader, writer = await asyncio.open_unix_connection(str(self.socket_path))
        try:
            ping_msg = {"command": "ping"}
            writer.write(json.dumps(ping_msg).encode())
            await writer.drain()

            data = await reader.read(1024)
            return PingResponse.model_validate_json(data)
        finally:
            writer.close()
            await writer.wait_closed()
