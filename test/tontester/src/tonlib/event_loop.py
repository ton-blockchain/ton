import asyncio
import logging
import threading
import traceback
from typing import final

from .tonlib_cdll import TonlibCDLL

logger = logging.getLogger(__name__)


@final
class TonlibEventLoop:
    def __init__(
        self,
        tonlib: TonlibCDLL,
        event_loop: asyncio.AbstractEventLoop | None = None,
        threads: int = 1,
    ):
        self._tonlib = tonlib
        self._loop = tonlib.event_loop_create(threads)
        self._next_continuation_id = 1
        self._futures: dict[int, asyncio.Future[None]] = {}
        self._py_event_loop = event_loop if event_loop is not None else asyncio.get_event_loop()
        self._cancelled = False
        self._background_error: Exception | None = None

        self._background_thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._background_thread.start()

    def __del__(self):
        assert self._loop == 0, (
            "EventLoop not destroyed. Call 'aclose' before destroying the object."
        )

    async def aclose(self) -> None:
        if self._loop == 0:
            return

        try:
            self._cancelled = True
            self._tonlib.event_loop_cancel(self._loop)
            self._background_thread.join()

            for future in self._futures.values():
                if not future.done():
                    future.set_exception(asyncio.CancelledError())
            self._futures.clear()
        finally:
            self._tonlib.event_loop_destroy(self._loop)
            self._loop = 0

    async def __aenter__(self):
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: traceback.TracebackException | None,
    ):
        await self.aclose()

    def _set_resolved(self, future: asyncio.Future[None]):
        if not future.done():
            future.set_result(None)

    def _set_exception(self, future: asyncio.Future[None], e: Exception):
        if not future.done():
            future.set_exception(e)

    def _cancel_all_futures(self, e: Exception):
        for future in self._futures.values():
            _ = self._py_event_loop.call_soon_threadsafe(self._set_exception, future, e)

    def _poll_loop(self) -> None:
        try:
            while not self._cancelled:
                continuation = self._tonlib.event_loop_wait(self._loop, 60.0)

                if continuation == 0:
                    continue

                future = self._futures.pop(continuation, None)
                if future is not None:
                    _ = self._py_event_loop.call_soon_threadsafe(self._set_resolved, future)
        except Exception as e:
            logger.error("EventLoop background task failed", exc_info=True)
            self._background_error = e
            _ = self._py_event_loop.call_soon_threadsafe(self._cancel_all_futures, e)

    def create_awaitable_future(self) -> tuple[int | None, asyncio.Future[None]]:
        if self._background_error is not None:
            future = self._py_event_loop.create_future()
            future.set_exception(self._background_error)
            return None, future

        continuation_id = self._next_continuation_id
        self._next_continuation_id += 1

        future = self._py_event_loop.create_future()
        future.add_done_callback(lambda _: self._futures.pop(continuation_id, None))
        self._futures[continuation_id] = future

        return continuation_id, future

    @property
    def loop(self):
        return self._loop
