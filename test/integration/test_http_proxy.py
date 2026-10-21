import argparse
import asyncio
import hashlib
import logging
import socket
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import cast, final

from tontester.install import Install

DATA = (bytes(range(251)) * 13000)[: 3 * 1024 * 1024 + 17]


def unused_port(kind: socket.SocketKind = socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return cast(tuple[str, int], sock.getsockname())[1]


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
                    "early-close": "Connection: keep-alive, ClOsE\r\n",
                    "early-proxy-close": "Proxy-Connection: keep-alive, ClOsE\r\n",
                    "early-reject": "Connection: close\r\n",
                    "plain-reject": "Connection: close\r\n",
                    "plain-close": "Connection: close\r\n",
                    "plain-token-close": "Connection: keep-alive, ClOsE\r\n",
                }[path]
                code = 413 if path in ("early-reject", "plain-reject") else 200
                response = DATA[: 256 * 1024] if code == 413 else b"accepted"
                writer.write(
                    (
                        f"HTTP/1.1 {code} Test\r\nContent-Length: {len(response)}\r\n"
                        f"{connection}\r\n"
                    ).encode()
                    + response
                )
                await writer.drain()
                if path in ("early-implicit", "early-keep") or path.startswith("plain-"):
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


async def check_early_response(port: int, host: str, backend: Backend, plain: bool = False) -> None:
    paths = (
        ("early-implicit", "early-keep", "plain-close", "plain-token-close", "plain-reject")
        if plain
        else ("early-implicit", "early-keep", "early-close", "early-proxy-close", "early-reject")
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
                if path in ("early-implicit", "early-keep") or path.startswith("plain-"):
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


if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING)
    parser = argparse.ArgumentParser()
    _ = parser.add_argument("--http-only", action="store_true")
    _ = parser.parse_args()
    asyncio.run(asyncio.wait_for(main_http(), timeout=2 * 60))
