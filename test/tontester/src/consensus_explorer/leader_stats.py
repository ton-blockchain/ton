import argparse
import enum
import logging
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast, final

from .models import ConsensusData, GroupData, GroupInfo, SlotData
from .parser import GroupParser
from .validator_set_info import ValidatorSetInfoProvider

logger = logging.getLogger(__name__)


class SlotStatus(enum.Enum):
    FINALIZED = "finalized"
    EMPTY = "empty"
    SKIPPED = "skipped"


@dataclass
class ValidatorLeaderStats:
    validator_idx: int
    adnl: str = ""
    pub_key_hash: str = ""
    name: str = ""
    total_leader_slots: int = 0
    finalized: int = 0
    empty: int = 0
    skipped: int = 0
    unknown: int = 0


@dataclass
class GroupLeaderStats:
    valgroup_id: str
    group_start_est: float
    total_validators: int
    slots_per_leader_window: int
    observed_slot_range: tuple[int, int]  # (min_slot, max_slot) inclusive
    validators: list[ValidatorLeaderStats] = field(default_factory=list)


@final
class LeaderStatsAnalyzer:
    def __init__(
        self,
        parser: GroupParser,
        vset_provider: ValidatorSetInfoProvider | None = None,
        verbose: bool = False,
    ) -> None:
        self._parser = parser
        self._vset_provider = vset_provider
        self._verbose = verbose
        # valgroup_name -> (revision it was computed from, stats). Tiny next to
        # the slots/events behind them, so every analyzed group stays cached.
        self._stats_cache: dict[str, tuple[int, GroupLeaderStats | None]] = {}

    def list_groups(
        self,
        time_from: float | None = None,
        time_until: float | None = None,
    ) -> list[GroupData]:
        groups = self._parser.list_groups()
        result: list[GroupData] = []
        for g in groups:
            if time_from is not None and g.group_start_est < time_from:
                continue
            if time_until is not None and g.group_start_est > time_until:
                continue
            result.append(g)
        return result

    def analyze_group(self, valgroup_name: str) -> GroupLeaderStats | None:
        revision = self._parser.group_revision(valgroup_name)
        cached = self._cached_stats(valgroup_name, revision)
        if cached is not None:
            return cached[0]
        stats = self._analyze_data(valgroup_name, self._parser.parse_group(valgroup_name))
        self._store_stats(valgroup_name, revision, stats)
        return stats

    def _cached_stats(
        self, valgroup_name: str, revision: int | None
    ) -> tuple[GroupLeaderStats | None] | None:
        """Returns a 1-tuple holding the cached value, or None when not cached."""
        if revision is None:
            return None
        entry = self._stats_cache.get(valgroup_name)
        if entry is None or entry[0] != revision:
            return None
        return (entry[1],)

    def _store_stats(
        self, valgroup_name: str, revision: int | None, stats: GroupLeaderStats | None
    ) -> None:
        if revision is not None:
            self._stats_cache[valgroup_name] = (revision, stats)

    def _analyze_data(self, valgroup_name: str, data: ConsensusData) -> GroupLeaderStats | None:
        if not data.groups:
            return None

        group_info = next(
            (g for g in data.groups if g.valgroup_name == valgroup_name),
            None,
        )
        if group_info is None:
            return None
        group_slots = {s.slot: s for s in data.slots if s.valgroup_id == valgroup_name}
        group_events = [e for e in data.events if e.valgroup_id == valgroup_name]

        if not group_slots:
            return None

        total_validators, slots_per_leader_window = _infer_group_params(group_slots)
        if total_validators is None or slots_per_leader_window is None:
            return None

        min_slot = min(group_slots.keys())
        max_slot = max(group_slots.keys())

        # Gather slots with finalization certificates.
        directly_finalized: set[int] = set()
        for e in group_events:
            if e.label == "finalize_reached":
                directly_finalized.add(e.slot)

        # Walk parent chains from each finalized slot to reconstruct the
        # finalized history. Visited slots are finalized (non-empty), slots
        # skipped over between parent links are empty, everything else is
        # unknown.
        finalized_history = _walk_finalized_chains(directly_finalized, group_slots)

        if self._verbose:
            print(
                f"  Group {valgroup_name}: directly finalized slots = {sorted(directly_finalized)}"
            )
            for s in range(min_slot, max_slot + 1):
                status = finalized_history.get(s)
                if status is not None:
                    sd = group_slots.get(s)
                    parent_info = ""
                    if sd is not None:
                        parent_info = f" parent={sd.parent_block} block={sd.block_id_ext}"
                    print(f"    slot {s}: {status.value}{parent_info}")

        # Per-validator stats
        stats_by_validator: dict[int, ValidatorLeaderStats] = {}
        for v in range(total_validators):
            stats_by_validator[v] = ValidatorLeaderStats(validator_idx=v)

        for s in range(min_slot, max_slot + 1):
            leader = s // slots_per_leader_window % total_validators
            vs = stats_by_validator[leader]
            vs.total_leader_slots += 1
            status = finalized_history.get(s)
            if status is None:
                vs.unknown += 1
            elif status == SlotStatus.FINALIZED:
                vs.finalized += 1
            elif status == SlotStatus.EMPTY:
                vs.empty += 1
            elif status == SlotStatus.SKIPPED:
                vs.skipped += 1

        # Enrich with ADNL info if available
        if self._vset_provider and isinstance(group_info, GroupInfo):
            try:
                vset_text = self._vset_provider.get_validator_set_text(valgroup_name, data.slots)
                adnl_map = _parse_vset_text(vset_text)
                if not adnl_map:
                    logger.warning(
                        "No ADNLs resolved for %s: %s", valgroup_name, vset_text.strip()[:200]
                    )
                for idx, (adnl, pub_key_hash, name) in adnl_map.items():
                    if idx in stats_by_validator:
                        stats_by_validator[idx].adnl = adnl
                        stats_by_validator[idx].pub_key_hash = pub_key_hash
                        stats_by_validator[idx].name = name
            except Exception:
                logger.exception("Validator set lookup failed for %s", valgroup_name)

        return GroupLeaderStats(
            valgroup_id=valgroup_name,
            group_start_est=group_info.group_start_est,
            total_validators=total_validators,
            slots_per_leader_window=slots_per_leader_window,
            observed_slot_range=(min_slot, max_slot),
            validators=sorted(stats_by_validator.values(), key=lambda v: v.validator_idx),
        )

    def analyze_time_range(
        self,
        time_from: float | None = None,
        time_until: float | None = None,
    ) -> list[GroupLeaderStats]:
        groups = self.list_groups(time_from, time_until)

        # Read revisions before parsing: a change landing mid-parse then bumps
        # past what we store, so the next call recomputes.
        revisions = {g.valgroup_name: self._parser.group_revision(g.valgroup_name) for g in groups}

        results: dict[str, GroupLeaderStats | None] = {}
        to_parse: list[str] = []
        for g in groups:
            cached = self._cached_stats(g.valgroup_name, revisions[g.valgroup_name])
            if cached is not None:
                results[g.valgroup_name] = cached[0]
            else:
                to_parse.append(g.valgroup_name)

        if to_parse:
            parsed = self._parser.parse_groups(to_parse)
            for name in to_parse:
                data = parsed.get(name)
                stats = self._analyze_data(name, data) if data is not None else None
                self._store_stats(name, revisions[name], stats)
                results[name] = stats

        return [stats for g in groups if (stats := results.get(g.valgroup_name)) is not None]

    @staticmethod
    def aggregate_by_validator(
        group_stats_list: list[GroupLeaderStats],
    ) -> dict[str, ValidatorLeaderStats]:
        """Aggregate stats across groups, keyed by ADNL.

        Validators without ADNL info are skipped since their idx is
        group-local and cannot be matched across groups.
        """
        agg: dict[str, ValidatorLeaderStats] = {}
        for gs in group_stats_list:
            for vs in gs.validators:
                if not vs.adnl:
                    continue
                if vs.adnl not in agg:
                    agg[vs.adnl] = ValidatorLeaderStats(
                        validator_idx=-1,
                        adnl=vs.adnl,
                        pub_key_hash=vs.pub_key_hash,
                        name=vs.name,
                    )
                a = agg[vs.adnl]
                a.total_leader_slots += vs.total_leader_slots
                a.finalized += vs.finalized
                a.empty += vs.empty
                a.skipped += vs.skipped
                a.unknown += vs.unknown
                if vs.name and not a.name:
                    a.name = vs.name
                if vs.pub_key_hash and not a.pub_key_hash:
                    a.pub_key_hash = vs.pub_key_hash
        return agg


