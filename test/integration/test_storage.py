import argparse
import asyncio
import hashlib
import secrets
import socket
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import final

from tonapi import ton_api
from tontester.install import Install
from tontester.network import Network


@final
class Arguments(argparse.Namespace):
    legacy_build_dir: Path | None = None


COMMAND_TIMEOUT = 15
DOWNLOAD_TIMEOUT = 120
OVERALL_TIMEOUT = 5 * 60
PIECE_SIZE = 128 * 1024
_ALLOCATED_PORTS: set[int] = set()


def _port(kind: socket.SocketKind, *, quic: bool = False):
    for _ in range(100):
        port = 20000 + secrets.randbelow(30000)
        ports = {port, port + 1000} if quic else {port}
        if ports & _ALLOCATED_PORTS:
            continue
        try:
            with socket.socket(socket.AF_INET, kind) as sock:
                sock.bind(("127.0.0.1", port))
                if quic:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as quic_sock:
                        quic_sock.bind(("127.0.0.1", port + 1000))
            _ALLOCATED_PORTS.update(ports)
            return port
        except OSError:
            continue
    raise RuntimeError("Could not allocate a local port")


async def _stop(process: asyncio.subprocess.Process):
    if process.returncode is not None:
        return
    try:
        process.terminate()
    except ProcessLookupError:
        pass
    try:
        _ = await asyncio.wait_for(process.wait(), 5)
    except TimeoutError:
        try:
            process.kill()
        except ProcessLookupError:
            pass
        _ = await asyncio.wait_for(process.wait(), 5)


