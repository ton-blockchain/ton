import asyncio
from pathlib import Path
from typing import Callable, final

from pydantic import BaseModel, ValidationError
from watchfiles import Change, awatch  # pyright: ignore[reportUnknownVariableType]


class DaemonConfig(BaseModel):
    host: str = "127.0.0.1"
    dashboard_port: int = 8080
    prometheus_port: int = 9090
    grafana_port: int = 3000


@final
class ConfigWatcher:
    def __init__(
        self,
        config_path: Path,
        on_config_change: Callable[[DaemonConfig | ValidationError | None], None],
    ):
        self.config_path = config_path
        self.on_config_change = on_config_change
        self._stop_event = asyncio.Event()
        self._last_config = self._load_config()

    def _load_config(self):
        if not self.config_path.exists():
            return None

        try:
            with open(self.config_path, "r") as f:
                json_content = f.read()
            return DaemonConfig.model_validate_json(json_content)
        except ValidationError as e:
            return e

    async def watch(self) -> None:
        config_dir = self.config_path.parent
        config_filename = self.config_path.name

        initial_config = self._load_config()
        if initial_config != self._last_config:
            self._last_config = initial_config
            self.on_config_change(initial_config)

        async for changes in awatch(config_dir, stop_event=self._stop_event, recursive=False):
            for change_type, changed_path in changes:
                if Path(changed_path).name == config_filename:
                    if change_type == Change.deleted:
                        if self._last_config is not None:
                            self._last_config = None
                            self.on_config_change(None)
                    elif change_type in (Change.added, Change.modified):
                        new_config = self._load_config()
                        if new_config != self._last_config:
                            self._last_config = new_config
                            self.on_config_change(new_config)

    def stop(self) -> None:
        self._stop_event.set()

    def get_current_config(self):
        return self._last_config