def _parse_parent_slot(parent_block: str | None) -> int | None:
    """Extract slot number from a parent_block string like '{42, base64hash}'.

    Returns None for genesis or missing parent data.
    """
    if parent_block is None or parent_block == "genesis":
        return None
    m = re.match(r"\{(\d+),", parent_block)
    if m is None:
        return None
    return int(m.group(1))


def _walk_finalized_chains(
    directly_finalized: set[int],
    group_slots: dict[int, SlotData],
) -> dict[int, SlotStatus]:
    """Reconstruct the finalized history by walking parent chains.

    For each slot with a finalization certificate, walk parent_block links
    until we run out of candidate data.

    - Visited slots with a non-empty block -> FINALIZED
    - Visited slots with an empty block -> EMPTY
    - Slots skipped over between parent links -> SKIPPED
    - Everything else is not in the returned dict (unknown).
    """
    history: dict[int, SlotStatus] = {}

    for start in directly_finalized:
        current = start
        while True:
            if current in history:
                break

            sd = group_slots.get(current)
            if sd is not None and (sd.block_id_ext == "empty" or sd.is_empty):
                history[current] = SlotStatus.EMPTY
            else:
                history[current] = SlotStatus.FINALIZED

            if sd is None:
                break

            parent_slot = _parse_parent_slot(sd.parent_block)

            if sd.parent_block == "genesis":
                for s in range(current):
                    _ = history.setdefault(s, SlotStatus.SKIPPED)
                break

            if parent_slot is None:
                break

            for s in range(parent_slot + 1, current):
                _ = history.setdefault(s, SlotStatus.SKIPPED)

            current = parent_slot

    return history


