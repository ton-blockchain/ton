# These two harness tests drive the local-net runner by substituting doubles for its
# subprocess, HTTP and filesystem seams, so they deliberately touch private helpers and pass
# values the checker can only see as Any. Suppressed per file rather than with ~165 inline
# ignores; production modules keep the full rule set.
# pyright: reportAny=false, reportExplicitAny=false, reportPrivateUsage=false
# pyright: reportUnannotatedClassAttribute=false, reportUnknownArgumentType=false
# pyright: reportUnknownLambdaType=false, reportUnusedCallResult=false
# pyright: reportUnusedParameter=false
import asyncio
import base64
import http.client
import importlib.util
import json
import sys
import time
from pathlib import Path
from typing import Any, cast

import pytest
from tontester.network import FullNode

_SCRIPT = Path(__file__).resolve().parents[4] / "benchmark/run_local_net.py"
_SPEC = importlib.util.spec_from_file_location("ton_run_local_net", _SCRIPT)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load {_SCRIPT}")
_MODULE = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _MODULE
_SPEC.loader.exec_module(_MODULE)
run_local_net = cast(Any, _MODULE)


def test_dedicated_collators_are_registered(tmp_path: Path) -> None:
    collator_ids = [b"\x11" * 32, b"\x22" * 32]
    node = object.__new__(FullNode)
    node._directory = tmp_path

    node.set_collators_list(collator_ids, disable_self_collate=True)

    payload = json.loads((tmp_path / "collators-list.json").read_text())
    entries = [
        {
            "@type": "engine.validator.collatorsList.collator",
            "adnl_id": base64.b64encode(collator_id).decode(),
        }
        for collator_id in collator_ids
    ]
    assert payload == {
        "@type": "engine.validator.collatorsList",
        "collators": entries,
        "disable_self_collate": True,
        "register_collators": entries,
    }


def _fake_build(repo: Path) -> Path:
    build = repo / "build"
    executables = (
        build / "utils/generate-random-id",
        build / "validator-engine/validator-engine",
        build / "dht-server/dht-server",
        build / "crypto/create-state",
        build / "benchmark/bench-state-gen",
        build / "benchmark/bench-spam",
    )
    for executable in executables:
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("fake\n")
        executable.chmod(0o700)
    tonlib_name = "libtonlibjson.dylib" if sys.platform == "darwin" else "libtonlibjson.so"
    tonlib = build / "tonlib" / tonlib_name
    tonlib.parent.mkdir(parents=True)
    tonlib.write_text("fake\n")
    (repo / "benchmark/contracts").mkdir(parents=True)
    return build


def _fake_state(directory: Path, *, total_wallets: int) -> Path:
    directory.mkdir()
    (directory / "celldb").mkdir()
    manifest = directory / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "root_hash_hex": "11" * 32,
                "file_hash_hex": "22" * 32,
                "total_balance": "0",
                "gen_utime": 0,
                "num_v5": total_wallets,
            }
        )
    )
    return manifest


