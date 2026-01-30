import asyncio
import logging
import signal
from pathlib import Path
import sys
from typing import final

from pydantic import ValidationError
import uvicorn

from .api import create_app
from .config import ConfigWatcher, DaemonConfig
from .container import ServiceManager
from .ipc import IPCServer
from .sqlite_storage import SQLiteStorage

logger = logging.getLogger(__name__)


@final
class DashboardDaemon:
    def __init__(self, config_path: Path, socket_path: Path, frontend_dir: Path):
        self.config_path = config_path
        self.socket_path = socket_path
        self.frontend_dir = frontend_dir

        self.storage = SQLiteStorage()
        self.ipc_server = IPCServer(socket_path, self.storage)

        self._shutdown_event = asyncio.Event()
        self._config_change_event = asyncio.Event()
        self._new_config: DaemonConfig | None = None

        self._http_server_task: asyncio.Task[None] | None = None
        self._container_task: asyncio.Task[None] | None = None

        self.config_watcher = ConfigWatcher(config_path, self._on_config_change)

    def _on_config_change(self, new_config: DaemonConfig | ValidationError | None) -> None:
        if new_config is None:
            self._shutdown_event.set()
        elif isinstance(new_config, ValidationError):
            logger.error(f"Invalid configuration: {new_config}")
        elif new_config != self._new_config:
            self._new_config = new_config
            self._config_change_event.set()

    async def _http_server_manager(self, initial_config: DaemonConfig) -> None:
        current_config = initial_config
        uvicorn_server: uvicorn.Server | None = None
        server_task: asyncio.Task[None] | None = None

        try:
            while True:
                app = create_app(self.storage, str(self.frontend_dir))
                uvicorn_config = uvicorn.Config(
                    app,
                    host=current_config.host,
                    port=current_config.dashboard_port,
                    log_level="info",
                )
                uvicorn_server = uvicorn.Server(uvicorn_config)
                server_task = asyncio.create_task(uvicorn_server.serve())

                _ = await asyncio.wait(
                    [server_task, asyncio.create_task(self._config_change_event.wait())],
                    return_when=asyncio.FIRST_COMPLETED,
                )

                if self._config_change_event.is_set():
                    self._config_change_event.clear()
                    if self._new_config is not None:
                        current_config = self._new_config

                    uvicorn_server.should_exit = True
                    await server_task
                    uvicorn_server = None
                    server_task = None
                else:
                    break

        except asyncio.CancelledError:
            if uvicorn_server:
                uvicorn_server.should_exit = True
            if server_task and not server_task.done():
                await server_task
            raise

    async def _container_manager(self, initial_config: DaemonConfig) -> None:
        services_dir = self.socket_path.parent / "services"
        services_dir.mkdir(parents=True, exist_ok=True)

        service_manager = ServiceManager(
            instance_dir=services_dir,
            prometheus_port=initial_config.prometheus_port,
            grafana_port=initial_config.grafana_port,
        )

        try:
            await service_manager.start_all()
        except RuntimeError as e:
            logger.warning(f"Could not start services: {e}")

        try:
            _ = await self._shutdown_event.wait()
        except asyncio.CancelledError:
            pass
        finally:
            await service_manager.stop_all()
            await service_manager.cleanup()

    async def run(self) -> None:
        await self.ipc_server.start()

        initial_config = self.config_watcher.get_current_config()
        if initial_config is None:
            raise RuntimeError("No valid configuration found")
        if isinstance(initial_config, ValidationError):
            raise RuntimeError("Invalid configuration") from initial_config

        self._new_config = initial_config

        self._http_server_task = asyncio.create_task(self._http_server_manager(initial_config))
        self._container_task = asyncio.create_task(self._container_manager(initial_config))
        watch_task = asyncio.create_task(self.config_watcher.watch())

        loop = asyncio.get_running_loop()

        def signal_handler() -> None:
            self._shutdown_event.set()

        loop.add_signal_handler(signal.SIGTERM, signal_handler)
        loop.add_signal_handler(signal.SIGINT, signal_handler)

        try:
            _ = await self._shutdown_event.wait()
        finally:
            self.config_watcher.stop()

            if self._http_server_task and not self._http_server_task.done():
                _ = self._http_server_task.cancel()
                try:
                    await self._http_server_task
                except asyncio.CancelledError:
                    pass

            if self._container_task and not self._container_task.done():
                _ = self._container_task.cancel()
                try:
                    await self._container_task
                except asyncio.CancelledError:
                    pass

            await watch_task
            await self.ipc_server.stop()
            self.storage.close()
