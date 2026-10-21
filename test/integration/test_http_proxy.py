import argparse
import asyncio
import base64
import binascii
import hashlib
import json
import logging
import socket
from collections.abc import AsyncIterator
from contextlib import AsyncExitStack, asynccontextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import cast, final

from tonapi import ton_api
from tontester.install import Install
from tontester.key import Key
from tontester.network import Network


@final
class Arguments(argparse.Namespace):
    http_only: bool = False


DATA = (bytes(range(251)) * 13000)[: 3 * 1024 * 1024 + 17]


def unused_port(kind: socket.SocketKind = socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return cast(tuple[str, int], sock.getsockname())[1]


def proxy_udp_port() -> int:
    while True:
        port = unused_port(socket.SOCK_DGRAM)
        if port + 1000 > 65535:
            continue
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            try:
                sock.bind(("127.0.0.1", port + 1000))
            except OSError:
                continue
        return port


def adnl_address(key: Key) -> str:
    data = b"\x2d" + key.id
    checksum = binascii.crc_hqx(data, 0).to_bytes(2, "big")
    return base64.b32encode(data + checksum).decode()[1:].lower()


async def close_writer(writer: asyncio.StreamWriter) -> None:
    writer.close()
    try:
        await writer.wait_closed()
    except ConnectionError:
        pass


async def read_headers(reader: asyncio.StreamReader) -> tuple[str, dict[str, str]]:
    lines = (await reader.readuntil(b"\r\n\r\n")).decode("latin1").split("\r\n")
    headers: dict[str, str] = {}
    for line in lines[1:-2]:
        name, value = line.split(":", 1)
        headers[name.lower()] = value.strip()
    return lines[0], headers


async def read_chunks(reader: asyncio.StreamReader) -> tuple[bytes, dict[str, str]]:
    chunks: list[bytes] = []
    while True:
        size = int((await reader.readline()).split(b";", 1)[0], 16)
        if size == 0:
            trailers: dict[str, str] = {}
            while (line := await reader.readline()) != b"\r\n":
                assert line, "EOF before the last chunk's trailers"
                name, value = line.decode("latin1").split(":", 1)
                trailers[name.lower()] = value.strip()
            return b"".join(chunks), trailers
        chunks.append(await reader.readexactly(size))
        assert await reader.readexactly(2) == b"\r\n"


async def read_body(
    reader: asyncio.StreamReader, headers: dict[str, str]
) -> tuple[bytes, dict[str, str]]:
    if headers.get("transfer-encoding", "").lower() == "chunked":
        return await read_chunks(reader)
    if "content-length" in headers:
        return await reader.readexactly(int(headers["content-length"])), {}
    return await reader.read(), {}


def chunked_body(data: bytes, trailer: str) -> bytes:
    chunks: list[bytes] = []
    for offset in range(0, len(data), 65537):
        part = data[offset : offset + 65537]
        chunks += [f"{len(part):x}\r\n".encode(), part, b"\r\n"]
    chunks.append(f"0\r\nX-Upload-Hash: {trailer}\r\n\r\n".encode())
    return b"".join(chunks)


@final
class Backend:
    def __init__(self) -> None:
        self.stream_continue = asyncio.Event()
        self.upload_started = asyncio.Event()
        self.upload_closed = asyncio.Event()
        self.early_uploads: dict[str, bytes] = {}
        self.early_done = asyncio.Event()
        self.tunnel_upload = b""
        self.tunnel_done = asyncio.Event()
        self.interrupted_bytes = 0
        self.errors: list[Exception] = []

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request, headers = await read_headers(reader)
            method, target, _ = request.split(" ")
            path = (
                target.removeprefix("http://").partition("/")[2] if "://" in target else target[1:]
            )
            if path == "interrupted":
                while part := await reader.read(65536):
                    self.interrupted_bytes += len(part)
                    self.upload_started.set()
                self.upload_closed.set()
                return
            if path.startswith(("early-", "plain-")):
                connection = {
                    "early-implicit": "",
                    "early-keep": "Connection: keep-alive\r\n",
                    "early-eof": "Connection: keep-alive\r\n",
                    "early-close": "Connection: keep-alive, ClOsE\r\n",
                    "early-proxy-close": "Proxy-Connection: keep-alive, ClOsE\r\n",
                    "early-reject": "Connection: close\r\n",
                    "plain-reject": "Connection: close\r\n",
                    "plain-close": "Connection: close\r\n",
                    "plain-token-close": "Connection: keep-alive, ClOsE\r\n",
                }[path]
                code = 413 if path in ("early-reject", "plain-reject") else 200
                response = DATA[: 256 * 1024] if code == 413 else b"accepted"
                framing = "" if path == "early-eof" else f"Content-Length: {len(response)}\r\n"
                writer.write(
                    (f"HTTP/1.1 {code} Test\r\n{framing}{connection}\r\n").encode() + response
                )
                await writer.drain()
                if path == "early-eof":
                    writer.write_eof()
                if path in ("early-implicit", "early-keep", "early-eof") or path.startswith(
                    "plain-"
                ):
                    body, _ = await read_body(reader, headers)
                    self.early_uploads[path] = body
                    self.early_done.set()
                else:
                    _ = await reader.read()
                return
            if path == "stream":
                writer.write(
                    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    + b"Trailer: X-Download-Hash\r\nConnection: close\r\n\r\n"
                )
                first = DATA[: 256 * 1024]
                writer.write(f"{len(first):x}\r\n".encode() + first + b"\r\n")
                await writer.drain()
                _ = await self.stream_continue.wait()
                rest = DATA[len(first) :]
                writer.write(f"{len(rest):x}\r\n".encode() + rest + b"\r\n")
                writer.write(
                    f"0\r\nX-Download-Hash: {hashlib.sha256(DATA).hexdigest()}\r\n\r\n".encode()
                )
            else:
                body, trailers = await read_body(reader, headers) if method == "POST" else (b"", {})
                response = body if method == "POST" else DATA if path == "large" else b"ready"
                trailer_hash = trailers.get("x-upload-hash", "")
                writer.write(
                    (
                        f"HTTP/1.1 200 OK\r\nContent-Length: {len(response)}\r\n"
                        f"X-Seen-Upload-Hash: {trailer_hash}\r\nConnection: close\r\n\r\n"
                    ).encode()
                    + response
                )
            await writer.drain()
        except ConnectionError, asyncio.IncompleteReadError:
            pass
        except Exception as error:
            self.errors.append(error)
        finally:
            await close_writer(writer)

    async def echo(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            first = await reader.readexactly(5)
            if first == b"HALF!":
                writer.write(DATA)
                await writer.drain()
                writer.write_eof()
                self.tunnel_upload = await reader.read()
                self.tunnel_done.set()
            else:
                writer.write(first)
                while data := await reader.read(65536):
                    writer.write(data)
                    await writer.drain()
        except ConnectionError, asyncio.IncompleteReadError:
            pass
        finally:
            await close_writer(writer)


@asynccontextmanager
async def proxy_process(
    binary: Path, directory: Path, args: list[str], use_database: bool = True
) -> AsyncIterator[asyncio.subprocess.Process]:
    directory.mkdir(parents=True, exist_ok=True)
    log_path = directory / "proxy.log"
    with log_path.open("wb") as log:
        process = await asyncio.create_subprocess_exec(
            binary,
            *args,
            *(["-D", str(directory)] if use_database else []),
            "-v",
            "3",
            stdout=log,
            stderr=log,
        )
        try:
            yield process
            assert process.returncode is None, f"proxy exited with {process.returncode}"
        except BaseException:
            print(
                f"{log_path.name} ({directory.name}):\n{log_path.read_text(errors='replace')[-8000:]}"
            )
            raise
        finally:
            if process.returncode is None:
                process.terminate()
                try:
                    _ = await asyncio.wait_for(process.wait(), 5)
                except TimeoutError:
                    process.kill()
                    _ = await process.wait()


async def request(
    port: int, host: str, method: str, path: str, body: bytes = b"", chunked: bool = False
) -> tuple[int, dict[str, str], bytes]:
    async with asyncio.timeout(30):
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        try:
            framing = (
                "Transfer-Encoding: chunked\r\nTrailer: X-Upload-Hash\r\n"
                if chunked
                else f"Content-Length: {len(body)}\r\n"
            )
            writer.write(
                (
                    f"{method} http://{host}/{path} HTTP/1.1\r\nHost: {host}\r\n"
                    f"{framing}Connection: close\r\n\r\n"
                ).encode()
            )
            writer.write(chunked_body(body, hashlib.sha256(body).hexdigest()) if chunked else body)
            await writer.drain()
            status, headers = await read_headers(reader)
            response, _ = await read_body(reader, headers)
            return int(status.split(" ")[1]), headers, response
        finally:
            await close_writer(writer)


async def check_stream(port: int, host: str, backend: Backend) -> None:
    async with asyncio.timeout(30):
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        try:
            writer.write(
                f"GET http://{host}/stream HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode()
            )
            await writer.drain()
            status, headers = await read_headers(reader)
            assert status.split(" ")[1] == "200"
            assert headers.get("transfer-encoding", "").lower() == "chunked"
            size = int(await reader.readline(), 16)
            assert size > 0
            first = await reader.readexactly(size)
            assert await reader.readexactly(2) == b"\r\n"
            assert first == DATA[:size]
            backend.stream_continue.set()
            rest, trailers = await read_chunks(reader)
            assert first + rest == DATA
            assert trailers["x-download-hash"] == hashlib.sha256(DATA).hexdigest()
        finally:
            backend.stream_continue.set()
            await close_writer(writer)


async def check_connect(port: int, host: str) -> None:
    async with asyncio.timeout(30):
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        try:
            writer.write(f"CONNECT {host}:443 HTTP/1.1\r\nHost: {host}:443\r\n\r\n".encode())
            await writer.drain()
            status, _ = await read_headers(reader)
            assert status.split(" ")[1] == "200"
            for data in (b"first tunnel message", DATA[: 256 * 1024], b"last tunnel message"):
                writer.write(data)
                await writer.drain()
                assert await reader.readexactly(len(data)) == data
            writer.write_eof()
            assert await reader.read() == b""
        finally:
            await close_writer(writer)


async def check_server_half_close(port: int, host: str, backend: Backend) -> None:
    async with asyncio.timeout(30):
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        try:
            writer.write(f"CONNECT {host}:443 HTTP/1.1\r\nHost: {host}:443\r\n\r\n".encode())
            await writer.drain()
            status, _ = await read_headers(reader)
            assert status.split()[1] == "200"
            writer.write(b"HALF!")
            await writer.drain()
            assert await reader.read() == DATA
            writer.write(DATA)
            await writer.drain()
            writer.write_eof()
            _ = await backend.tunnel_done.wait()
            assert backend.tunnel_upload == DATA
        finally:
            await close_writer(writer)


async def check_interrupted_upload(
    port: int, host: str, backend: Backend, close_timeout: float = 5
) -> None:
    async with asyncio.timeout(30):
        _, writer = await asyncio.open_connection("127.0.0.1", port)
        try:
            writer.write(
                (
                    f"POST http://{host}/interrupted HTTP/1.1\r\nHost: {host}\r\n"
                    f"Content-Length: {len(DATA)}\r\nConnection: close\r\n\r\n"
                ).encode()
                + DATA[: 256 * 1024]
            )
            await writer.drain()
            _ = await backend.upload_started.wait()
        finally:
            await close_writer(writer)
        async with asyncio.timeout(close_timeout):
            _ = await backend.upload_closed.wait()
        assert 0 < backend.interrupted_bytes < len(DATA)
        status, _, body = await request(port, host, "GET", "ready")
        assert status == 200 and body == b"ready"


async def check_early_response(port: int, host: str, backend: Backend, plain: bool = False) -> None:
    paths = (
        (
            "early-implicit",
            "early-keep",
            "early-eof",
            "plain-close",
            "plain-token-close",
            "plain-reject",
        )
        if plain
        else (
            "early-implicit",
            "early-keep",
            "early-eof",
            "early-close",
            "early-proxy-close",
            "early-reject",
        )
    )
    for path in paths:
        print(f"Checking {path}", flush=True)
        async with asyncio.timeout(15):
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            try:
                backend.early_done.clear()
                writer.write(
                    (
                        f"POST http://{host}/{path} HTTP/1.1\r\nHost: {host}\r\n"
                        f"Content-Length: {len(DATA)}\r\nConnection: keep-alive\r\n\r\n"
                    ).encode()
                    + DATA[:65536]
                )
                await writer.drain()
                status, headers = await read_headers(reader)
                body, _ = await read_body(reader, headers)
                assert int(status.split()[1]) == (
                    413 if path in ("early-reject", "plain-reject") else 200
                )
                assert body == (
                    DATA[: 256 * 1024] if path in ("early-reject", "plain-reject") else b"accepted"
                )
                if path in ("early-implicit", "early-keep", "early-eof") or path.startswith(
                    "plain-"
                ):
                    writer.write(DATA[65536:])
                    await writer.drain()
                    _ = await backend.early_done.wait()
                    assert backend.early_uploads[path] == DATA
                    writer.write(
                        f"GET http://{host}/ready HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode()
                    )
                    await writer.drain()
                    following_status, following_headers = await read_headers(reader)
                    following_body, _ = await read_body(reader, following_headers)
                    assert following_status.split()[1] == "200" and following_body == b"ready"

                else:
                    assert await reader.read() == b""
            finally:
                await close_writer(writer)
        status, _, body = await request(port, host, "GET", "ready")
        assert status == 200 and body == b"ready"


async def check_plain_proxy(
    install: Install, directory: Path, backend: Backend, backend_port: int
) -> None:
    async with proxy_process(
        install.build_dir / "http/http-proxy",
        directory / "plain",
        ["-p", str(plain_port := unused_port())],
        use_database=False,
    ):
        plain_host = f"127.0.0.1:{backend_port}"
        async with asyncio.timeout(10):
            while True:
                try:
                    status, _, body = await request(plain_port, plain_host, "GET", "ready")
                    assert status == 200 and body == b"ready"
                    break
                except OSError:
                    await asyncio.sleep(0.1)
        for method in ("GET", "POST"):
            status, _, body = await request(
                plain_port,
                plain_host,
                method,
                "large",
                DATA if method == "POST" else b"",
            )
            assert status == 200 and body == DATA
        status, headers, body = await request(plain_port, plain_host, "POST", "echo", DATA, True)
        assert status == 200 and body == DATA
        assert headers["x-seen-upload-hash"] == hashlib.sha256(DATA).hexdigest()
        backend.stream_continue.clear()
        await check_stream(plain_port, plain_host, backend)
        await check_early_response(plain_port, plain_host, backend, plain=True)
        print(
            "Classic HTTP proxy: bodies, trailers, streaming and default upload policy passed",
            flush=True,
        )


async def main_http() -> None:
    repo = Path(__file__).resolve().parents[2]
    install = Install(repo / "build", repo)
    with TemporaryDirectory(prefix="ton-http-proxy-") as temporary:
        backend = Backend()
        server = await asyncio.start_server(backend.handle, "127.0.0.1", 0)
        async with server:
            port = cast(tuple[str, int], server.sockets[0].getsockname())[1]
            await check_plain_proxy(install, Path(temporary), backend, port)
            assert not backend.errors, backend.errors


async def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    install = Install(repo / "build", repo)
    binary = install.build_dir / "rldp-http-proxy/rldp-http-proxy"
    assert binary.is_file(), f"build {binary} before running this test"
    with TemporaryDirectory(prefix="ton-http-proxy-") as temporary:
        directory = Path(temporary)
        async with Network(
            install, directory / "network", port_base=unused_port(socket.SOCK_DGRAM) - 1
        ) as network:
            dht = network.create_dht_node()
            await dht.run()
            config = directory / "global.json"
            global_config = ton_api.Config_global(
                dht=ton_api.Dht_config_global(
                    k=3, a=3, static_nodes=ton_api.Dht_nodes(nodes=[dht.signed_address])
                ),
                validator=ton_api.Validator_config_global(
                    zero_state=ton_api.TonNode_blockIdExt(
                        workchain=-1,
                        shard=-(2**63),
                        seqno=0,
                        root_hash=b"\x01" * 32,
                        file_hash=b"\x02" * 32,
                    )
                ),
            ).to_dict()
            global_config["liteservers"] = [
                ton_api.Liteserver_desc(
                    id=Key().public_key, ip=0x7F000001, port=unused_port()
                ).to_dict()
            ]
            _ = config.write_text(json.dumps(global_config))
            backend = Backend()
            http_backend = await asyncio.start_server(backend.handle, "127.0.0.1", 0)
            echo_backend = await asyncio.start_server(backend.echo, "127.0.0.1", 0)
            async with http_backend, echo_backend, AsyncExitStack() as stack:
                backend_port = cast(tuple[str, int], http_backend.sockets[0].getsockname())[1]
                echo_port = cast(tuple[str, int], echo_backend.sockets[0].getsockname())[1]
                server_directory = directory / "server"
                keyring = server_directory / "keyring"
                keyring.mkdir(parents=True)
                key = Key()
                _ = key.add_to_keyring(keyring)
                address = adnl_address(key)
                host = address + ".adnl"
                server = await stack.enter_async_context(
                    proxy_process(
                        binary,
                        server_directory,
                        [
                            "-a",
                            f"127.0.0.1:{proxy_udp_port()}",
                            "-A",
                            address,
                            "-R",
                            f"{host}:80@127.0.0.1:{backend_port}",
                            "-R",
                            f"{host}:443@127.0.0.1:{echo_port}",
                            "-C",
                            str(config),
                        ],
                    )
                )
                client_port = unused_port()
                client = await stack.enter_async_context(
                    proxy_process(
                        binary,
                        directory / "client",
                        [
                            "-c",
                            str(unused_port(socket.SOCK_DGRAM)),
                            "-p",
                            str(client_port),
                            "-C",
                            str(config),
                        ],
                    )
                )
                async with asyncio.timeout(90):
                    while True:
                        assert server.returncode is None and client.returncode is None
                        try:
                            async with asyncio.timeout(4):
                                status, _, body = await request(client_port, host, "GET", "ready")
                            if status == 200 and body == b"ready":
                                break
                        except OSError, TimeoutError, asyncio.IncompleteReadError:
                            pass
                        await asyncio.sleep(0.2)
                status, _, body = await request(client_port, host, "GET", "large")
                assert status == 200 and body == DATA
                status, _, body = await request(client_port, host, "POST", "echo", DATA)
                assert status == 200 and body == DATA
                status, headers, body = await request(client_port, host, "POST", "echo", DATA, True)
                assert status == 200 and body == DATA
                assert headers["x-seen-upload-hash"] == hashlib.sha256(DATA).hexdigest()
                await check_stream(client_port, host, backend)
                await check_connect(client_port, host)
                await check_server_half_close(client_port, host, backend)
                await check_interrupted_upload(client_port, host, backend, 25)
                await check_early_response(client_port, host, backend, plain=False)
                await check_plain_proxy(install, directory, backend, backend_port)
                assert not backend.errors, backend.errors
                assert "got HTTP request over rldp" in (server_directory / "proxy.log").read_text(
                    errors="replace"
                )
    print(
        "RLDP2 HTTP proxy: multipart GET/POST, chunked trailers, streaming, CONNECT half-close, upload cancellation and early responses passed"
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    parser = argparse.ArgumentParser()
    _ = parser.add_argument("--http-only", action="store_true")
    args = parser.parse_args(namespace=Arguments())
    asyncio.run(asyncio.wait_for(main_http() if args.http_only else main(), timeout=5 * 60))