def test_one_validator_defaults_to_one_shard_validator(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    config = run_local_net.validate_args(
        run_local_net.build_parser().parse_args(
            [
                "--repo",
                str(repo),
                "--build",
                str(build),
                "--net-dir",
                str(tmp_path / "net"),
                "--validators",
                "1",
            ]
        )
    )

    assert config.validators == 1
    assert config.shard_validators == 1


def test_message_chunk_matches_bench_spam_target_and_exact_wallet_pool(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    state = _fake_state(tmp_path / "state", total_wallets=2_400)

    config = run_local_net.validate_args(
        run_local_net.build_parser().parse_args(
            [
                "--repo",
                str(repo),
                "--build",
                str(build),
                "--state",
                str(state),
                "--net-dir",
                str(tmp_path / "net"),
                "--validators",
                "1",
                "--rate",
                "20",
                "--chunk-s",
                "120",
            ]
        )
    )

    assert config.messages_per_chunk == 2_400


def test_sub_message_chunk_is_rejected(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    state = _fake_state(tmp_path / "state", total_wallets=1)
    args = run_local_net.build_parser().parse_args(
        [
            "--repo",
            str(repo),
            "--build",
            str(build),
            "--state",
            str(state),
            "--net-dir",
            str(tmp_path / "net"),
            "--rate",
            "0.1",
            "--chunk-s",
            "1",
        ]
    )

    with pytest.raises(ValueError, match="round to zero messages"):
        run_local_net.validate_args(args)


def test_exporter_ports_must_not_overlap_node_ports(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    net_dir = tmp_path / "net"
    args = run_local_net.build_parser().parse_args(
        [
            "--repo",
            str(repo),
            "--build",
            str(build),
            "--net-dir",
            str(net_dir),
            "--validators",
            "1",
            "--port-base",
            "2000",
            "--exporter-base",
            "2001",
        ]
    )

    with pytest.raises(ValueError, match="exporter port range overlaps"):
        run_local_net.validate_args(args)
    assert not net_dir.exists()


def test_manifest_hash_must_decode_to_exactly_32_bytes(tmp_path: Path) -> None:
    manifest = _fake_state(tmp_path / "state", total_wallets=1)
    value = json.loads(manifest.read_text())
    value["root_hash_hex"] = "11" * 31 + "  "  # 64 characters, but fromhex decodes 31 bytes.
    manifest.write_text(json.dumps(value))

    with pytest.raises(ValueError, match="exactly 32 bytes"):
        run_local_net._load_bench_state(manifest)


def test_bench_spam_outputs_stay_in_net_directory(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    state = _fake_state(tmp_path / "state", total_wallets=2_400)
    config = run_local_net.validate_args(
        run_local_net.build_parser().parse_args(
            [
                "--repo",
                str(repo),
                "--build",
                str(build),
                "--state",
                str(state),
                "--net-dir",
                str(tmp_path / "net"),
                "--rate",
                "20",
                "--chunk-s",
                "120",
            ]
        )
    )
    endpoint = run_local_net.LiteserverEndpoint("127.0.0.1", 2003, "public-key")

    command = run_local_net._bench_spam_command(config, endpoint, chunk=7, offset=14_400)

    output = Path(command[command.index("--out") + 1])
    blocks_csv = Path(command[command.index("--blocks-csv") + 1])
    assert output == config.net_dir / "spam/chunk-007.json"
    assert blocks_csv == config.net_dir / "spam/chunk-007-blocks.csv"
    assert output.is_absolute()
    assert blocks_csv.is_absolute()
    assert command[command.index("--wallet-offset") + 1] == "14400"


@pytest.mark.parametrize("rate", ["nan", "inf", "-1"])
def test_invalid_rate_is_rejected_before_net_dir_creation(tmp_path: Path, rate: str) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    build = _fake_build(repo)
    net_dir = tmp_path / "net"
    args = run_local_net.build_parser().parse_args(
        [
            "--repo",
            str(repo),
            "--build",
            str(build),
            "--net-dir",
            str(net_dir),
            "--rate",
            rate,
        ]
    )

    with pytest.raises(ValueError, match="finite non-negative"):
        run_local_net.validate_args(args)
    assert not net_dir.exists()


def test_net_dir_must_not_overlap_protected_paths(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    build = repo / "build"
    state = tmp_path / "state"
    build.mkdir(parents=True)
    state.mkdir()

    for unsafe in (tmp_path, repo, repo / "net", build, state, state / "net"):
        with pytest.raises(ValueError, match="overlap|ancestor"):
            run_local_net._validate_net_dir_location(
                unsafe,
                repo=repo.resolve(),
                build=build.resolve(),
                state_dir=state.resolve(),
            )


def test_owned_net_dir_is_locked_and_safely_recreated(tmp_path: Path) -> None:
    net_dir = (tmp_path / "net").resolve()

    with run_local_net.NetDirectoryLease.acquire(net_dir):
        assert (net_dir / run_local_net._MARKER_NAME).is_file()
        assert (net_dir / run_local_net._LOCK_NAME).is_file()
        with pytest.raises(ValueError, match="live PID"):
            run_local_net.NetDirectoryLease.acquire(net_dir)

    old_file = net_dir / "node0" / "old.log"
    old_file.parent.mkdir()
    old_file.write_text("old")
    with run_local_net.NetDirectoryLease.acquire(net_dir):
        assert not old_file.exists()
        assert (net_dir / run_local_net._MARKER_NAME).is_file()
    assert (net_dir / run_local_net._LOCK_NAME).is_file()


def test_existing_unmarked_net_dir_is_refused(tmp_path: Path) -> None:
    net_dir = tmp_path / "unmarked"
    net_dir.mkdir()

    with pytest.raises(ValueError, match="not owned"):
        run_local_net.NetDirectoryLease.acquire(net_dir.resolve())


class _FakeResponse:
    def __init__(self, payload: dict[str, Any]) -> None:
        self._payload = payload

    def read(self) -> bytes:
        return json.dumps(self._payload).encode()

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *_: object) -> None:
        return None


def _capture_annotations(
    monkeypatch: pytest.MonkeyPatch, *, error: Exception | None = None, delay: float = 0.0
) -> list[dict[str, Any]]:
    """Record every annotation request the client makes instead of sending it."""
    calls: list[dict[str, Any]] = []

    def fake_urlopen(request: Any, timeout: float) -> _FakeResponse:
        calls.append(
            {
                "url": request.full_url,
                "method": request.get_method(),
                "timeout": timeout,
                "body": json.loads(request.data),
                "auth": request.get_header("Authorization"),
                "content_type": request.get_header("Content-type"),
            }
        )
        time.sleep(delay)  # blocking, exactly like the real call it stands in for
        if error is not None:
            raise error
        return _FakeResponse({"id": 77})

    monkeypatch.setattr(run_local_net.urllib.request, "urlopen", fake_urlopen)
    return calls


@pytest.mark.asyncio
async def test_run_annotations_post_a_region_a_chunk_point_and_an_end(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls = _capture_annotations(monkeypatch)

    async with run_local_net.RunAnnotations.from_env(
        {"GRAFANA_URL": "http://grafana:3000/"}
    ) as runs:
        await runs.start("net: 8v+8sv rate=20 main@abc1234")
        await runs.point("chunk 1: rate=20")

    assert [call["method"] for call in calls] == ["POST", "POST", "PATCH"]
    assert [call["url"] for call in calls] == [
        "http://grafana:3000/api/annotations",
        "http://grafana:3000/api/annotations",
        "http://grafana:3000/api/annotations/77",
    ]
    assert all(call["timeout"] == run_local_net._ANNOTATION_TIMEOUT_S for call in calls)
    assert all(call["content_type"] == "application/json" for call in calls)
    expected_auth = "Basic " + base64.b64encode(b"admin:admin").decode()
    assert all(call["auth"] == expected_auth for call in calls)

    start, chunk, end = (call["body"] for call in calls)
    assert start["tags"] == ["ton-run"] and chunk["tags"] == ["ton-run"]
    assert start["text"] == "net: 8v+8sv rate=20 main@abc1234"
    assert chunk["text"] == "chunk 1: rate=20"
    assert start["time"] <= chunk["time"] <= end["timeEnd"]
    assert set(end) == {"timeEnd"}


@pytest.mark.asyncio
async def test_run_annotations_use_a_bearer_token_over_basic_auth(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls = _capture_annotations(monkeypatch)

    await run_local_net.RunAnnotations.from_env({"GRAFANA_TOKEN": "secret"}).point("run")

    assert [call["auth"] for call in calls] == ["Bearer secret"]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "env",
    [{"GRAFANA_URL": ""}, {"GRAFANA_AUTH": ""}, {"GRAFANA_AUTH": "", "GRAFANA_TOKEN": ""}],
)
async def test_empty_grafana_environment_disables_annotations(
    monkeypatch: pytest.MonkeyPatch, env: dict[str, str]
) -> None:
    calls = _capture_annotations(monkeypatch)

    async with run_local_net.RunAnnotations.from_env(env) as runs:
        await runs.start("run")
        await runs.point("chunk 1: rate=20")

    assert calls == []


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("error", "message"),
    [
        (OSError("connection refused"), "connection refused"),
        (http.client.BadStatusLine("not http at all"), "not http at all"),
    ],
)
async def test_annotation_failure_is_swallowed_and_stops_further_posts(
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
    error: Exception,
    message: str,
) -> None:
    calls = _capture_annotations(monkeypatch, error=error)

    with caplog.at_level("WARNING"):
        async with run_local_net.RunAnnotations.from_env({}) as runs:
            await runs.start("run")
            await runs.point("chunk 1: rate=20")

    assert len(calls) == 1
    assert [record.getMessage() for record in caplog.records] == [
        f"no run annotations: POST /api/annotations failed ({message})"
    ]


@pytest.mark.asyncio
async def test_annotations_close_the_region_when_the_run_is_cancelled(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The harness shuts down by cancelling its run task, and that must still close the region."""
    calls = _capture_annotations(monkeypatch)
    opened = asyncio.Event()

    async def run() -> None:
        async with run_local_net.RunAnnotations.from_env({}) as runs:
            await runs.start("run")
            opened.set()
            _ = await asyncio.Event().wait()

    task = asyncio.create_task(run())
    _ = await opened.wait()
    _ = task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assert [call["method"] for call in calls] == ["POST", "PATCH"]


@pytest.mark.asyncio
async def test_annotations_never_block_the_event_loop(monkeypatch: pytest.MonkeyPatch) -> None:
    """A slow Grafana must cost the supervising loop nothing but a context switch."""
    calls = _capture_annotations(monkeypatch, delay=0.2)
    ticks = 0

    async def tick() -> None:
        nonlocal ticks
        while True:
            await asyncio.sleep(0)
            ticks += 1

    ticker = asyncio.create_task(tick())
    await run_local_net.RunAnnotations.from_env({}).point("run")
    _ = ticker.cancel()

    assert len(calls) == 1
    assert ticks > 10  # a blocking urlopen would have let the ticker run zero times


@pytest.mark.asyncio
async def test_run_cmd_terminates_child_when_cancelled(monkeypatch: pytest.MonkeyPatch) -> None:
    class FakeProcess:
        def __init__(self) -> None:
            self.pid: int = 1234
            self.returncode: int | None = None
            self.terminated: bool = False
            self.killed: bool = False
            self._finished = asyncio.Event()

        async def wait(self) -> int:
            await self._finished.wait()
            assert self.returncode is not None
            return self.returncode

        def terminate(self) -> None:
            self.terminated = True
            self.returncode = -15
            self._finished.set()

        def kill(self) -> None:
            self.killed = True
            self.returncode = -9
            self._finished.set()

    process = FakeProcess()

    async def create_subprocess_exec(*_: object, **kwargs: object) -> FakeProcess:
        assert kwargs == {"start_new_session": True}
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create_subprocess_exec)
    monkeypatch.setattr(run_local_net.os, "killpg", lambda pid, sig: process.terminate())
    task = asyncio.create_task(run_local_net.run_cmd(["fake"], description="fake child"))
    await asyncio.sleep(0)
    task.cancel()

    with pytest.raises(asyncio.CancelledError):
        await task
    assert process.terminated
    assert not process.killed


@pytest.mark.asyncio
async def test_run_checked_propagates_load_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fail(*_: object, **__: object) -> int:
        return 17

    monkeypatch.setattr(run_local_net, "run_cmd", fail)
    with pytest.raises(RuntimeError, match="message-load chunk 3.*17"):
        await run_local_net.run_checked(["fake"], description="message-load chunk 3")
