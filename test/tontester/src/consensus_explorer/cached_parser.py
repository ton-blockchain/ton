import base64
import logging
import threading
from collections import OrderedDict
from pathlib import Path
from typing import final, override

from .file_index import FileIndex, FileIndexCallback
from .models import ConsensusData, GroupData
from .parser import GroupParser
from .parser.parser_session_stats import ParserSessionStats

logger = logging.getLogger(__name__)


@final
class CachedGroupParser(GroupParser, FileIndexCallback):
    def __init__(
        self,
        file_index: FileIndex,
        hostname_regex: str,
        cache_size: int = 32,
        sudo_helper: str | None = None,
    ):
        self._file_index = file_index
        self._hostname_regex = hostname_regex
        self._sudo_helper = sudo_helper
        self._cache_size = cache_size
        self._cache: OrderedDict[bytes, ConsensusData] = OrderedDict()
        self._dirty_groups: set[bytes] = set()
        self._lock = threading.Lock()

    @override
    def on_files_changed(self, changed_groups: set[bytes]) -> None:
        with self._lock:
            self._dirty_groups |= changed_groups
        logger.info("on_files_changed: %d groups invalidated", len(changed_groups))

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
        import time

        valgroup_hash = self._resolve_name(valgroup_name)

        with self._lock:
            dirty = valgroup_hash in self._dirty_groups
            self._dirty_groups.discard(valgroup_hash)

            if not dirty and valgroup_hash in self._cache:
                entry = self._cache[valgroup_hash]
                self._cache.move_to_end(valgroup_hash)
                logger.debug("parse_group %s: cache hit", valgroup_name)
                return entry

        logger.info(
            "parse_group %s: cache miss (dirty=%s, cached=%s)",
            valgroup_name,
            dirty,
            valgroup_hash in self._cache,
        )

        t0 = time.monotonic()
        log_paths: list[Path] = self._file_index.get_files_for_group(valgroup_hash)
        crosslink_paths = self._file_index.get_crosslink_files_for_group(valgroup_hash)
        existing = set(log_paths)
        for p in crosslink_paths:
            if p not in existing:
                log_paths.append(p)
        target_hashes = {valgroup_hash}
        if crosslink_paths:
            target_hashes |= self._file_index.get_group_hashes_in_files(crosslink_paths)
        logger.info(
            "parse_group %s: %d files to parse (%d crosslink)",
            valgroup_name,
            len(log_paths),
            len(crosslink_paths),
        )

        parser = ParserSessionStats(
            log_paths,
            self._hostname_regex,
            with_cache=False,
            target_group_hashes=target_hashes,
            sudo_helper=self._sudo_helper,
        )
        data = parser.parse()
        elapsed = time.monotonic() - t0
        logger.info(
            "parse_group %s: parsed in %.2fs (%d slots, %d events)",
            valgroup_name,
            elapsed,
            len(data.slots),
            len(data.events),
        )

        with self._lock:
            self._cache[valgroup_hash] = data
            self._cache.move_to_end(valgroup_hash)

            while len(self._cache) > self._cache_size:
                _ = self._cache.popitem(last=False)

        return data
