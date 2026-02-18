import json
import math
import re
import subprocess
import tempfile
from collections.abc import Sequence
from html import unescape
from pathlib import Path
from typing import final, cast
from urllib.parse import parse_qs, urlencode, urljoin, urlparse

import requests

from .models import SlotData


@final
class ValidatorSetInfoProvider:
    _MASTERCHAIN_WORKCHAIN = -1
    _MASTERCHAIN_SHARD_HEX = "8000000000000000"
    _VALGROUP_RE = re.compile(r"^(?P<workchain>-?\d+),(?P<shard>[0-9a-fA-F]+)\.(?P<cc_seqno>\d+)$")
    _ROW_RE = re.compile(r"^\s*(\d+)\s+\S+\s+([0-9a-fA-F]{64})\s+\d+\s*$")

    def __init__(
        self,
        explorer_url: str | None = None,
        show_validator_set_bin: str | Path | None = None,
        validator_names_json: str | Path | None = None,
    ):
        self._explorer_url: str | None = explorer_url.rstrip("/") if explorer_url else ""
        self._show_validator_set_bin: Path | None = self._resolve_show_validator_set_bin(
            show_validator_set_bin
        )
        self._validator_names: dict[str, str] = self._load_validator_names(validator_names_json)

    @staticmethod
    def _resolve_show_validator_set_bin(path: str | Path | None) -> Path | None:
        if path:
            return Path(path)
        default_in_repo = Path(__file__).resolve().parents[4] / "build/utils/show-validator-set"
        if default_in_repo.exists():
            return default_in_repo
        default_in_cwd = Path("build/utils/show-validator-set")
        if default_in_cwd.exists():
            return default_in_cwd
        return None

    @staticmethod
    def _load_validator_names(path: str | Path | None) -> dict[str, str]:
        if not path:
            return {}

        names_path = Path(path)
        try:
            raw_json = names_path.read_text(encoding="utf-8")
            parsed = cast(dict[str, str], json.loads(raw_json))
        except (OSError, json.JSONDecodeError):
            return {}

        return parsed

    @classmethod
    def _parse_valgroup_id(cls, valgroup_id: str) -> tuple[int, str, int] | None:
        match = cls._VALGROUP_RE.fullmatch(valgroup_id)
        if match is None:
            return None

        return (
            int(match.group("workchain")),
            match.group("shard").lower(),
            int(match.group("cc_seqno")),
        )

    @staticmethod
    def _group_start_utime(valgroup_id: str, slots: Sequence[SlotData]) -> int | None:
        start_ms = min(
            (
                slot.slot_start_est_ms
                for slot in slots
                if slot.valgroup_id == valgroup_id and math.isfinite(slot.slot_start_est_ms)
            ),
            default=None,
        )
        if start_ms is None:
            return None
        return max(0, int(start_ms // 1000))

    @staticmethod
    def _fetch_text(url: str, timeout: int) -> str:
        resp = requests.get(url, headers={"User-Agent": "consensus-explorer/1.0"}, timeout=timeout)
        resp.raise_for_status()
        return resp.content.decode("utf-8", errors="ignore")

    @staticmethod
    def _fetch_bytes(url: str, timeout: int) -> bytes:
        resp = requests.get(url, headers={"User-Agent": "consensus-explorer/1.0"}, timeout=timeout)
        resp.raise_for_status()
        return resp.content

    @staticmethod
    def _extract_prev_key_block_href(page: str) -> str | None:
        match = re.search(
            r'<tr>\s*<th>\s*prev_key_block_seqno\s*</th>\s*<td>\s*<a href="([^"]+)"',
            page,
            flags=re.IGNORECASE | re.DOTALL,
        )
        if match is None:
            return None
        return unescape(match.group(1))

    @staticmethod
    def _extract_table_value(page: str, key: str) -> str | None:
        match = re.search(
            rf"<tr>\s*<th>\s*{re.escape(key)}\s*</th>\s*<td>\s*([^<\s]+)\s*</td>",
            page,
            flags=re.IGNORECASE | re.DOTALL,
        )
        if match is None:
            return None
        return match.group(1).strip()

    @staticmethod
    def _extract_seqno_from_href(href: str) -> int | None:
        parsed = urlparse(href)
        params = parse_qs(parsed.query)
        values = params.get("seqno")
        if values:
            try:
                return int(values[0])
            except (TypeError, ValueError):
                pass

        match = re.search(r"(?:^|[?&])seqno=(\d+)", href)
        if match:
            return int(match.group(1))
        return None

    @staticmethod
    def _extract_block_seqno(page: str) -> int | None:
        match = re.search(
            r"<tr>\s*<th>\s*block\s*</th>\s*<td>\s*\([^,]+,[^,]+,(\d+)\)\s*</td>",
            page,
            flags=re.IGNORECASE | re.DOTALL,
        )
        if match is None:
            return None
        return int(match.group(1))

    def _run_show_validator_set(
        self,
        block_path: Path,
        group_workchain: int,
        group_shard_hex: str,
        cc_seqno: int,
    ) -> str:
        command = [
            str(self._show_validator_set_bin),
            "-f",
            str(block_path),
            "-w",
            str(group_workchain),
            "-s",
            group_shard_hex,
            "-c",
            str(cc_seqno),
        ]
        try:
            result = subprocess.run(command, capture_output=True, text=True, check=False)
        except OSError as exc:
            raise RuntimeError(f"failed to run {self._show_validator_set_bin}: {exc}") from exc

        if result.returncode != 0:
            error_details = result.stderr.strip() or result.stdout.strip() or "unknown error"
            raise RuntimeError(
                f"{self._show_validator_set_bin} exited with code {result.returncode}: {error_details}"
            )

        output = result.stdout.strip()
        if not output:
            raise RuntimeError(f"{self._show_validator_set_bin} returned empty output")
        return output

    @classmethod
    def _parse_validator_rows(cls, output: str) -> list[tuple[int, str]]:
        rows: list[tuple[int, str]] = []
        for line in output.splitlines():
            match = cls._ROW_RE.match(line)
            if match is not None:
                rows.append((int(match.group(1)), match.group(2)))
                continue
        return rows

    def _build_table(self, output: str) -> str:
        rows = self._parse_validator_rows(output)
        if not rows:
            raise RuntimeError("failed to parse validator rows from show-validator-set output")

        table_lines = ["idx | adnl | name"]
        for idx, adnl in rows:
            table_lines.append(f"{idx} | {adnl} | {self._validator_names.get(adnl, '')}")
        return "\n".join(table_lines)

    def _fetch_validator_set_info(
        self,
        valgroup_id: str,
        group_start_utime: int,
        group_workchain: int,
        group_shard_hex: str,
        cc_seqno: int,
    ) -> str:
        search_query = urlencode(
            {
                "workchain": self._MASTERCHAIN_WORKCHAIN,
                "shard": self._MASTERCHAIN_SHARD_HEX,
                "seqno": "",
                "lt": "",
                "utime": group_start_utime,
                "roothash": "",
                "filehash": "",
            }
        )
        search_url = f"{self._explorer_url}/search?{search_query}"

        search_page = self._fetch_text(search_url, timeout=20)
        prev_key_block_href = self._extract_prev_key_block_href(search_page)
        if prev_key_block_href is None:
            raise RuntimeError("prev_key_block_seqno link was not found in explorer search page")

        key_block_url = urljoin(f"{self._explorer_url}/", prev_key_block_href)
        key_block_page = self._fetch_text(key_block_url, timeout=20)

        key_block_seqno = self._extract_seqno_from_href(prev_key_block_href)
        if key_block_seqno is None:
            key_block_seqno = self._extract_block_seqno(key_block_page)
        if key_block_seqno is None:
            raise RuntimeError("key block seqno was not found")

        root_hash = self._extract_table_value(key_block_page, "roothash")
        file_hash = self._extract_table_value(key_block_page, "filehash")
        if not root_hash or not file_hash:
            raise RuntimeError("key block roothash/filehash were not found")

        download_query = urlencode(
            {
                "workchain": self._MASTERCHAIN_WORKCHAIN,
                "shard": self._MASTERCHAIN_SHARD_HEX,
                "seqno": key_block_seqno,
                "roothash": root_hash,
                "filehash": file_hash,
            }
        )
        download_url = f"{self._explorer_url}/download?{download_query}"
        block_data = self._fetch_bytes(download_url, timeout=40)
        if not block_data:
            raise RuntimeError("downloaded key block is empty")

        with tempfile.TemporaryDirectory(prefix="validator_set_") as tmp_dir:
            key_block_path = Path(tmp_dir) / f"key_block_{key_block_seqno}.boc"
            _ = key_block_path.write_bytes(block_data)
            validator_set_output = self._run_show_validator_set(
                key_block_path,
                group_workchain=group_workchain,
                group_shard_hex=group_shard_hex,
                cc_seqno=cc_seqno,
            )
        validator_set_table = self._build_table(validator_set_output)

        return "\n".join(
            [
                f"valgroup = {valgroup_id}",
                f"group_start_utime = {group_start_utime}",
                f"key_block = (-1,{self._MASTERCHAIN_SHARD_HEX},{key_block_seqno})",
                f"roothash = {root_hash}",
                f"filehash = {file_hash}",
                "",
                validator_set_table,
            ]
        )

    def get_validator_set_text(self, valgroup_id: str, slots: Sequence[SlotData]) -> str:
        if not self._explorer_url:
            return "validator set info: explorer url is not configured"

        if self._show_validator_set_bin is None:
            return "validator set info: show-validator-set binary is not configured"

        parsed_group = self._parse_valgroup_id(valgroup_id)
        if parsed_group is None:
            return f"validator set info: invalid valgroup format ({valgroup_id})"

        group_start_utime = self._group_start_utime(valgroup_id, slots)
        if group_start_utime is None:
            return f"validator set info: group start timestamp was not found for {valgroup_id}"

        group_workchain, group_shard_hex, cc_seqno = parsed_group
        try:
            result = self._fetch_validator_set_info(
                valgroup_id=valgroup_id,
                group_start_utime=group_start_utime,
                group_workchain=group_workchain,
                group_shard_hex=group_shard_hex,
                cc_seqno=cc_seqno,
            )
        except requests.HTTPError as exc:
            return f"validator set info error for {valgroup_id}: explorer request failed ({exc})"
        except Exception as exc:
            return f"validator set info error for {valgroup_id}: {exc}"

        return result
