import json
import traceback
from typing import cast, final

from tonapi import ton_api

from tl import JSONSerializable, TLRequest

from .event_loop import TonlibEventLoop
from .tonlib_cdll import TonlibCDLL


class LocalError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code: int = code
        self.message: str = message


class RemoteError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code: int = code
        self.message: str = message


@final
class EngineConsoleClient:
    def __init__(
        self,
        tonlib: TonlibCDLL,
        event_loop: TonlibEventLoop,
        config: ton_api.EngineConsoleClient_config,
    ):
        self._tonlib = tonlib
        self._event_loop = event_loop
        config_json = config.to_json().encode()
        self._console = tonlib.engine_console_create(event_loop.loop, config_json)

        if tonlib.engine_console_is_error(self._console):
            error_code = tonlib.engine_console_get_error_code(self._console)
            error_message = tonlib.engine_console_get_error_message(self._console).decode()
            tonlib.engine_console_destroy(self._console)
            self._console = 0
            raise LocalError(error_code, error_message)

    def __del__(self):
        assert self._console == 0, (
            "EngineConsoleClient not destroyed. Call 'aclose' before destroying the object."
        )

    async def aclose(self) -> None:
        if self._console == 0:
            return

        self._tonlib.engine_console_destroy(self._console)
        self._console = 0

    async def __aenter__(self):
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: traceback.TracebackException | None,
    ):
        await self.aclose()

    async def request(self, request: TLRequest) -> JSONSerializable:
        response = self._tonlib.engine_console_request(self._console, request.to_json().encode())

        try:
            if not self._tonlib.response_await_ready(response):
                continuation_id, future = self._event_loop.create_awaitable_future()
                if continuation_id is not None:
                    self._tonlib.response_await_suspend(response, continuation_id)
                await future

            if self._tonlib.response_is_error(response):
                error_code = self._tonlib.response_get_error_code(response)
                error_message = self._tonlib.response_get_error_message(response).decode()
                raise LocalError(error_code, error_message)

            response_json = self._tonlib.response_get_response(response).decode()
            response_json = cast(JSONSerializable, json.loads(response_json))

            if (
                isinstance(response_json, dict)
                and response_json.get("@type", None) == "engine.validator.controlQueryError"
            ):
                error = ton_api.Engine_validator_controlQueryError.from_dict(response_json)
                raise RemoteError(error.code, error.message)

            return response_json
        finally:
            self._tonlib.response_destroy(response)

    async def get_actor_stats(self) -> str:
        query = ton_api.Engine_validator_getActorTextStatsRequest()
        return query.parse_result(await self.request(query)).data
