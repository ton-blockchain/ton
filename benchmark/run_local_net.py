"""Local validator net with Prometheus exporters, for dashboard work and benchmarks.

Boots N validators (optionally on a pre-generated bench state, see bench-state-gen /
DESIGN.md), plus M dedicated collator nodes, optionally spamming presigned jetton
transfers. See doc/local-net-dashboards.md.

Run from the repo root:
  uv run python benchmark/run_local_net.py --net-dir .../net --state .../manifest.json --rate 0
  uv run python benchmark/run_local_net.py --net-dir .../net --validators 1 --collators 1
"""

import argparse
import asyncio
import base64
import fcntl
import http.client
import json
import logging
import math
import os
import shutil
import signal
import stat
import subprocess
import sys
import time
import urllib.request
import uuid
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from tontester.install import Install
from tontester.network import LiteserverEndpoint, Network, StartOptions
from tontester.zerostate import ExternalBasechainState, SimplexConsensusConfig

l = logging.getLogger("local-net")
FULL_SHARD = -(2**63)

_MARKER_NAME = ".ton-local-net.json"
_LOCK_NAME = ".ton-local-net.pid"
_MARKER_VERSION = 1
_MAX_COLLATORS = 8
_SUBPROCESS_TERM_TIMEOUT_S = 10.0
_SUBPROCESS_KILL_TIMEOUT_S = 5.0
_ANNOTATION_TAG = "ton-run"  # the tag every TON dashboard's "Runs" layer displays
_ANNOTATION_TIMEOUT_S = 2.0


@dataclass(frozen=True, slots=True)
class LocalNetConfig:
    repo: Path
    build: Path
    state: Path | None
    external_basechain: ExternalBasechainState | None
    total_wallets: int | None
    net_dir: Path
    validators: int
    collators: int
    shard_validators: int
    valgroup_lifetime: int
    rate: float
    chunk_s: int
    messages_per_chunk: int
    target_ms: int
    min_block_interval_ms: int
    mc_protocol: int
    shard_protocol: int
    exporter_base: int
    port_base: int
    verbosity: int


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument(
        "--build",
        type=Path,
        default=None,
        help="build dir (default: <repo>/cmake-build-relwithdebinfo)",
    )
    parser.add_argument("--state", type=Path, default=None, help="bench-state manifest.json")
    parser.add_argument(
        "--net-dir",
        type=Path,
        required=True,
        help="dedicated harness-owned working dir; created or safely recreated",
    )
    parser.add_argument("--validators", type=int, default=8)
    parser.add_argument("--collators", type=int, default=0, help="dedicated non-validator nodes")
    parser.add_argument(
        "--shard-validators",
        type=int,
        default=None,
        help="shard validator group size (default: min(8, --validators))",
    )
    parser.add_argument(
        "--valgroup-lifetime",
        type=int,
        default=250,
        help="catchain lifetime; collator registry entries are picked up on its boundaries",
    )
    parser.add_argument("--rate", type=float, default=0.0, help="messages/s; 0 = idle")
    parser.add_argument("--chunk-s", type=int, default=600, help="seconds per spam chunk")
    parser.add_argument("--target-ms", type=int, default=400)
    parser.add_argument("--min-block-interval-ms", type=int, default=300)
    parser.add_argument("--mc-protocol", type=int, default=0, help="MC protocol (mainnet: 0)")
    parser.add_argument("--shard-protocol", type=int, default=1, help="shard protocol (mainnet: 1)")
    parser.add_argument("--exporter-base", type=int, default=9101, help="first exporter port")
    parser.add_argument(
        "--port-base",
        type=int,
        default=2000,
        help="allocation base; first ADNL port is base + 1",
    )
    parser.add_argument("--verbosity", type=int, default=1, help="validator-engine verbosity")
    return parser


def parse_args(argv: list[str] | None = None) -> LocalNetConfig:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return validate_args(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))


def _bounded_int(name: str, value: int, minimum: int, maximum: int) -> int:
    if not minimum <= value <= maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}], got {value}")
    return value