def _infer_group_params(
    group_slots: dict[int, SlotData],
) -> tuple[int | None, int | None]:
    """Infer (total_validators, slots_per_leader_window) from collator assignments."""
    pairs = [
        (s, sd.collator) for s, sd in sorted(group_slots.items()) if isinstance(sd.collator, int)
    ]
    if len(pairs) < 2:
        return None, None

    # collator = slot // leader_window % num_validators
    # Find leader_window by finding where collator first changes.
    first_collator = pairs[0][1]
    leader_window: int | None = None
    for slot, collator in pairs:
        if collator != first_collator:
            leader_window = slot
            break

    if leader_window is None:
        return None, None

    collators_seen: set[int] = set()
    for _, collator in pairs:
        collators_seen.add(collator)

    num_validators = len(collators_seen)
    if num_validators < 1:
        return None, None

    # Verify the formula
    for slot, collator in pairs[: min(len(pairs), leader_window * num_validators * 2)]:
        if slot // leader_window % num_validators != collator:
            break
    else:
        return num_validators, leader_window

    return num_validators, leader_window


def _parse_vset_text(text: str) -> dict[int, tuple[str, str, str]]:
    """Parse validator set text output to get idx -> (adnl, pub_key_hash, name)."""
    result: dict[int, tuple[str, str, str]] = {}
    for line in text.splitlines():
        parts = line.split("|")
        if len(parts) >= 4:
            try:
                idx = int(parts[0].strip())
                adnl = parts[1].strip()
                pub_key_hash = parts[2].strip()
                name = parts[3].strip()
                if len(adnl) == 64:
                    result[idx] = (adnl, pub_key_hash, name)
            except ValueError, IndexError:
                continue
    return result


def _serialize_validator(v: ValidatorLeaderStats) -> dict[str, str | int]:
    return {
        "validator_idx": v.validator_idx,
        "adnl": v.adnl,
        "pub_key_hash": v.pub_key_hash,
        "name": v.name,
        "total_leader_slots": v.total_leader_slots,
        "finalized": v.finalized,
        "empty": v.empty,
        "skipped": v.skipped,
        "unknown": v.unknown,
    }


def _serialize_group(
    gs: GroupLeaderStats,
) -> dict[str, str | float | int | list[int] | list[dict[str, str | int]]]:
    return {
        "valgroup_id": gs.valgroup_id,
        "group_start_est": gs.group_start_est,
        "total_validators": gs.total_validators,
        "slots_per_leader_window": gs.slots_per_leader_window,
        "observed_slot_range": list(gs.observed_slot_range),
        "validators": [_serialize_validator(v) for v in gs.validators],
    }


