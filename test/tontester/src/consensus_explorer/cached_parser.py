import base64
import threading
from collections import OrderedDict
from pathlib import Path
from typing import final, override

from .file_index import FileIndex, FileIndexCallback
from .models import ConsensusData, GroupData
from .parser import GroupParser
from .parser.parser_session_stats import ParserSessionStats


@final
class CachedGroupParser(GroupParser, FileIndexCallback):
    def __init__(self, file_index: FileIndex, hostname_regex: str, cache_size: int = 32):
        self._file_index = file_index
        self._hostname_regex = hostname_regex
        self._cache_size = cache_size
        self._cache: OrderedDict[bytes, ConsensusData] = OrderedDict()
        self._dirty_groups: set[bytes] = set()
        self._lock = threading.Lock()

    @override
    def on_files_changed(self, changed_groups: set[bytes]) -> None:
        with self._lock:
            self._dirty_groups |= changed_groups

    def _resolve_name(self, valgroup_name: str) -> bytes:
        for info in self._file_index.get_all_groups():
            if info.valgroup_name == valgroup_name:
                return info.valgroup_hash
        return base64.b64decode(valgroup_name)

    @override
    def list_groups(self) -> list[GroupData]:
        return self._file_index.get_all_groups()

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        valgroup_hash = self._resolve_name(valgroup_name)

        with self._lock:
            dirty = valgroup_hash in self._dirty_groups
            self._dirty_groups.discard(valgroup_hash)

            if not dirty and valgroup_hash in self._cache:
                entry = self._cache[valgroup_hash]
                self._cache.move_to_end(valgroup_hash)
                return entry

        log_paths: list[Path] = self._file_index.get_files_for_group(valgroup_hash)

        parser = ParserSessionStats(
            log_paths,
            self._hostname_regex,
            with_cache=False,
            target_group_hash=valgroup_hash,
        )
        data = parser.parse()

        with self._lock:
            self._cache[valgroup_hash] = data
            self._cache.move_to_end(valgroup_hash)

            while len(self._cache) > self._cache_size:
                _ = self._cache.popitem(last=False)

        return data
