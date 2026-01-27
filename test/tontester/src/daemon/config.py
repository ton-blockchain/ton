import asyncio
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import yaml
from watchfiles import Change, awatch


@dataclass
class DaemonConfig:
    host: str
    port: int


class ConfigWatcher:
    def __init__(self, config_path: Path, on_config_change: Callable[[DaemonConfig | None], None]):
        self.config_path = config_path
        self.on_config_change = on_config_change
        self._stop_event = asyncio.Event()
        self._last_config: DaemonConfig | None = None

    def _load_config(self) -> DaemonConfig | None:
        if not self.config_path.exists():
            return None

        with open(self.config_path, "r") as f:
            data = yaml.safe_load(f)

        if not data or not isinstance(data, dict):
            return None

        return DaemonConfig(
            host=str(data.get("host", "127.0.0.1")),
            port=int(data.get("port", 8080)),
        )

    async def watch(self) -> None:
        config_dir = self.config_path.parent
        config_filename = self.config_path.name

        initial_config = self._load_config()
        if initial_config != self._last_config:
            self._last_config = initial_config
            self.on_config_change(initial_config)

        async for changes in awatch(config_dir, stop_event=self._stop_event):
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

    def get_current_config(self) -> DaemonConfig | None:
        if not self.config_path.exists():
            return None
        return self._load_config()