def _serialize_response(
    all_stats: list[GroupLeaderStats], analyzer: LeaderStatsAnalyzer
) -> dict[
    str,
    list[dict[str, str | float | int | list[int] | list[dict[str, str | int]]]]
    | dict[str, dict[str, str | int]],
]:
    agg = analyzer.aggregate_by_validator(all_stats)
    return {
        "groups": [_serialize_group(gs) for gs in all_stats],
        "aggregate": {key: _serialize_validator(v) for key, v in agg.items()},
    }


def format_group_stats(stats: GroupLeaderStats) -> str:
    lines = [
        f"Group: {stats.valgroup_id}",
        f"  Observed slots: {stats.observed_slot_range[0]} - {stats.observed_slot_range[1]}",
        f"  Total validators: {stats.total_validators}, leader window: {stats.slots_per_leader_window}",
        "",
        f"  {'Idx':>4} {'ADNL':>16} {'PubKeyHash':>16} {'Name':>12} {'Leader':>7} {'Finalized':>10} {'Empty':>6} {'Skipped':>8} {'Unknown':>8} {'Prod%':>6}",
        f"  {'-' * 4} {'-' * 16} {'-' * 16} {'-' * 12} {'-' * 7} {'-' * 10} {'-' * 6} {'-' * 8} {'-' * 8} {'-' * 6}",
    ]
    for v in stats.validators:
        known = v.finalized + v.empty + v.skipped
        pct = f"{(v.finalized + v.empty) / known * 100:.1f}" if known > 0 else "n/a"
        adnl_short = v.adnl[:16] if v.adnl else ""
        pkh_short = v.pub_key_hash[:16] if v.pub_key_hash else ""
        name = v.name[:12] if v.name else ""
        lines.append(
            f"  {v.validator_idx:>4} {adnl_short:>16} {pkh_short:>16} {name:>12} {v.total_leader_slots:>7} {v.finalized:>10} {v.empty:>6} {v.skipped:>8} {v.unknown:>8} {pct:>6}"
        )
    return "\n".join(lines)


_HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>Leader Stats</title>
    <style>
        body { font-family: monospace; margin: 20px; background: #fafafa; }
        h1 { margin-bottom: 4px; }
        .controls { margin: 16px 0; padding: 12px; background: #fff; border: 1px solid #ddd;
                     border-radius: 4px; }
        .controls label { margin-right: 16px; }
        .controls input[type=text] { width: 200px; padding: 4px; }
        .controls button { padding: 6px 16px; cursor: pointer; }
        table { border-collapse: collapse; margin: 12px 0; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 6px 10px; text-align: right; }
        th { background: #f0f0f0; }
        td:first-child, th:first-child { text-align: left; }
        .good { color: #2a7; }
        .warn { color: #c80; }
        .bad { color: #c33; }
        #results { margin-top: 16px; }
        .tab-bar { display: flex; gap: 0; margin-bottom: 0; }
        .tab-bar button { padding: 8px 20px; border: 1px solid #ddd; border-bottom: none;
                          background: #f0f0f0; cursor: pointer; border-radius: 4px 4px 0 0; }
        .tab-bar button.active { background: #fff; font-weight: bold; }
        .tab-content { border: 1px solid #ddd; padding: 16px; background: #fff; }
    </style>
</head>
<body>
    <h1>Consensus Leader Stats</h1>
    <p>Per-validator leader slot finalization statistics.</p>

    <div class="controls">
        <label>Time from: <input type="text" id="time-from" placeholder="unix timestamp"></label>
        <label>Time until: <input type="text" id="time-until" placeholder="unix timestamp"></label>
        <label>Group: <input type="text" id="group-filter"
               placeholder="valgroup id (optional)"></label>
        <button onclick="loadStats()">Load</button>
        <button onclick="loadLastMinutes(10)">Last 10 min</button>
        <button onclick="loadLastMinutes(20)">Last 20 min</button>
    </div>

    <div class="tab-bar">
        <button id="tab-groups" class="active" onclick="showTab('groups')">Per Group</button>
        <button id="tab-aggregate" onclick="showTab('aggregate')">Aggregate</button>
    </div>

    <div id="results" class="tab-content">
        <p>Click "Load" to fetch statistics.</p>
    </div>

    <script>
        const WEB_ROOT = '{{ web_root }}';
        let currentTab = 'groups';
        let lastData = null;

        function showTab(tab) {
            currentTab = tab;
            document.getElementById('tab-groups').className = tab === 'groups' ? 'active' : '';
            document.getElementById('tab-aggregate').className =
                tab === 'aggregate' ? 'active' : '';
            if (lastData) renderData(lastData);
        }

        function loadLastMinutes(minutes) {
            const now = Math.floor(Date.now() / 1000);
            document.getElementById('time-from').value = now - minutes * 60;
            document.getElementById('time-until').value = '';
            loadStats();
        }

        async function loadStats() {
            const timeFrom = document.getElementById('time-from').value;
            const timeUntil = document.getElementById('time-until').value;
            const groupFilter = document.getElementById('group-filter').value;
            if (!timeFrom && !timeUntil && !groupFilter) {
                return;
            }
            const results = document.getElementById('results');
            results.innerHTML = '<p>Loading...</p>';

            let url;
            if (groupFilter) {
                url = WEB_ROOT + 'api/group/' + encodeURIComponent(groupFilter);
            } else {
                const params = new URLSearchParams();
                if (timeFrom) params.set('time_from', timeFrom);
                if (timeUntil) params.set('time_until', timeUntil);
                url = WEB_ROOT + 'api/stats?' + params.toString();
            }

            try {
                const resp = await fetch(url);
                if (!resp.ok) {
                    results.innerHTML = '<p>Error: ' + resp.statusText + '</p>';
                    return;
                }
                lastData = await resp.json();
                renderData(lastData);
            } catch(e) {
                results.innerHTML = '<p>Error: ' + e.message + '</p>';
            }
        }

        function pctClass(pct) {
            if (pct >= 90) return 'good';
            if (pct >= 70) return 'warn';
            return 'bad';
        }

        function renderData(data) {
            const results = document.getElementById('results');
            if (currentTab === 'groups') {
                renderGroups(data, results);
            } else {
                renderAggregate(data, results);
            }
        }

        function renderGroups(data, el) {
            if (!data.groups || data.groups.length === 0) {
                el.innerHTML = '<p>No groups found.</p>';
                return;
            }
            let html = '';
            for (const g of data.groups) {
                html += '<h3>' + g.valgroup_id + '</h3>';
                html += '<p>Slots ' + g.observed_slot_range[0] + ' - '
                        + g.observed_slot_range[1];
                html += ', validators: ' + g.total_validators;
                html += ', leader window: ' + g.slots_per_leader_window + '</p>';
                html += renderTable(g.validators, true);
            }
            el.innerHTML = html;
        }

        function renderAggregate(data, el) {
            if (!data.aggregate || Object.keys(data.aggregate).length === 0) {
                el.innerHTML = '<p>No aggregate data. Aggregation requires ADNL resolution (--block-explorer-url and --show-validator-set-bin).</p>';
                return;
            }
            const vals = Object.values(data.aggregate);
            vals.sort((a, b) => {
                const ka = a.finalized + a.empty + a.skipped;
                const pa = ka > 0 ? (a.finalized + a.empty) / ka : 0;
                const kb = b.finalized + b.empty + b.skipped;
                const pb = kb > 0 ? (b.finalized + b.empty) / kb : 0;
                return pb - pa;
            });
            el.innerHTML = '<h3>Aggregate across all groups</h3>' + renderTable(vals, false);
        }

        function renderTable(validators, showIdx) {
            let html = '<table><tr>';
            if (showIdx) html += '<th>Idx</th>';
            html += '<th>ADNL</th><th>PubKeyHash</th><th>Name</th>';
            html += '<th>Leader slots</th><th>Finalized</th><th>Empty</th>';
            html += '<th>Skipped</th><th>Unknown</th>';
            html += '<th title="(finalized + empty) / (finalized + empty + skipped)">Produced%</th></tr>';
            for (const v of validators) {
                const known = v.finalized + v.empty + v.skipped;
                const pct = known > 0 ? ((v.finalized + v.empty) / known * 100) : -1;
                const pctStr = pct >= 0 ? pct.toFixed(1) + '%' : 'n/a';
                const cls = pct >= 0 ? pctClass(pct) : '';
                const adnl = v.adnl ? v.adnl.substring(0, 16) + '...' : '';
                const pkh = v.pub_key_hash || '';
                html += '<tr>';
                if (showIdx) html += '<td>' + v.validator_idx + '</td>';
                html += '<td>' + adnl + '</td>';
                html += '<td>' + pkh + '</td>';
                html += '<td>' + (v.name || '') + '</td>';
                html += '<td>' + v.total_leader_slots + '</td>';
                html += '<td>' + v.finalized + '</td>';
                html += '<td>' + v.empty + '</td>';
                html += '<td>' + v.skipped + '</td>';
                html += '<td>' + v.unknown + '</td>';
                html += '<td class="' + cls + '">' + pctStr + '</td>';
                html += '</tr>';
            }
            html += '</table>';
            return html;
        }
    </script>
</body>
</html>
"""


def _build_web_app(analyzer: LeaderStatsAnalyzer, host: str, port: int, web_root: str) -> None:
    from flask import Flask, Response, jsonify, render_template_string, request

    # Normalize web_root: ensure it starts and ends with /
    web_root = "/" + web_root.strip("/")
    if not web_root.endswith("/"):
        web_root += "/"

    app = Flask(__name__)

    def index() -> str:
        return render_template_string(_HTML_TEMPLATE, web_root=web_root)

    def api_groups() -> Response:
        time_from = request.args.get("time_from", type=float)
        time_until = request.args.get("time_until", type=float)
        groups = analyzer.list_groups(time_from, time_until)
        return jsonify(
            [
                {
                    "valgroup_name": g.valgroup_name,
                    "group_start_est": g.group_start_est,
                }
                for g in groups
            ]
        )

    def api_group(valgroup_name: str) -> Response | tuple[Response, int]:
        stats = analyzer.analyze_group(valgroup_name)
        if stats is None:
            return jsonify({"error": "group not found"}), 404
        return jsonify(_serialize_response([stats], analyzer))

    def api_stats() -> Response:
        time_from = request.args.get("time_from", type=float)
        time_until = request.args.get("time_until", type=float)
        all_stats = analyzer.analyze_time_range(time_from, time_until)
        return jsonify(_serialize_response(all_stats, analyzer))

    app.add_url_rule(web_root, endpoint="index", view_func=index)
    app.add_url_rule(f"{web_root}api/groups", endpoint="api_groups", view_func=api_groups)
    app.add_url_rule(
        f"{web_root}api/group/<path:valgroup_name>", endpoint="api_group", view_func=api_group
    )
    app.add_url_rule(f"{web_root}api/stats", endpoint="api_stats", view_func=api_stats)

    print(f"Leader stats server running at http://{host}:{port}{web_root}")
    app.run(host=host, port=port, debug=False)


def _main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s: %(message)s",
        force=True,
    )

    ap = argparse.ArgumentParser(description="Consensus leader finalization stats")
    source = ap.add_mutually_exclusive_group(required=True)
    _ = source.add_argument("--logs", nargs="+", help="Paths to log files or directory")
    _ = source.add_argument(
        "--stats-dir", help="Directory with gzipped session stats files (watched for changes)"
    )
    _ = ap.add_argument("--db", help="Path to SQLite database (default: <stats-dir>/index.db)")
    _ = ap.add_argument(
        "--hostname-regex",
        default=r"^(.*)$",
        help="Regex with capture group to extract hostname from filename",
    )
    _ = ap.add_argument("--host", default="127.0.0.1", help="Host to bind to (default: 127.0.0.1)")
    _ = ap.add_argument("--port", type=int, default=8051, help="Port to bind to (default: 8051)")
    _ = ap.add_argument(
        "--block-explorer-url",
        default=os.getenv("CONSENSUS_EXPLORER_URL", ""),
        help="Block explorer base url (for validator set lookup)",
    )
    _ = ap.add_argument(
        "--show-validator-set-bin",
        default=os.getenv("CONSENSUS_EXPLORER_SHOW_VALIDATOR_SET_BIN", ""),
        help="Path to show-validator-set binary",
    )
    _ = ap.add_argument(
        "--validator-names-json",
        default=os.getenv("CONSENSUS_EXPLORER_VALIDATOR_NAMES_JSON", ""),
        help='Path to json map {"adnl": "name"} for validator names',
    )
    _ = ap.add_argument(
        "--cache-dir",
        default=os.getenv("CONSENSUS_EXPLORER_CACHE_DIR", ""),
        help="Directory to cache downloaded key blocks",
    )
    _ = ap.add_argument("--web-root", default="/", help="Web root prefix (default: /)")
    _ = ap.add_argument(
        "--sudo-helper",
        default="",
        help="Path to helper script for reading files via sudo on permission denied",
    )
    _ = ap.add_argument(
        "--text", action="store_true", help="Print text output instead of starting web server"
    )
    _ = ap.add_argument("--verbose", action="store_true", help="Print per-slot debug info")
    _ = ap.add_argument(
        "--time-from", type=float, help="Filter groups starting after this unix timestamp"
    )
    _ = ap.add_argument(
        "--time-until", type=float, help="Filter groups starting before this unix timestamp"
    )

    raw = ap.parse_args()

    logs = cast(list[str] | None, raw.logs)
    stats_dir_str = cast(str | None, raw.stats_dir)
    db_str = cast(str | None, raw.db)
    hostname_regex = cast(str, raw.hostname_regex)
    host = cast(str, raw.host)
    port = cast(int, raw.port)
    block_explorer_url = cast(str, raw.block_explorer_url)
    show_validator_set_bin = cast(str, raw.show_validator_set_bin)
    validator_names_json = cast(str, raw.validator_names_json)
    cache_dir = cast(str, raw.cache_dir)
    web_root = cast(str, raw.web_root)
    sudo_helper = cast(str, raw.sudo_helper)
    text = cast(bool, raw.text)
    verbose = cast(bool, raw.verbose)
    time_from = cast(float | None, raw.time_from)
    time_until = cast(float | None, raw.time_until)

    vset_provider = None
    if block_explorer_url and show_validator_set_bin:
        if not Path(show_validator_set_bin).exists():
            ap.error(f"show-validator-set binary not found at {show_validator_set_bin}")
        vset_provider = ValidatorSetInfoProvider(
            block_explorer_url,
            show_validator_set_bin,
            validator_names_json,
            cache_dir=cache_dir or None,
        )

    if stats_dir_str:
        from .cached_parser import CachedGroupParser
        from .file_index import FileIndex

        stats_dir = Path(stats_dir_str)
        db_path = Path(db_str) if db_str else stats_dir / "index.db"

        file_index = FileIndex(stats_dir, db_path, sudo_helper=sudo_helper or None)
        cached_parser = CachedGroupParser(
            file_index,
            hostname_regex,
            sudo_helper=sudo_helper or None,
            # Only consensus events are read here; block stats and the MC files
            # pulled in for crosslink markers are pure overhead.
            need_block_stats=False,
            need_crosslinks=False,
        )
        file_index.install_callback(cached_parser)

        with file_index:
            analyzer = LeaderStatsAnalyzer(cached_parser, vset_provider, verbose)
            if text:
                _print_text(analyzer, time_from, time_until)
            else:
                _build_web_app(analyzer, host, port, web_root)
    else:
        from .parser.parser_session_stats import ParserSessionStats

        assert logs is not None
        log_paths: list[Path] = []
        for log in logs:
            p = Path(log)
            if p.is_dir():
                log_paths.extend(p.iterdir())
            else:
                log_paths.append(p)

        group_parser = ParserSessionStats(log_paths, hostname_regex)
        analyzer = LeaderStatsAnalyzer(group_parser, vset_provider, verbose)
        if text:
            _print_text(analyzer, time_from, time_until)
        else:
            _build_web_app(analyzer, host, port, web_root)


def _print_text(
    analyzer: LeaderStatsAnalyzer,
    time_from: float | None,
    time_until: float | None,
) -> None:
    all_stats = analyzer.analyze_time_range(time_from, time_until)
    if not all_stats:
        print("No groups found.")
        return
    for gs in all_stats:
        print(format_group_stats(gs))
        print()

    if len(all_stats) > 1:
        agg = analyzer.aggregate_by_validator(all_stats)
        print("=== Aggregate across all groups ===")
        print(
            f"  {'ADNL':>20} {'PubKeyHash':>16} {'Name':>12} {'Leader':>7} {'Finalized':>10} {'Empty':>6} {'Skipped':>8} {'Unknown':>8} {'Prod%':>6}"
        )
        for key, v in sorted(agg.items(), key=lambda kv: -(kv[1].finalized)):
            known = v.finalized + v.empty + v.skipped
            pct = f"{(v.finalized + v.empty) / known * 100:.1f}" if known > 0 else "n/a"
            display_key = (v.name or v.adnl[:20]) if v.adnl else key[:20]
            pkh_short = v.pub_key_hash[:16] if v.pub_key_hash else ""
            print(
                f"  {display_key:>20} {pkh_short:>16} {v.name[:12]:>12} {v.total_leader_slots:>7} {v.finalized:>10} {v.empty:>6} {v.skipped:>8} {v.unknown:>8} {pct:>6}"
            )


if __name__ == "__main__":
    _main()