def _resolve_existing_directory(path: Path, option: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_dir():
        raise ValueError(f"{option} is not a directory: {path}")
    return path


def _require_file(path: Path, description: str, *, executable: bool = False) -> None:
    if not path.is_file():
        raise ValueError(f"missing {description}: {path}")
    if executable and not os.access(path, os.X_OK):
        raise ValueError(f"{description} is not executable: {path}")


def _tonlib_path(build: Path) -> Path:
    if sys.platform.startswith("linux"):
        return build / "tonlib/libtonlibjson.so"
    if sys.platform == "darwin":
        return build / "tonlib/libtonlibjson.dylib"
    raise ValueError(f"unsupported platform: {sys.platform}")


def _paths_overlap(left: Path, right: Path) -> bool:
    return left == right or left.is_relative_to(right) or right.is_relative_to(left)


def _validate_net_dir_location(
    raw_net_dir: Path,
    *,
    repo: Path,
    build: Path,
    state_dir: Path | None,
) -> Path:
    expanded = raw_net_dir.expanduser()
    if expanded.is_symlink():
        raise ValueError(f"--net-dir itself must not be a symlink: {expanded}")
    net_dir = expanded.resolve(strict=False)

    filesystem_root = Path(net_dir.anchor)
    home = Path.home().resolve()
    temp_root = Path(os.getenv("TMPDIR", "/tmp")).resolve()
    standard_temp_root = Path("/tmp").resolve()
    if net_dir == filesystem_root:
        raise ValueError(f"refusing dangerous --net-dir: {net_dir}")
    for label, broad_path in (
        ("home directory", home),
        ("temporary directory", temp_root),
        ("system temporary directory", standard_temp_root),
    ):
        if broad_path.is_relative_to(net_dir):
            raise ValueError(f"--net-dir must not be an ancestor of the {label}: {net_dir}")

    protected = [("repository", repo), ("build directory", build)]
    if state_dir is not None:
        protected.append(("bench state", state_dir))
    for label, path in protected:
        if _paths_overlap(net_dir, path):
            raise ValueError(f"--net-dir must not overlap the {label}: {net_dir} vs {path}")

    existing_parent = net_dir
    while not existing_parent.exists() and existing_parent != existing_parent.parent:
        existing_parent = existing_parent.parent
    if not existing_parent.is_dir():
        raise ValueError(f"--net-dir has a non-directory parent: {existing_parent}")
    if not os.access(existing_parent, os.W_OK | os.X_OK):
        raise ValueError(f"--net-dir parent is not writable: {existing_parent}")

    _preflight_owned_net_dir(net_dir)
    return net_dir


def _read_json_object(path: Path, description: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {description} {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{description} must contain a JSON object: {path}")
    return value


def _load_bench_state(
    path: Path,
) -> tuple[ExternalBasechainState, dict[str, object]]:
    manifest = _read_json_object(path, "bench-state manifest")

    def string_field(name: str) -> str:
        value = manifest.get(name)
        if not isinstance(value, str):
            raise ValueError(f"{path}: field {name!r} must be a string")
        return value

    def integer_field(name: str) -> int:
        value = manifest.get(name)
        if type(value) is not int:
            raise ValueError(f"{path}: field {name!r} must be an integer")
        return value

    def hash_field(name: str) -> bytes:
        encoded = string_field(name)
        if len(encoded) != 64:
            raise ValueError(f"{path}: field {name!r} must contain exactly 32 bytes of hex")
        try:
            decoded = bytes.fromhex(encoded)
        except ValueError as error:
            raise ValueError(f"{path}: field {name!r} is not hexadecimal") from error
        # bytes.fromhex() ignores ASCII whitespace, so checking the source-text length above is
        # necessary but not sufficient to guarantee a 256-bit hash.
        if len(decoded) != 32:
            raise ValueError(f"{path}: field {name!r} must contain exactly 32 bytes of hex")
        return decoded

    total_balance_text = string_field("total_balance")
    try:
        total_balance = int(total_balance_text)
    except ValueError as error:
        raise ValueError(f"{path}: field 'total_balance' is not decimal") from error
    if not 0 <= total_balance < 2**128:
        raise ValueError(f"{path}: field 'total_balance' is outside uint128 range")

    gen_utime = integer_field("gen_utime")
    if not 0 <= gen_utime < 2**32:
        raise ValueError(f"{path}: field 'gen_utime' is outside uint32 range")

    return (
        ExternalBasechainState(
            root_hash=hash_field("root_hash_hex"),
            file_hash=hash_field("file_hash_hex"),
            total_balance=total_balance,
            gen_utime=gen_utime,
        ),
        manifest,
    )


def _marker_payload(net_dir: Path) -> dict[str, object]:
    return {
        "kind": "ton-local-net",
        "version": _MARKER_VERSION,
        "path": str(net_dir),
    }


def _validate_marker(net_dir: Path) -> None:
    marker = net_dir / _MARKER_NAME
    if marker.is_symlink() or not marker.is_file():
        raise ValueError(
            f"existing --net-dir is not owned by this harness (missing {marker}); "
            "use a new path or move the old directory aside"
        )
    if _read_json_object(marker, "net-dir marker") != _marker_payload(net_dir):
        raise ValueError(f"invalid or relocated net-dir marker: {marker}")


def _validate_marker_at(directory_descriptor: int, net_dir: Path) -> None:
    try:
        marker_descriptor = os.open(
            _MARKER_NAME,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=directory_descriptor,
        )
        with os.fdopen(marker_descriptor) as marker_file:
            marker = json.load(marker_file)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read net-dir marker for {net_dir}: {error}") from error
    if marker != _marker_payload(net_dir):
        raise ValueError(f"invalid or relocated net-dir marker in {net_dir}")


def _read_lock(lock_path: Path) -> tuple[int, str]:
    lock = _read_json_object(lock_path, "net-dir PID lock")
    pid = lock.get("pid")
    token = lock.get("token")
    if type(pid) is not int or pid <= 0 or not isinstance(token, str) or not token:
        raise ValueError(f"malformed net-dir PID lock: {lock_path}")
    return pid, token


def _preflight_owned_net_dir(net_dir: Path) -> None:
    if not net_dir.exists():
        return
    if net_dir.is_symlink() or not net_dir.is_dir():
        raise ValueError(f"existing --net-dir must be a real directory: {net_dir}")
    _validate_marker(net_dir)
    lock_path = net_dir / _LOCK_NAME
    if lock_path.exists():
        if lock_path.is_symlink() or not lock_path.is_file():
            raise ValueError(f"net-dir PID lock must be a regular file: {lock_path}")
        descriptor = os.open(lock_path, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
        try:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                try:
                    pid, _ = _read_lock(lock_path)
                    owner = f"live PID {pid}"
                except ValueError:
                    owner = "another process"
                raise ValueError(
                    f"--net-dir is already supervised by {owner}: {net_dir}"
                ) from error
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _write_json_exclusive(path: Path, value: dict[str, object]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(value, output, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


class NetDirectoryLease:
    """Exclusive ownership of a validated, harness-owned net directory."""

    def __init__(
        self,
        path: Path,
        lock_descriptor: int,
        directory_descriptor: int,
        directory_identity: tuple[int, int],
    ):
        self.path: Path = path
        self._lock_descriptor: int | None = lock_descriptor
        self._directory_descriptor: int | None = directory_descriptor
        self._directory_identity: tuple[int, int] = directory_identity

    @classmethod
    def acquire(cls, path: Path) -> "NetDirectoryLease":
        try:
            path.mkdir(parents=True)
        except FileExistsError:
            pass
        else:
            try:
                _write_json_exclusive(path / _MARKER_NAME, _marker_payload(path))
            except BaseException:
                path.rmdir()
                raise

        _preflight_owned_net_dir(path)
        directory_descriptor = os.open(
            path,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            directory_stat = os.fstat(directory_descriptor)
            path_stat = path.stat(follow_symlinks=False)
        except BaseException:
            os.close(directory_descriptor)
            raise
        if (
            path.is_symlink()
            or not stat.S_ISDIR(directory_stat.st_mode)
            or (path_stat.st_dev, path_stat.st_ino)
            != (directory_stat.st_dev, directory_stat.st_ino)
        ):
            os.close(directory_descriptor)
            raise RuntimeError(f"--net-dir changed while acquiring its lock: {path}")
        try:
            _validate_marker_at(directory_descriptor, path)
        except BaseException:
            os.close(directory_descriptor)
            raise

        lock_path = path / _LOCK_NAME
        token = uuid.uuid4().hex
        lock_payload: dict[str, object] = {
            "pid": os.getpid(),
            "token": token,
            "started_at": time.time(),
            "path": str(path),
        }
        try:
            lock_descriptor = os.open(
                _LOCK_NAME,
                os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0),
                0o600,
                dir_fd=directory_descriptor,
            )
        except BaseException:
            os.close(directory_descriptor)
            raise
        try:
            fcntl.flock(lock_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            os.close(lock_descriptor)
            os.close(directory_descriptor)
            try:
                pid, _ = _read_lock(lock_path)
                owner = f"live PID {pid}"
            except ValueError:
                owner = "another process"
            raise RuntimeError(f"--net-dir is already supervised by {owner}: {path}") from error
        except BaseException:
            os.close(lock_descriptor)
            os.close(directory_descriptor)
            raise

        try:
            encoded_lock = (json.dumps(lock_payload, sort_keys=True) + "\n").encode()
            os.ftruncate(lock_descriptor, 0)
            os.lseek(lock_descriptor, 0, os.SEEK_SET)
            while encoded_lock:
                written = os.write(lock_descriptor, encoded_lock)
                if written == 0:
                    raise OSError(f"short write while creating PID lock: {lock_path}")
                encoded_lock = encoded_lock[written:]
            os.fsync(lock_descriptor)
        except BaseException:
            fcntl.flock(lock_descriptor, fcntl.LOCK_UN)
            os.close(lock_descriptor)
            os.close(directory_descriptor)
            raise

        try:
            path_stat = path.stat(follow_symlinks=False)
            path_changed = (
                path.is_symlink()
                or path.resolve() != path
                or (path_stat.st_dev, path_stat.st_ino)
                != (directory_stat.st_dev, directory_stat.st_ino)
            )
        except BaseException:
            fcntl.flock(lock_descriptor, fcntl.LOCK_UN)
            os.close(lock_descriptor)
            os.close(directory_descriptor)
            raise
        if path_changed:
            fcntl.flock(lock_descriptor, fcntl.LOCK_UN)
            os.close(lock_descriptor)
            os.close(directory_descriptor)
            raise RuntimeError(f"--net-dir changed while acquiring its lock: {path}")
        lease = cls(
            path,
            lock_descriptor,
            directory_descriptor,
            (directory_stat.st_dev, directory_stat.st_ino),
        )
        try:
            lease._clear_previous_run()
        except BaseException:
            lease.close()
            raise
        return lease

    def _clear_previous_run(self) -> None:
        assert self._directory_descriptor is not None
        directory_stat = self.path.stat(follow_symlinks=False)
        if (
            self.path.is_symlink()
            or not self.path.is_dir()
            or self.path.resolve() != self.path
            or (directory_stat.st_dev, directory_stat.st_ino) != self._directory_identity
        ):
            raise RuntimeError(f"--net-dir changed after its lock was acquired: {self.path}")
        if not shutil.rmtree.avoids_symlink_attacks:
            raise RuntimeError("safe net-dir cleanup is unsupported on this platform")
        preserved = {_MARKER_NAME, _LOCK_NAME}
        for name in os.listdir(self._directory_descriptor):
            if name in preserved:
                continue
            entry_stat = os.stat(name, dir_fd=self._directory_descriptor, follow_symlinks=False)
            if stat.S_ISDIR(entry_stat.st_mode):
                shutil.rmtree(name, dir_fd=self._directory_descriptor)
            else:
                os.unlink(name, dir_fd=self._directory_descriptor)

    def close(self) -> None:
        if self._lock_descriptor is None:
            return
        descriptor = self._lock_descriptor
        self._lock_descriptor = None
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            try:
                os.close(descriptor)
            finally:
                assert self._directory_descriptor is not None
                os.close(self._directory_descriptor)
                self._directory_descriptor = None

    def __enter__(self) -> "NetDirectoryLease":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def validate_args(args: argparse.Namespace) -> LocalNetConfig:
    """Validate and normalize every CLI value without mutating external state."""

    repo = _resolve_existing_directory(args.repo, "--repo")
    build_arg = args.build if args.build is not None else repo / "cmake-build-relwithdebinfo"
    build = _resolve_existing_directory(build_arg, "--build")

    validators = _bounded_int("--validators", args.validators, 1, 10_000)
    collators = _bounded_int("--collators", args.collators, 0, _MAX_COLLATORS)
    shard_validators_arg = args.shard_validators
    if shard_validators_arg is None:
        shard_validators_arg = min(8, validators)
    shard_validators = _bounded_int("--shard-validators", shard_validators_arg, 1, validators)
    valgroup_lifetime = _bounded_int("--valgroup-lifetime", args.valgroup_lifetime, 1, 2**31 - 1)
    chunk_s = _bounded_int("--chunk-s", args.chunk_s, 1, 2**31 - 1)
    target_ms = _bounded_int("--target-ms", args.target_ms, 1, 2**31 - 1)
    min_block_interval_ms = _bounded_int(
        "--min-block-interval-ms", args.min_block_interval_ms, 0, target_ms
    )
    mc_protocol = _bounded_int("--mc-protocol", args.mc_protocol, 0, 2**31 - 1)
    shard_protocol = _bounded_int("--shard-protocol", args.shard_protocol, 0, 2**31 - 1)
    exporter_base = _bounded_int("--exporter-base", args.exporter_base, 1, 65_535)
    port_base = _bounded_int("--port-base", args.port_base, 0, 65_534)
    verbosity = _bounded_int("--verbosity", args.verbosity, 0, 100)

    rate = args.rate
    if not math.isfinite(rate) or rate < 0:
        raise ValueError(f"--rate must be a finite non-negative number, got {rate}")
    if exporter_base + validators + collators - 1 > 65_535:
        raise ValueError("exporter port range exceeds 65535")
    # One DHT address followed by three addresses for every full node.
    node_port_first = port_base + 1
    node_port_last = port_base + 1 + 3 * (validators + collators)
    if node_port_last > 65_535:
        raise ValueError("ADNL/liteserver/console port range exceeds 65535")
    exporter_port_last = exporter_base + validators + collators - 1
    if max(node_port_first, exporter_base) <= min(node_port_last, exporter_port_last):
        raise ValueError(
            "exporter port range overlaps the DHT/ADNL/liteserver/console port range: "
            f"{exporter_base}-{exporter_port_last} vs {node_port_first}-{node_port_last}"
        )

    state: Path | None = None
    state_dir: Path | None = None
    external_basechain: ExternalBasechainState | None = None
    total_wallets: int | None = None
    state_arg: Path | None = args.state
    if state_arg is not None:
        state_path = state_arg.expanduser().resolve()
        _require_file(state_path, "bench-state manifest")
        state = state_path
        state_dir = state_path.parent
        if not (state_dir / "celldb").is_dir():
            raise ValueError(f"bench state has no celldb directory: {state_dir / 'celldb'}")
        external_basechain, manifest = _load_bench_state(state_path)
        if rate > 0:
            num_v5 = manifest.get("num_v5")
            if type(num_v5) is not int or num_v5 <= 0:
                raise ValueError("positive --rate requires a positive integer num_v5 in --state")
            total_wallets = num_v5
    elif rate > 0:
        raise ValueError("positive --rate requires --state (the spammer uses its wallet pool)")

    messages_per_chunk = 0
    if rate > 0:
        # Keep this byte-for-byte equivalent to bench-spam's non-negative
        # static_cast<uint64>(rate * duration + 0.5). Warmup is part of duration and only changes
        # the measurement window; it does not consume another set of wallets.
        rounded_messages = rate * chunk_s + 0.5
        if not math.isfinite(rounded_messages) or rounded_messages > sys.maxsize:
            raise ValueError("--rate and --chunk-s produce an unrepresentable chunk size")
        messages_per_chunk = int(rounded_messages)
        if messages_per_chunk == 0:
            raise ValueError("--rate and --chunk-s round to zero messages per chunk")
    if total_wallets is not None and total_wallets < messages_per_chunk:
        raise ValueError(
            f"bench state has {total_wallets} wallets, fewer than one load chunk "
            f"({messages_per_chunk})"
        )

    net_dir = _validate_net_dir_location(
        args.net_dir,
        repo=repo,
        build=build,
        state_dir=state_dir,
    )

    required_executables = {
        "generate-random-id": build / "utils/generate-random-id",
        "validator-engine": build / "validator-engine/validator-engine",
        "dht-server": build / "dht-server/dht-server",
        "create-state": build / "crypto/create-state",
    }
    if state is not None:
        required_executables["bench-state-gen"] = build / "benchmark/bench-state-gen"
    if rate > 0:
        required_executables["bench-spam"] = build / "benchmark/bench-spam"
        contracts = repo / "benchmark/contracts"
        if not contracts.is_dir():
            raise ValueError(f"missing benchmark contracts directory: {contracts}")
    for description, path in required_executables.items():
        _require_file(path, description, executable=True)
    _require_file(_tonlib_path(build), "tonlibjson")

    return LocalNetConfig(
        repo=repo,
        build=build,
        state=state,
        external_basechain=external_basechain,
        total_wallets=total_wallets,
        net_dir=net_dir,
        validators=validators,
        collators=collators,
        shard_validators=shard_validators,
        valgroup_lifetime=valgroup_lifetime,
        rate=rate,
        chunk_s=chunk_s,
        messages_per_chunk=messages_per_chunk,
        target_ms=target_ms,
        min_block_interval_ms=min_block_interval_ms,
        mc_protocol=mc_protocol,
        shard_protocol=shard_protocol,
        exporter_base=exporter_base,
        port_base=port_base,
        verbosity=verbosity,
    )


def _git_revision(repo: Path) -> str:
    """`<branch>@<short sha>` of the tree, or an empty string when git cannot say.

    Blocking: every caller runs it off the event loop.
    """

    def rev_parse(*flags: str) -> str | None:
        try:
            done = subprocess.run(
                ["git", "-C", str(repo), "rev-parse", *flags],
                capture_output=True,
                text=True,
                timeout=_ANNOTATION_TIMEOUT_S,
            )
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
        return done.stdout.strip() if done.returncode == 0 else None

    branch, revision = rev_parse("--abbrev-ref", "HEAD"), rev_parse("--short", "HEAD")
    return f"{branch}@{revision}" if branch and revision else ""


async def run_annotation_text(config: LocalNetConfig) -> str:
    """What a run's annotation says: which net, how big, how loaded, and from which build."""
    text = (
        f"{config.net_dir.name}: {config.validators}v+{config.shard_validators}sv "
        f"rate={config.rate}"
    )
    revision = await asyncio.to_thread(_git_revision, config.repo)
    return f"{text} {revision}" if revision else text


class RunAnnotations:
    """Best-effort Grafana run annotations, so every dashboard can show what was running.

    Never fatal and never retried: the first failure logs one warning and disables posting for the
    rest of the run. The run is a region opened at net-up and closed on shutdown, so a harness that
    is killed outright leaves the opening point behind rather than an unterminated region.

    Every request is a blocking urlopen bounded by _ANNOTATION_TIMEOUT_S, handed to a worker thread
    so a slow or dead Grafana cannot stall the nets this harness is supervising.
    """

    def __init__(self, url: str, headers: dict[str, str] | None):
        self._url = url.rstrip("/")
        self._headers = headers if self._url else None
        self._region: int | None = None

    @classmethod
    def from_env(cls, env: Mapping[str, str]) -> "RunAnnotations":
        """GRAFANA_TOKEN (bearer) wins over GRAFANA_AUTH (user:pass); any empty value disables."""
        token = env.get("GRAFANA_TOKEN", "")
        auth = env.get("GRAFANA_AUTH", "admin:admin")
        if token:
            credentials = f"Bearer {token}"
        elif auth:
            credentials = "Basic " + base64.b64encode(auth.encode()).decode()
        else:
            credentials = None
        url = env.get("GRAFANA_URL", "http://127.0.0.1:3000")
        return cls(url, {"Authorization": credentials} if credentials else None)

    async def _request(self, path: str, payload: dict[str, object], *, method: str) -> object:
        if self._headers is None:
            return None
        request = urllib.request.Request(
            f"{self._url}{path}",
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"} | self._headers,
            method=method,
        )

        def send() -> object:
            with urllib.request.urlopen(request, timeout=_ANNOTATION_TIMEOUT_S) as response:
                return json.loads(response.read() or b"null")

        try:
            return await asyncio.to_thread(send)
        except (OSError, ValueError, http.client.HTTPException) as error:
            l.warning("no run annotations: %s %s failed (%s)", method, path, error)
            self._headers = None
            return None

    async def _post(self, text: str) -> object:
        return await self._request(
            "/api/annotations",
            {"time": int(time.time() * 1000), "tags": [_ANNOTATION_TAG], "text": text},
            method="POST",
        )

    async def start(self, text: str) -> None:
        """Open the run's region; without its id the run stays a point annotation."""
        answer = await self._post(text)
        region = answer.get("id") if isinstance(answer, dict) else None
        self._region = region if isinstance(region, int) else None

    async def point(self, text: str) -> None:
        _ = await self._post(text)

    async def finish(self) -> None:
        if self._region is None:
            return
        region, self._region = self._region, None
        _ = await self._request(
            f"/api/annotations/{region}",
            {"timeEnd": int(time.time() * 1000)},
            method="PATCH",
        )

    async def __aenter__(self) -> "RunAnnotations":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.finish()


async def _terminate_process(
    process: asyncio.subprocess.Process,
    *,
    description: str,
) -> None:
    if process.returncode is not None:
        await process.wait()
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        await asyncio.wait_for(asyncio.shield(process.wait()), _SUBPROCESS_TERM_TIMEOUT_S)
        return
    except TimeoutError:
        l.warning("%s ignored SIGTERM; sending SIGKILL", description)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    await asyncio.wait_for(asyncio.shield(process.wait()), _SUBPROCESS_KILL_TIMEOUT_S)


async def run_cmd(cmd: list[str], *, description: str) -> int:
    process = await asyncio.create_subprocess_exec(*cmd, start_new_session=True)
    try:
        return await process.wait()
    except asyncio.CancelledError:
        try:
            await asyncio.shield(_terminate_process(process, description=description))
        except Exception:
            l.exception("failed to stop cancelled subprocess: %s", description)
        raise


async def run_checked(cmd: list[str], *, description: str) -> None:
    return_code = await run_cmd(cmd, description=description)
    if return_code != 0:
        raise RuntimeError(f"{description} failed with exit code {return_code}")


def _bench_spam_command(
    config: LocalNetConfig,
    endpoint: LiteserverEndpoint,
    *,
    chunk: int,
    offset: int,
) -> list[str]:
    """Build one load command whose complete output stays inside the leased net directory."""

    stem = f"chunk-{chunk:03d}"
    spam_dir = config.net_dir / "spam"
    assert config.state is not None
    return [
        str(config.build / "benchmark/bench-spam"),
        "--manifest",
        str(config.state),
        "--contracts-dir",
        str(config.repo / "benchmark/contracts"),
        "--liteserver",
        f"{endpoint.host}:{endpoint.port}",
        "--liteserver-pubkey-b64",
        endpoint.pubkey_b64,
        "--rate",
        str(config.rate),
        "--duration",
        str(config.chunk_s),
        "--warmup",
        "5",
        "--wallet-offset",
        str(offset),
        "--track-sample",
        "0.02",
        "--out",
        str(spam_dir / f"{stem}.json"),
        "--blocks-csv",
        str(spam_dir / f"{stem}-blocks.csv"),
    ]


async def _run_local_net(config: LocalNetConfig) -> int:
    install = Install(config.build, config.repo)
    install.tonlibjson.client_set_verbosity_level(1)

    with NetDirectoryLease.acquire(config.net_dir):
        async with Network(install, config.net_dir, port_base=config.port_base) as network:
            network_config = network.config
            network_config.split = 0
            network_config.monitor_min_split = 0
            network_config.shard_validators = config.shard_validators
            network_config.mc_valgroup_lifetime = config.valgroup_lifetime
            network_config.shard_valgroup_lifetime = config.valgroup_lifetime

            def simplex(version: int) -> SimplexConsensusConfig:
                return SimplexConsensusConfig(
                    target_block_rate_ms=config.target_ms,
                    min_block_interval_ms=config.min_block_interval_ms,
                    protocol_version=version,
                )

            network_config.mc_consensus = simplex(config.mc_protocol)
            network_config.shard_consensus = simplex(config.shard_protocol)
            # Delegated collation needs the on-chain registry. Leaving it out otherwise keeps
            # this net's zerostate identical to every other tontester net's.
            network_config.validator_registry = config.collators > 0
            if config.external_basechain is not None:
                network.external_basechain = config.external_basechain

            dht = network.create_dht_node()
            validators = []
            for _ in range(config.validators):
                node = network.create_full_node()
                node.make_initial_validator()
                node.announce_to(dht)
                validators.append(node)

            collators = []
            for _ in range(config.collators):
                node = network.create_full_node()
                node.announce_to(dht)
                collators.append(node)
            collator_ids = [node.make_collator() for node in collators]
            if collator_ids:
                for node in validators:
                    node.set_collators_list(collator_ids, disable_self_collate=True)

            if config.state is not None:
                celldb_src = config.state.parent / "celldb"
                for node in validators + collators:
                    await run_checked(
                        [
                            str(config.build / "benchmark/bench-state-gen"),
                            "checkpoint",
                            "--src",
                            str(celldb_src),
                            "--dst",
                            str(node.directory / "celldb"),
                        ],
                        description=f"celldb checkpoint for {node.name}",
                    )

            await dht.run()
            for index, node in enumerate(validators + collators):
                await node.run(
                    StartOptions(
                        args=(
                            "--disable-state-serializer",
                            "--exporter-address",
                            f"127.0.0.1:{config.exporter_base + index}",
                        ),
                        verbosity=config.verbosity,
                    )
                )
            async with RunAnnotations.from_env(os.environ) as runs:
                await runs.start(await run_annotation_text(config))
                l.info("waiting for wc0 seqno >= 3")
                async with asyncio.timeout(600):
                    await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=3)

                if config.rate == 0:
                    l.info("idle mode: no load, keeping the net alive")
                    await asyncio.Future()
                    return 0

                assert config.state is not None
                assert config.total_wallets is not None
                endpoint = validators[0].liteserver_endpoint()
                spam_dir = config.net_dir / "spam"
                spam_dir.mkdir()
                offset, chunk = 0, 0
                while offset + config.messages_per_chunk <= config.total_wallets:
                    chunk += 1
                    l.info(
                        "chunk %d: offset=%d rate=%s for %ss",
                        chunk,
                        offset,
                        config.rate,
                        config.chunk_s,
                    )
                    await runs.point(f"chunk {chunk}: rate={config.rate}")
                    await run_checked(
                        _bench_spam_command(config, endpoint, chunk=chunk, offset=offset),
                        description=f"message-load chunk {chunk}",
                    )
                    offset += config.messages_per_chunk
                l.info("wallet pool exhausted after %d chunks; net stays up", chunk)
                await asyncio.Future()
                return 0


async def main(config: LocalNetConfig) -> int:
    """Run until completion or an interrupt/TERM/HUP requests graceful shutdown."""

    loop = asyncio.get_running_loop()
    shutdown = asyncio.Event()
    installed_signals: list[signal.Signals] = []

    def request_shutdown(received_signal: signal.Signals) -> None:
        if not shutdown.is_set():
            l.info("received %s; stopping load and child nodes", received_signal.name)
            shutdown.set()

    for received_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        try:
            loop.add_signal_handler(received_signal, request_shutdown, received_signal)
        except (NotImplementedError, RuntimeError):
            continue
        installed_signals.append(received_signal)

    run_task = asyncio.create_task(_run_local_net(config), name="local-net")
    shutdown_task = asyncio.create_task(shutdown.wait(), name="local-net-shutdown")
    try:
        done, _ = await asyncio.wait((run_task, shutdown_task), return_when=asyncio.FIRST_COMPLETED)
        if run_task in done:
            return await run_task

        run_task.cancel()
        try:
            return await run_task
        except asyncio.CancelledError:
            return 0
    finally:
        if not run_task.done():
            run_task.cancel()
            try:
                await run_task
            except asyncio.CancelledError:
                pass
        shutdown_task.cancel()
        try:
            await shutdown_task
        except asyncio.CancelledError:
            pass
        for received_signal in installed_signals:
            loop.remove_signal_handler(received_signal)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="[%(levelname)s][%(asctime)s] %(message)s")
    sys.exit(asyncio.run(main(parse_args())))
