# pyright: reportAny=false, reportUnknownArgumentType=false

import asyncio
import json
from unittest.mock import Mock

import pytest
from tonapi import ton_api
from tonlib.engine_console import LocalError, RemoteError

from tonlib import EngineConsoleClient

MOCK_LOOP_PTR = 12345
MOCK_CONSOLE_PTR = 67890
MOCK_RESPONSE_PTR = 11111
MOCK_CONTINUATION_ID = 42


@pytest.fixture
def mock_tonlib():
    tonlib = Mock()
    tonlib.engine_console_create = Mock(return_value=MOCK_CONSOLE_PTR)
    tonlib.engine_console_destroy = Mock()
    tonlib.engine_console_is_error = Mock(return_value=False)
    tonlib.engine_console_get_error_code = Mock()
    tonlib.engine_console_get_error_message = Mock()
    tonlib.engine_console_request = Mock(return_value=MOCK_RESPONSE_PTR)
    tonlib.response_destroy = Mock()
    tonlib.response_await_ready = Mock(return_value=True)
    tonlib.response_await_suspend = Mock()
    tonlib.response_is_error = Mock(return_value=False)
    tonlib.response_get_error_code = Mock()
    tonlib.response_get_error_message = Mock()
    tonlib.response_get_response = Mock(return_value=b'{"@type": "ok"}')
    return tonlib


@pytest.fixture
def mock_event_loop():
    event_loop = Mock()
    event_loop.loop = MOCK_LOOP_PTR
    event_loop.create_awaitable_future = Mock()
    return event_loop


@pytest.fixture
def config():
    return ton_api.EngineConsoleClient_config(address="127.0.0.1:1234")


@pytest.mark.asyncio
async def test_init_success(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    client = EngineConsoleClient(mock_tonlib, mock_event_loop, config)

    mock_tonlib.engine_console_create.assert_called_once_with(
        MOCK_LOOP_PTR, config.to_json().encode()
    )
    mock_tonlib.engine_console_is_error.assert_called_once_with(MOCK_CONSOLE_PTR)

    await client.aclose()

    mock_tonlib.engine_console_destroy.assert_called_once_with(MOCK_CONSOLE_PTR)


@pytest.mark.asyncio
async def test_init_error(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    mock_tonlib.engine_console_is_error = Mock(return_value=True)
    mock_tonlib.engine_console_get_error_code = Mock(return_value=500)
    mock_tonlib.engine_console_get_error_message = Mock(return_value=b"Connection failed")

    with pytest.raises(LocalError, match="Connection failed") as exc_info:
        _ = EngineConsoleClient(mock_tonlib, mock_event_loop, config)

    assert exc_info.value.code == 500
    assert exc_info.value.message == "Connection failed"

    mock_tonlib.engine_console_destroy.assert_called_once_with(MOCK_CONSOLE_PTR)


@pytest.mark.asyncio
async def test_context_manager(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    async with EngineConsoleClient(mock_tonlib, mock_event_loop, config):
        pass

    mock_tonlib.engine_console_destroy.assert_called_once_with(MOCK_CONSOLE_PTR)


@pytest.mark.asyncio
async def test_aclose_idempotent(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    client = EngineConsoleClient(mock_tonlib, mock_event_loop, config)

    await client.aclose()
    await client.aclose()

    mock_tonlib.engine_console_destroy.assert_called_once()


@pytest.mark.asyncio
async def test_request_synchronous(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    mock_tonlib.response_await_ready = Mock(return_value=True)
    response_data = {"@type": "engine.validator.success", "data": "test"}
    mock_tonlib.response_get_response = Mock(return_value=json.dumps(response_data).encode())

    async with EngineConsoleClient(mock_tonlib, mock_event_loop, config) as client:
        request = ton_api.Engine_validator_getConfigRequest()
        result = await client.request(request)

        mock_tonlib.engine_console_request.assert_called_once_with(
            MOCK_CONSOLE_PTR, request.to_json().encode()
        )

        mock_tonlib.response_await_ready.assert_called_once_with(MOCK_RESPONSE_PTR)
        mock_tonlib.response_await_suspend.assert_not_called()
        mock_tonlib.response_destroy.assert_called_once_with(MOCK_RESPONSE_PTR)

        assert result == response_data


@pytest.mark.asyncio
async def test_request_asynchronous(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    mock_tonlib.response_await_ready = Mock(return_value=False)
    response_data = {"@type": "engine.validator.success", "data": "test"}
    mock_tonlib.response_get_response = Mock(return_value=json.dumps(response_data).encode())

    future = asyncio.get_event_loop().create_future()
    future.set_result(None)
    mock_event_loop.create_awaitable_future = Mock(return_value=(MOCK_CONTINUATION_ID, future))

    async with EngineConsoleClient(mock_tonlib, mock_event_loop, config) as client:
        request = ton_api.Engine_validator_getConfigRequest()
        result = await client.request(request)

        mock_tonlib.response_await_suspend.assert_called_once_with(
            MOCK_RESPONSE_PTR, MOCK_CONTINUATION_ID
        )
        mock_tonlib.response_destroy.assert_called_once_with(MOCK_RESPONSE_PTR)

        assert result == response_data


@pytest.mark.asyncio
async def test_request_local_error(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    mock_tonlib.response_await_ready = Mock(return_value=True)
    mock_tonlib.response_is_error = Mock(return_value=True)
    mock_tonlib.response_get_error_code = Mock(return_value=404)
    mock_tonlib.response_get_error_message = Mock(return_value=b"Not found")

    async with EngineConsoleClient(mock_tonlib, mock_event_loop, config) as client:
        request = ton_api.Engine_validator_getConfigRequest()

        with pytest.raises(LocalError, match="Not found") as exc_info:
            _ = await client.request(request)

        assert exc_info.value.code == 404
        assert exc_info.value.message == "Not found"

        mock_tonlib.response_destroy.assert_called_once_with(MOCK_RESPONSE_PTR)


@pytest.mark.asyncio
async def test_request_remote_error(
    mock_tonlib: Mock, mock_event_loop: Mock, config: ton_api.EngineConsoleClient_config
):
    mock_tonlib.response_await_ready = Mock(return_value=True)
    error_response = {
        "@type": "engine.validator.controlQueryError",
        "code": 400,
        "message": "Invalid query",
    }
    mock_tonlib.response_get_response = Mock(return_value=json.dumps(error_response).encode())

    async with EngineConsoleClient(mock_tonlib, mock_event_loop, config) as client:
        request = ton_api.Engine_validator_getConfigRequest()

        with pytest.raises(RemoteError, match="Invalid query") as exc_info:
            _ = await client.request(request)

        assert exc_info.value.code == 400
        assert exc_info.value.message == "Invalid query"

        mock_tonlib.response_destroy.assert_called_once_with(MOCK_RESPONSE_PTR)
