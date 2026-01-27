import asyncio
import signal
from pathlib import Path
from typing import final

import uvicorn

from .api import create_app
from .config import ConfigWatcher, DaemonConfig
from .ipc import IPCServer
from .sqlite_storage import SQLiteStorage


@final
class DashboardDaemon:
    def __init__(self, config_path: Path, socket_path: Path, frontend_dir: Path):
        self.config_path = config_path
        self.socket_path = socket_path
        self.frontend_dir = frontend_dir

        self.storage = SQLiteStorage()
        self.ipc_server = IPCServer(socket_path, self.storage)

        self._current_config: DaemonConfig | None = None
        self._uvicorn_server: uvicorn.Server | None = None
        self._server_task: asyncio.Task[None] | None = None
        self._shutdown_event = asyncio.Event()

        self.config_watcher = ConfigWatcher(config_path, self._on_config_change)

    def _on_config_change(self, new_config: DaemonConfig | None) -> None:
        if new_config is None:
            _ = asyncio.create_task(self._stop_http_server())
            _ = asyncio.create_task(self._shutdown())
        elif new_config != self._current_config:
            _ = asyncio.create_task(self._restart_http_server(new_config))

    async def _start_http_server(self, config: DaemonConfig) -> None:
        app = create_app(self.storage, str(self.frontend_dir))

        uvicorn_config = uvicorn.Config(
            app,
            host=config.host,
            port=config.port,
            log_level="info",
        )

        self._uvicorn_server = uvicorn.Server(uvicorn_config)
        self._current_config = config

        self._server_task = asyncio.create_task(self._uvicorn_server.serve())

    async def _stop_http_server(self) -> None:
        if self._uvicorn_server:
            self._uvicorn_server.should_exit = True
            if self._server_task:
                await self._server_task
            self._uvicorn_server = None
            self._server_task = None
            self._current_config = None

    async def _restart_http_server(self, new_config: DaemonConfig) -> None:
        await self._stop_http_server()
        await self._start_http_server(new_config)

    async def _shutdown(self) -> None:
        self._shutdown_event.set()

    async def start(self) -> None:
        await self.ipc_server.start()

        initial_config = self.config_watcher.get_current_config()
        if initial_config:
            await self._start_http_server(initial_config)

        watch_task = asyncio.create_task(self.config_watcher.watch())

        loop = asyncio.get_running_loop()

        def signal_handler() -> None:
            _ = asyncio.create_task(self._shutdown())

        loop.add_signal_handler(signal.SIGTERM, signal_handler)
        loop.add_signal_handler(signal.SIGINT, signal_handler)

        _ = await self._shutdown_event.wait()

        self.config_watcher.stop()
        await watch_task

        await self._stop_http_server()
        await self.ipc_server.stop()
        self.storage.close()
