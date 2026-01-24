import ctypes
from pathlib import Path
from typing import Callable, cast, final


@final
class TonlibCDLL:
    def __init__(self, cdll_path: Path):
        tonlib = ctypes.CDLL(cdll_path)

        event_loop_create = tonlib.tonlib_event_loop_create
        event_loop_create.restype = ctypes.c_void_p
        event_loop_create.argtypes = [ctypes.c_int]
        self.event_loop_create = cast(Callable[[int], int], event_loop_create)

        event_loop_destroy = tonlib.tonlib_event_loop_destroy
        event_loop_destroy.restype = None
        event_loop_destroy.argtypes = [ctypes.c_void_p]
        self.event_loop_destroy = cast(Callable[[int], None], event_loop_destroy)

        event_loop_cancel = tonlib.tonlib_event_loop_cancel
        event_loop_cancel.restype = None
        event_loop_cancel.argtypes = [ctypes.c_void_p]
        self.event_loop_cancel = cast(Callable[[int], None], event_loop_cancel)

        event_loop_wait = tonlib.tonlib_event_loop_wait
        event_loop_wait.restype = ctypes.c_void_p
        event_loop_wait.argtypes = [ctypes.c_void_p, ctypes.c_double]
        self.event_loop_wait = cast(Callable[[int, float], int], event_loop_wait)

        engine_console_create = tonlib.tonlib_engine_console_create
        engine_console_create.restype = ctypes.c_void_p
        engine_console_create.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.engine_console_create = cast(Callable[[int, bytes], int], engine_console_create)

        engine_console_destroy = tonlib.tonlib_engine_console_destroy
        engine_console_destroy.restype = None
        engine_console_destroy.argtypes = [ctypes.c_void_p]
        self.engine_console_destroy = cast(Callable[[int], None], engine_console_destroy)

        engine_console_is_error = tonlib.tonlib_engine_console_is_error
        engine_console_is_error.restype = ctypes.c_bool
        engine_console_is_error.argtypes = [ctypes.c_void_p]
        self.engine_console_is_error = cast(Callable[[int], bool], engine_console_is_error)

        engine_console_get_error_code = tonlib.tonlib_engine_console_get_error_code
        engine_console_get_error_code.restype = ctypes.c_int
        engine_console_get_error_code.argtypes = [ctypes.c_void_p]
        self.engine_console_get_error_code = cast(
            Callable[[int], int], engine_console_get_error_code
        )

        engine_console_get_error_message = tonlib.tonlib_engine_console_get_error_message
        engine_console_get_error_message.restype = ctypes.c_char_p
        engine_console_get_error_message.argtypes = [ctypes.c_void_p]
        self.engine_console_get_error_message = cast(
            Callable[[int], bytes], engine_console_get_error_message
        )

        engine_console_request = tonlib.tonlib_engine_console_request
        engine_console_request.restype = ctypes.c_void_p
        engine_console_request.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.engine_console_request = cast(Callable[[int, bytes], int], engine_console_request)

        response_destroy = tonlib.tonlib_response_destroy
        response_destroy.restype = None
        response_destroy.argtypes = [ctypes.c_void_p]
        self.response_destroy = cast(Callable[[int], None], response_destroy)

        response_await_ready = tonlib.tonlib_response_await_ready
        response_await_ready.restype = ctypes.c_bool
        response_await_ready.argtypes = [ctypes.c_void_p]
        self.response_await_ready = cast(Callable[[int], bool], response_await_ready)

        response_await_suspend = tonlib.tonlib_response_await_suspend
        response_await_suspend.restype = None
        response_await_suspend.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.response_await_suspend = cast(Callable[[int, int], None], response_await_suspend)

        response_is_error = tonlib.tonlib_response_is_error
        response_is_error.restype = ctypes.c_bool
        response_is_error.argtypes = [ctypes.c_void_p]
        self.response_is_error = cast(Callable[[int], bool], response_is_error)

        response_get_error_code = tonlib.tonlib_response_get_error_code
        response_get_error_code.restype = ctypes.c_int
        response_get_error_code.argtypes = [ctypes.c_void_p]
        self.response_get_error_code = cast(Callable[[int], int], response_get_error_code)

        response_get_error_message = tonlib.tonlib_response_get_error_message
        response_get_error_message.restype = ctypes.c_char_p
        response_get_error_message.argtypes = [ctypes.c_void_p]
        self.response_get_error_message = cast(Callable[[int], bytes], response_get_error_message)

        response_get_response = tonlib.tonlib_response_get_response
        response_get_response.restype = ctypes.c_char_p
        response_get_response.argtypes = [ctypes.c_void_p]
        self.response_get_response = cast(Callable[[int], bytes], response_get_response)
