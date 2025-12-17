import asyncio
import ctypes
import json
import logging
import typing
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path
from typing import override

from tontester.tl import tonlib_api

from tl import JSONSerializable, TLObject

logger = logging.getLogger(__name__)


class TonlibError(Exception):
    def __init__(self, result: tonlib_api.Error):
        super().__init__()
        self.result: tonlib_api.Error = result

    @property
    def code(self):
        return self.result.code

    @override
    def __str__(self):
        return self.result.message


def _parse_tonlib_error(result: dict[str, JSONSerializable]):
    if result.get("@type") == "error":
        er = tonlib_api.Error.from_dict(result)
        return TonlibError(er)
    return None


@dataclass
class _Payload[T]:
    value: T


class _Status(Enum):
    NONE = auto()
    FINISHED = auto()
    CRASHED = auto()


class TonLib:
    def __init__(
        self,
        loop: asyncio.AbstractEventLoop,
        ls_index: int,
        cdll_path: Path,
        tonlib_cancel_check_interval: int,
        verbosity_level: int = 0,
    ):
        tonlib = ctypes.CDLL(cdll_path)

        tonlib_client_set_verbosity_level = tonlib.tonlib_client_set_verbosity_level
        tonlib_client_set_verbosity_level.restype = None
        tonlib_client_set_verbosity_level.argtypes = [ctypes.c_int]

        tonlib_client_set_verbosity_level(verbosity_level)

        tonlib_json_client_create = tonlib.tonlib_client_json_create
        tonlib_json_client_create.restype = ctypes.c_void_p
        tonlib_json_client_create.argtypes = []
        self._client: int = tonlib_json_client_create()

        tonlib_json_client_receive = tonlib.tonlib_client_json_receive
        tonlib_json_client_receive.restype = ctypes.c_char_p
        tonlib_json_client_receive.argtypes = [ctypes.c_void_p, ctypes.c_double]
        self._tonlib_json_client_receive: typing.Callable[[int, float], bytes | None] = (
            tonlib_json_client_receive
        )

        tonlib_json_client_send = tonlib.tonlib_client_json_send
        tonlib_json_client_send.restype = None
        tonlib_json_client_send.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self._tonlib_json_client_send: typing.Callable[[int, bytes], None] = tonlib_json_client_send

        tonlib_json_client_destroy = tonlib.tonlib_client_json_destroy
        tonlib_json_client_destroy.restype = None
        tonlib_json_client_destroy.argtypes = [ctypes.c_void_p]
        self._tonlib_json_client_destroy: typing.Callable[[int], None] = tonlib_json_client_destroy

        # FIXME: This only exists because we cannot cancel running `tonlib_json_client_receive`.
        self._tonlib_cancel_check_interval: int = tonlib_cancel_check_interval

        self._request_id: int = 0
        self._futures: dict[str, asyncio.Future[JSONSerializable]] = {}
        self._loop: asyncio.AbstractEventLoop = loop
        self._ls_index: int = ls_index
        self._state: _Status = _Status.NONE

        self._work_notification: asyncio.Event = asyncio.Event()
        self._read_results_task: asyncio.Task[None] = self._loop.create_task(self._read_results())

    def __del__(self):
        assert self._client == 0, (
            "TonLib client not destroyed. Call 'aclose' before destroying the object."
        )

    def _send(self, query: JSONSerializable):
        assert self._is_working
        q = json.dumps(query).encode("utf-8")
        self._tonlib_json_client_send(self._client, q)

    def _next_request_id(self):
        result = str(self._request_id)
        self._request_id += 1
        return result

    async def execute(self, query: TLObject) -> JSONSerializable:
        assert self._is_working, f"TonLib failed with state: {self._state}"

        request_id = self._next_request_id()
        query_d = query.to_dict()
        query_d["@extra"] = request_id

        future: asyncio.Future[JSONSerializable] = self._loop.create_future()
        future.add_done_callback(lambda _: self._futures.pop(request_id, None))
        self._futures[request_id] = future

        self._send(query_d)
        self._work_notification.set()

        result = await future
        return result

    @property
    def _is_working(self):
        return self._state == _Status.NONE

    def _receive(self) -> _Payload[JSONSerializable] | None:
        result = self._tonlib_json_client_receive(self._client, self._tonlib_cancel_check_interval)
        if result is None:
            return None
        return _Payload(typing.cast(JSONSerializable, json.loads(result.decode("utf-8"))))

    async def aclose(self):
        try:
            self._state = _Status.FINISHED
            self._work_notification.set()
            await self._read_results_task
            for f in self._futures.values():
                if not f.done():
                    f.set_exception(asyncio.CancelledError())
        finally:
            self._tonlib_json_client_destroy(self._client)
            self._client = 0

    async def _read_results(self):
        try:
            while True:
                # If there are no requests in flight, wait for new work.
                if not len(self._futures):
                    _ = await self._work_notification.wait()
                self._work_notification.clear()

                # We might be woken up because we are cancelled.
                if not self._is_working:
                    break

                result = await self._loop.run_in_executor(None, self._receive)
                # Read has timed out. Recheck the state.
                if result is None:
                    continue

                result = result.value
                assert isinstance(result, dict)
                assert isinstance(result["@extra"], str)
                request_id = result["@extra"]
                future = self._futures.get(request_id, None)

                if future is None or future.done():
                    continue

                try:
                    tonlib_error = _parse_tonlib_error(result)
                    if tonlib_error is not None:
                        future.set_exception(tonlib_error)
                    else:
                        future.set_result(result)
                    _ = self._futures.pop(request_id, None)
                except Exception as e:
                    future.set_exception(e)
        except Exception as e:
            logger.error("TonLib background task failed", exc_info=True)
            self._state = _Status.CRASHED
            for f in self._futures.values():
                if not f.done():
                    f.set_exception(e)
