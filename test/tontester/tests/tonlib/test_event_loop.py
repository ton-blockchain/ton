# pyright: reportAny=false, reportUnknownArgumentType=false

import asyncio
import threading
from unittest.mock import Mock

import pytest

from tonlib import TonlibEventLoop

MOCK_LOOP_PTR = 12345


@pytest.fixture
def mock_tonlib():
    cancel_event = threading.Event()

    def mock_wait(*_):
        _ = cancel_event.wait()
        return 0

    tonlib = Mock()
    tonlib.event_loop_create = Mock(return_value=MOCK_LOOP_PTR)
    tonlib.event_loop_destroy = Mock()
    tonlib.event_loop_cancel = Mock(side_effect=lambda _: cancel_event.set())  # pyright: ignore[reportUnknownLambdaType]
    tonlib.event_loop_wait = Mock(side_effect=mock_wait)
    return tonlib


@pytest.mark.asyncio
async def test_basic_lifecycle(mock_tonlib: Mock):
    loop = TonlibEventLoop(mock_tonlib, None, threads=2)

    mock_tonlib.event_loop_create.assert_called_once_with(2)

    _, future1 = loop.create_awaitable_future()
    _, future2 = loop.create_awaitable_future()

    await loop.aclose()

    mock_tonlib.event_loop_cancel.assert_called_once_with(MOCK_LOOP_PTR)
    mock_tonlib.event_loop_destroy.assert_called_once_with(MOCK_LOOP_PTR)

    with pytest.raises(asyncio.CancelledError):
        await future1
    with pytest.raises(asyncio.CancelledError):
        await future2

    await loop.aclose()
    mock_tonlib.event_loop_destroy.assert_called_once()


@pytest.mark.asyncio
async def test_context_manager(mock_tonlib: Mock):
    async with TonlibEventLoop(mock_tonlib) as loop:
        assert loop.loop == MOCK_LOOP_PTR

    mock_tonlib.event_loop_destroy.assert_called_once()


@pytest.mark.asyncio
async def test_create_awaitable_future_unique_ids(mock_tonlib: Mock):
    async with TonlibEventLoop(mock_tonlib) as loop:
        cont_id1, _ = loop.create_awaitable_future()
        cont_id2, _ = loop.create_awaitable_future()
        cont_id3, _ = loop.create_awaitable_future()

        assert len(set([cont_id1, cont_id2, cont_id3])) == 3


@pytest.mark.asyncio
async def test_poll_loop_resolves_futures(mock_tonlib: Mock):
    first_wait = True
    continuation_id = None
    future_created = threading.Event()
    cancel_event = threading.Event()

    def mock_wait(*_):
        nonlocal first_wait

        if first_wait:
            _ = future_created.wait()
            assert continuation_id is not None
            return continuation_id
        else:
            _ = cancel_event.wait()
            return 0

    def mock_cancel(_):
        cancel_event.set()

    mock_tonlib.event_loop_wait = Mock(side_effect=mock_wait)
    mock_tonlib.event_loop_cancel = Mock(side_effect=mock_cancel)

    async with TonlibEventLoop(mock_tonlib) as loop:
        continuation_id, future = loop.create_awaitable_future()
        future_created.set()
        await future


@pytest.mark.asyncio
async def test_poll_loop_handles_missing_continuation(mock_tonlib: Mock):
    first_wait = True
    wait_called = threading.Event()
    cancel_event = threading.Event()

    def mock_wait(*_):
        nonlocal first_wait

        if first_wait:
            first_wait = False
            wait_called.set()
            return 999
        else:
            _ = cancel_event.wait()
            return 0

    def mock_cancel(_):
        cancel_event.set()

    mock_tonlib.event_loop_wait = Mock(side_effect=mock_wait)
    mock_tonlib.event_loop_cancel = Mock(side_effect=mock_cancel)

    async with TonlibEventLoop(mock_tonlib):
        _ = wait_called.wait()


@pytest.mark.asyncio
async def test_poll_loop_exception_fails_all_futures(mock_tonlib: Mock):
    test_error = RuntimeError("Test error")
    futures_created = threading.Event()

    def mock_wait_error(*_):
        _ = futures_created.wait()
        raise test_error

    mock_tonlib.event_loop_wait = Mock(side_effect=mock_wait_error)
    mock_tonlib.event_loop_cancel = Mock()

    async with TonlibEventLoop(mock_tonlib) as loop:
        cont1, future1 = loop.create_awaitable_future()
        cont2, future2 = loop.create_awaitable_future()
        cont3, future3 = loop.create_awaitable_future()
        assert None not in [cont1, cont2, cont3]

        futures_created.set()

        with pytest.raises(RuntimeError, match="Test error"):
            await future1
        with pytest.raises(RuntimeError, match="Test error"):
            await future2
        with pytest.raises(RuntimeError, match="Test error"):
            await future3

        mock_tonlib.event_loop_wait.assert_called_once()

        cont4, future = loop.create_awaitable_future()
        assert cont4 is None

        assert future.done()
        with pytest.raises(RuntimeError, match="Test error"):
            await future
