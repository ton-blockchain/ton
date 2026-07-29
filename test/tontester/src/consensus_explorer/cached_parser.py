import base64
import logging
import threading
import time
from collections import OrderedDict
from collections.abc import Sequence
from pathlib import Path
from typing import final, override

from .file_index import FileIndex, FileIndexCallback
from .models import ConsensusData, GroupData
from .parser import GroupParser
from .parser.parser_base import split_by_group
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
        *,
        need_block_stats: bool = True,
        need_crosslinks: bool = True,
    ):
        self._file_index = file_index
        self._hostname_regex = hostname_regex
        self._sudo_helper = sudo_helper
        self._cache_size = cache_size
        self._need_block_stats = need_block_stats
        self._need_crosslinks = need_crosslinks
        self._cache: OrderedDict[bytes, ConsensusData] = OrderedDict()
        self._dirty_groups: set[bytes] = set()
        self._revisions: dict[bytes, int] = {}
        self._name_to_hash: dict[str, bytes] = {}
        self._lock = threading.Lock()

    @override
    def on_files_changed(self, changed_groups: set[bytes]) -> None:
        with self._lock:
            self._dirty_groups |= changed_groups
            for group_hash in changed_groups:
                self._revisions[group_hash] = self._revisions.get(group_hash, 0) + 1
        logger.info("on_files_changed: %d groups invalidated", len(changed_groups))

    def _resolve_name(self, valgroup_name: str) -> bytes:
        cached = self._name_to_hash.get(valgroup_name)
        if cached is not None:
            return cached
        # Refresh the whole mapping at once, not once per unknown name.
        mapping = {
            info.valgroup_name: info.valgroup_hash for info in self._file_index.get_all_groups()
        }
        self._name_to_hash = mapping
        resolved = mapping.get(valgroup_name)
        if resolved is not None:
            return resolved
        return base64.b64decode(valgroup_name)

    @override
    def group_revision(self, valgroup_name: str) -> int | None:
        valgroup_hash = self._resolve_name(valgroup_name)
        with self._lock:
            return self._revisions.get(valgroup_hash, 0)

    @override
    def list_groups(self) -> list[GroupData]:
        return self._file_index.get_all_groups()

    def _collect_inputs(self, valgroup_hashes: Sequence[bytes]) -> tuple[list[Path], set[bytes]]:
        """Collect the log files to read and the group hashes to keep from them."""
        log_paths: list[Path] = []
        seen: set[Path] = set()
        target_hashes: set[bytes] = set(valgroup_hashes)

        for valgroup_hash in valgroup_hashes:
            for path in self._file_index.get_files_for_group(valgroup_hash):
                if path not in seen:
                    seen.add(path)
                    log_paths.append(path)

        if not self._need_crosslinks:
            return log_paths, target_hashes

        # Every crosslink file contributes its groups to the targets, including
        # files the group already reads -- their MC events must be parsed too.
        crosslink_paths: list[Path] = []
        for valgroup_hash in valgroup_hashes:
            for path in self._file_index.get_crosslink_files_for_group(valgroup_hash):
                crosslink_paths.append(path)
                if path not in seen:
                    seen.add(path)
                    log_paths.append(path)
        if crosslink_paths:
            target_hashes |= self._file_index.get_group_hashes_in_files(crosslink_paths)

        return log_paths, target_hashes

    def _parse_uncached(self, valgroup_hashes: Sequence[bytes], label: str) -> ConsensusData:
        t0 = time.monotonic()
        log_paths, target_hashes = self._collect_inputs(valgroup_hashes)
        logger.info(
            "parse %s: %d files to parse, %d target groups",
            label,
            len(log_paths),
            len(target_hashes),
        )

        parser = ParserSessionStats(
            log_paths,
            self._hostname_regex,
            with_cache=False,
            target_group_hashes=target_hashes,
            sudo_helper=self._sudo_helper,
            parse_block_stats=self._need_block_stats,
        )
        data = parser.parse()
        logger.info(
            "parse %s: parsed in %.2fs (%d slots, %d events)",
            label,
            time.monotonic() - t0,
            len(data.slots),
            len(data.events),
        )
        return data

    def _store(self, valgroup_hash: bytes, data: ConsensusData) -> None:
        self._cache[valgroup_hash] = data
        self._cache.move_to_end(valgroup_hash)
        while len(self._cache) > self._cache_size:
            _ = self._cache.popitem(last=False)

    @override
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        return self.parse_groups([valgroup_name])[valgroup_name]

    @override
    def parse_groups(self, valgroup_names: Sequence[str]) -> dict[str, ConsensusData]:
        hashes = {name: self._resolve_name(name) for name in valgroup_names}

        result: dict[str, ConsensusData] = {}
        missing: list[str] = []
        with self._lock:
            for name, valgroup_hash in hashes.items():
                dirty = valgroup_hash in self._dirty_groups
                entry = None if dirty else self._cache.get(valgroup_hash)
                if entry is not None:
                    self._cache.move_to_end(valgroup_hash)
                    result[name] = entry
                    continue
                # Clear before parsing, so a change landing mid-parse re-marks
                # the group instead of being swallowed.
                self._dirty_groups.discard(valgroup_hash)
                missing.append(name)

        logger.info(
            "parse_groups: %d requested, %d cached, %d to parse",
            len(valgroup_names),
            len(result),
            len(missing),
        )
        if not missing:
            return {name: result[name] for name in valgroup_names}

        # One pass over the union of their files: a file holds many groups, so
        # parsing group by group re-reads and re-deserializes the same bytes.
        data = self._parse_uncached([hashes[name] for name in missing], f"{len(missing)} groups")
        split = split_by_group(data, missing)

        with self._lock:
            for name in missing:
                self._store(hashes[name], split[name])
        result.update(split)

        return {name: result[name] for name in valgroup_names}