async def _run(*args: str | Path, cwd: Path, timeout: float = COMMAND_TIMEOUT):
    process = await asyncio.create_subprocess_exec(
        *args,
        cwd=cwd,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        async with asyncio.timeout(timeout):
            stdout, stderr = await process.communicate()
        if process.returncode != 0:
            message = (
                f"{args[0]} exited with {process.returncode}:\n"
                f"{stdout.decode(errors='replace')}\n{stderr.decode(errors='replace')}"
            )
            raise RuntimeError(message)
        return stdout.decode()
    finally:
        await _stop(process)


@final
class Daemon:
    def __init__(self, name: str, *, install: Install, root: Path, config: Path):
        self.install = install
        self.db = root / name
        self.log = root / f"{name}.log"
        self.config = config
        self.port = _port(socket.SOCK_DGRAM, quic=True)
        self.control_port = _port(socket.SOCK_STREAM)
        self.process: asyncio.subprocess.Process | None = None

    async def start(self, *, client_only: bool = False):
        self.db.mkdir(exist_ok=True)
        address = f":{self.port}" if client_only else f"127.0.0.1:{self.port}"
        with self.log.open("ab") as log:
            self.process = await asyncio.create_subprocess_exec(
                self.install.build_dir / "storage/storage-daemon/storage-daemon",
                "--global-config",
                self.config,
                "--db",
                self.db,
                "--ip",
                address,
                "--control-port",
                str(self.control_port),
                "--verbosity",
                "4",
                cwd=self.db,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=log,
                stderr=asyncio.subprocess.STDOUT,
            )
        async with asyncio.timeout(30):
            while True:
                if self.process.returncode is not None:
                    raise RuntimeError(f"Storage daemon stopped:\n{self.log.read_text()}")
                if (self.db / "cli-keys/server.pub").is_file():
                    try:
                        _ = await self.command("list --json", timeout=2)
                        return
                    except RuntimeError, TimeoutError:
                        pass
                await asyncio.sleep(0.1)

    async def stop(self):
        if self.process is not None:
            await _stop(self.process)
            self.process = None

    async def command(self, command: str, *, timeout: float = COMMAND_TIMEOUT):
        return await _run(
            self.install.build_dir / "storage/storage-daemon/storage-daemon-cli",
            "--ip",
            f"127.0.0.1:{self.control_port}",
            "--key",
            self.db / "cli-keys/client",
            "--pub",
            self.db / "cli-keys/server.pub",
            "--cmd",
            command,
            cwd=self.db,
            timeout=timeout,
        )

    async def info(self, bag_id: str):
        info = ton_api.Storage_daemon_torrentFull.from_json(
            await self.command(f"get {bag_id} --json")
        ).torrent
        assert info is not None
        return info


async def _wait_download(daemon: Daemon, bag_id: str, *, complete: bool = True):
    async with asyncio.timeout(DOWNLOAD_TIMEOUT):
        while True:
            info = await daemon.info(bag_id)
            assert not info.fatal_error, info.fatal_error
            if complete and info.completed:
                return info
            if not complete and info.downloaded_size >= 2 * PIECE_SIZE:
                assert not info.completed, "Download finished before the restart scenario"
                return info
            await asyncio.sleep(0.25)


def _make_bag(directory: Path):
    files = {"payload.bin": 4 * 1024 * 1024 + 17, "nested/data.bin": 1024 * 1024 + 31}
    for name, size in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        _ = path.write_bytes(hashlib.shake_256(name.encode()).digest(size))
    _ = (directory / "small.txt").write_text("TON Storage over QUIC\n")


def _verify_files(source: Path, target: Path):
    def hashes(directory: Path):
        return {
            path.relative_to(directory): hashlib.sha256(path.read_bytes()).digest()
            for path in directory.rglob("*")
            if path.is_file()
        }

    assert hashes(target) == hashes(source), f"Downloaded files differ in {target}"


async def main(legacy_build_dir: Path | None = None):
    repo_root = Path(__file__).resolve().parents[2]
    install = Install(repo_root / "build", repo_root)
    server_install = Install(legacy_build_dir, repo_root) if legacy_build_dir else install
    with TemporaryDirectory(prefix="ton-storage-quic-") as temporary:
        root = Path(temporary)
        async with Network(
            install, root / "dht", port_base=_port(socket.SOCK_DGRAM) - 1
        ) as network:
            dht = network.create_dht_node()
            await dht.run()
            config = root / "global.json"
            _ = config.write_text(
                ton_api.Config_global(
                    dht=ton_api.Dht_config_global(
                        static_nodes=ton_api.Dht_nodes(nodes=[dht.signed_address]), k=3, a=3
                    ),
                    validator=None,
                ).to_json()
            )
            a = Daemon("a", install=server_install, root=root, config=config)
            b = Daemon("b", install=install, root=root, config=config)
            client = Daemon("client", install=install, root=root, config=config)
            try:
                await a.start()
                await b.start()
                source = root / "bag"
                _make_bag(source)
                created = ton_api.Storage_daemon_torrentFull.from_json(
                    await a.command(f'create --json "{source}"')
                )
                assert created.torrent is not None
                bag_id = created.torrent.hash.hex()
                assert created.torrent.completed
                assert len(created.files) == 3
                pieces = ton_api.Storage_daemon_torrentPiecesInfo.from_json(
                    await a.command(f"get-pieces-info {bag_id} --json")
                )
                assert pieces.total_pieces > 1

                destination = root / "forward"
                _ = await b.command("set-speed-limits --download 256k")
                _ = await b.command(f'add-by-hash {bag_id} -d "{destination}"')
                _ = await _wait_download(b, bag_id, complete=False)
                _ = await b.command(f"download-pause {bag_id}")
                await asyncio.sleep(21)
                paused = await b.info(bag_id)
                assert not paused.active_download and not paused.completed
                assert paused.downloaded_size >= 2 * PIECE_SIZE
                if legacy_build_dir is None:
                    await a.stop()
                    offset = b.log.stat().st_size
                    _ = await b.command(f"download-resume {bag_id}")
                    await asyncio.sleep(11)
                    _ = await b.command(f"download-pause {bag_id}")
                    failed_logs = b.log.read_text(errors="replace")[offset:]
                    assert "Selected Storage transport: QUIC" in failed_logs
                    assert "Selected Storage transport: RLDP2" not in failed_logs
                    assert not (await b.info(bag_id)).completed
                    await a.start()
                    print("PASS: QUIC failure does not select RLDP2", flush=True)
                await b.stop()
                await b.start()
                restored = await b.info(bag_id)
                assert not restored.active_download and not restored.completed
                assert restored.downloaded_size >= paused.downloaded_size
                _ = await b.command("set-speed-limits --download unlimited")
                _ = await b.command(f"download-resume {bag_id}")
                _ = await _wait_download(b, bag_id)
                _verify_files(source, destination / source.name)
                print("PASS: multipart download, pause, restart and resume", flush=True)

                _ = await a.command(f"remove {bag_id}")
                reverse = root / "reverse"
                _ = await a.command(f'add-by-hash {bag_id} -d "{reverse}"')
                _ = await _wait_download(a, bag_id)
                _verify_files(source, reverse / source.name)
                print("PASS: download in the opposite direction", flush=True)

                await b.stop()
                await client.start(client_only=True)
                client_destination = root / "client-download"
                _ = await client.command(f'add-by-hash {bag_id} -d "{client_destination}"')
                _ = await _wait_download(client, bag_id)
                _verify_files(source, client_destination / source.name)
                print("PASS: daemon with no advertised address", flush=True)

                meta = root / "bag.torrent"
                _ = await a.command(f'get-meta {bag_id} "{meta}"')
                cli_destination = root / "cli-download"
                cli_destination.mkdir()
                _ = await _run(
                    install.build_dir / "storage/storage-cli",
                    "--config",
                    config,
                    "--ip",
                    f"127.0.0.1:{_port(socket.SOCK_DGRAM, quic=True)}",
                    "--db",
                    root / "cli-db",
                    "--execute",
                    "download ../bag.torrent",
                    cwd=cli_destination,
                    timeout=DOWNLOAD_TIMEOUT,
                )
                _verify_files(source, cli_destination / source.name)
                print("PASS: storage-cli download", flush=True)
                logs = "\n".join(path.read_text(errors="replace") for path in root.glob("*.log"))
                assert "Selected Storage transport: RLDP2" in logs
                if legacy_build_dir is None:
                    assert "Selected Storage transport: QUIC" in logs
                else:
                    assert "Selected Storage transport: QUIC" not in logs
                    print("PASS: RLDP2 compatibility in both directions", flush=True)
            except BaseException:
                for log in root.glob("*.log"):
                    print(f"\n{log.name}:\n{log.read_text(errors='replace')[-8000:]}", flush=True)
                raise
            finally:
                _ = await asyncio.gather(a.stop(), b.stop(), client.stop())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    _ = parser.add_argument("--legacy-build-dir", type=Path)
    args = parser.parse_args(namespace=Arguments())

    async def run():
        await main()
        if args.legacy_build_dir is not None:
            await main(args.legacy_build_dir)

    asyncio.run(asyncio.wait_for(run(), OVERALL_TIMEOUT * 2))
