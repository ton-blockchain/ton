import asyncio
from pathlib import Path
from types import SimpleNamespace
from typing import override

import pytest
import tontester.network as network_module
from tontester.network import Network, StartOptions


class _TestNode(Network.Node):
    @override
    async def run(self, options: StartOptions | None = None) -> None:
        pass


@pytest.mark.asyncio
async def test_node_stop_escalates_from_term_to_kill(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_network = SimpleNamespace(_directory=tmp_path, _node_idx=0, _port=2000)
    node = _TestNode(fake_network, "stubborn")  # pyright: ignore[reportArgumentType]

    class StubbornProcess:
        def __init__(self) -> None:
            self.pid = 1234
            self.returncode: int | None = None
            self.terminated = False
            self.killed = False
            self._finished = asyncio.Event()

        async def wait(self) -> int:
            await self._finished.wait()
            self.returncode = -9
            return -9

        def terminate(self) -> None:
            self.terminated = True

        def kill(self) -> None:
            self.killed = True
            self._finished.set()

    class LogStreamer:
        def __init__(self) -> None:
            self.closed = False

        async def aclose(self) -> None:
            self.closed = True

    process = StubbornProcess()
    streamer = LogStreamer()
    watcher = asyncio.create_task(process.wait())
    node._Node__process = process  # pyright: ignore[reportAttributeAccessIssue]
    node._Node__process_watcher = watcher  # pyright: ignore[reportAttributeAccessIssue]
    node._Node__log_streamer = streamer  # pyright: ignore[reportAttributeAccessIssue]
    monkeypatch.setattr(network_module, "_NODE_TERM_TIMEOUT_S", 0.001)
    monkeypatch.setattr(
        network_module.os,
        "killpg",
        lambda pid, sent_signal: (
            process.terminate() if sent_signal == network_module.signal.SIGTERM else process.kill()
        ),
    )

    await node.stop()

    assert process.terminated
    assert process.killed
    assert streamer.closed


@pytest.mark.asyncio
async def test_node_stop_handles_cancellation_immediately_after_spawn(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_network = SimpleNamespace(
        _directory=tmp_path,
        _node_idx=0,
        _port=2000,
        _status=network_module._Status.INITED,
    )
    node = _TestNode(fake_network, "partial")  # pyright: ignore[reportArgumentType]

    class PartialProcess:
        def __init__(self) -> None:
            self.pid = 1234
            self.returncode: int | None = None
            self.stderr = object()
            self._finished = asyncio.Event()

        async def wait(self) -> int:
            await self._finished.wait()
            assert self.returncode is not None
            return self.returncode

    process = PartialProcess()

    spawned = asyncio.Event()

    async def create_subprocess_exec(*_: object, **kwargs: object) -> PartialProcess:
        assert kwargs["start_new_session"] is True
        spawned.set()
        return process

    def killpg(_: int, sent_signal: int) -> None:
        assert sent_signal == network_module.signal.SIGTERM
        process.returncode = -sent_signal
        process._finished.set()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create_subprocess_exec)
    monkeypatch.setattr(network_module.os, "killpg", killpg)

    run_task = asyncio.create_task(
        node._run(
            Path("fake"),
            SimpleNamespace(to_json=lambda: "{}"),  # pyright: ignore[reportArgumentType]
            None,
            StartOptions(),
        )
    )
    await spawned.wait()
    run_task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await run_task
    await node.stop()
    assert process.returncode == -network_module.signal.SIGTERM


@pytest.mark.asyncio
async def test_network_closes_every_node_when_one_stop_fails() -> None:
    calls: list[str] = []

    class FakeNode:
        def __init__(self, name: str, *, fail: bool = False) -> None:
            self.name = name
            self.fail = fail

        async def stop(self) -> None:
            calls.append(self.name)
            await asyncio.sleep(0)
            if self.fail:
                raise RuntimeError(self.name)

    class FakeEventLoop:
        def __init__(self) -> None:
            self.closed = False

        def close(self) -> None:
            self.closed = True

    network = object.__new__(Network)
    network._status = network_module._Status.INITED
    network._Network__nodes = [  # pyright: ignore[reportAttributeAccessIssue]
        FakeNode("failed", fail=True),
        FakeNode("healthy"),
    ]
    event_loop = FakeEventLoop()
    network._event_loop = event_loop  # pyright: ignore[reportAttributeAccessIssue]
    network._Network__close_task = None  # pyright: ignore[reportAttributeAccessIssue]

    with pytest.raises(BaseExceptionGroup, match="Failed to close network"):
        await network.aclose()

    assert calls == ["failed", "healthy"]
    assert event_loop.closed


@pytest.mark.asyncio
async def test_concurrent_network_close_callers_join_same_cleanup() -> None:
    started = asyncio.Event()
    finish = asyncio.Event()
    calls = 0

    class FakeNode:
        async def stop(self) -> None:
            nonlocal calls
            calls += 1
            started.set()
            await finish.wait()

    class FakeEventLoop:
        def close(self) -> None:
            pass

    network = object.__new__(Network)
    network._status = network_module._Status.INITED
    network._Network__nodes = [FakeNode()]  # pyright: ignore[reportAttributeAccessIssue]
    network._event_loop = FakeEventLoop()  # pyright: ignore[reportAttributeAccessIssue]
    network._Network__close_task = None  # pyright: ignore[reportAttributeAccessIssue]

    first = asyncio.create_task(network.aclose())
    await started.wait()
    first.cancel()
    with pytest.raises(asyncio.CancelledError):
        await first

    second = asyncio.create_task(network.aclose())
    await asyncio.sleep(0)
    assert not second.done()
    finish.set()
    await second
    assert calls == 1


@pytest.mark.asyncio
async def test_context_exit_holds_until_shared_cleanup_finishes_when_cancelled() -> None:
    started = asyncio.Event()
    finish = asyncio.Event()

    class FakeNode:
        async def stop(self) -> None:
            started.set()
            await finish.wait()

    class FakeEventLoop:
        def close(self) -> None:
            pass

    network = object.__new__(Network)
    network._status = network_module._Status.INITED
    network._Network__nodes = [FakeNode()]  # pyright: ignore[reportAttributeAccessIssue]
    network._event_loop = FakeEventLoop()  # pyright: ignore[reportAttributeAccessIssue]
    network._Network__close_task = None  # pyright: ignore[reportAttributeAccessIssue]

    exit_task = asyncio.create_task(network.__aexit__(None, None, None))
    await started.wait()
    exit_task.cancel()
    await asyncio.sleep(0)
    assert not exit_task.done()
    finish.set()
    with pytest.raises(asyncio.CancelledError):
        await exit_task
