#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tonlib/tonlibjson_export.h"

#ifdef __cplusplus
#include <string>

namespace tonlib {

class FFIEventLoop;
template <typename T>
class FFIAwaitable;

}  // namespace tonlib

using TonlibEventLoop = tonlib::FFIEventLoop;
using TonlibResponse = tonlib::FFIAwaitable<std::string>;
#else
typedef struct TonlibEventLoop TonlibEventLoop;
typedef struct TonlibResponse TonlibResponse;
#endif

typedef struct TonlibEngineConsole TonlibEngineConsole;

#ifdef __cplusplus
extern "C" {
#endif

// ===== Event Loop =====
// An interaction with the engine console client starts by creating an TonlibEventLoop object that
// allows foreign caller to wait for asynchronous event completion in a mostly non-blocking
// manner. In particular, after suspending TonlibResponse, tonlib_event_loop_wait will return when
// the awaitable TonlibResponse is resolved.
//
// tonlib_event_loop_wait does nothing for `timeout` seconds if there is nothing to process, so a
// typical interaction flow goes as follows:
//
// 1. Create an event loop before first library usage.
// 2. Spawn a background thread that continuously polls tonlib_event_loop_wait and resumes
//    continuations of resolved responses.
// 3. Call asynchronous functions, providing a continuation that the background thread knows how to
//    resume.
//
// There must be a total happens-before order in which tonlib functions (except
// `tonlib_event_loop_wait`) bound to a single event loop are called (i. e. the event loop is
// thread-aware but not thread-safe). `tonlib_event_loop_wait` calls must be happens-before ordered
// with respect to each other as well but are not required to be ordered with the respect to other
// functions. However, last call to `tonlib_event_loop_wait` must happen before
// `tonlib_event_loop_destroy` call. To facilitate this, `tonlib_event_loop_cancel` can be used to
// cancel wait without destroying the loop.

// Creates a new event loop instance. Never fails.
TONLIBJSON_EXPORT TonlibEventLoop *tonlib_event_loop_create(int threads);

// Destroys the event loop.
//
// Non-destroyed instances of engine console client will deadlock the function. (Calling
// `tonlib_engine_console_destroy` during `tonlib_event_loop_destroy` is UB as it violates the
// global happens-before ordering requirement.)
TONLIBJSON_EXPORT void tonlib_event_loop_destroy(TonlibEventLoop *loop);

// Puts event loop into the cancelled state.
TONLIBJSON_EXPORT void tonlib_event_loop_cancel(TonlibEventLoop *loop);

// Waits for the next event for `timeout` seconds. If no event happens within the timeout, returns
// nullptr. If the event loop is cancelled on function entry, returns immediately with nullptr. If
// the event loop is cancelled during wait, the function eventually (as soon as scheduled) returns
// nullptr as well. timeout=-1.0 is no timeout.
TONLIBJSON_EXPORT const void *tonlib_event_loop_wait(TonlibEventLoop *loop, double timeout);

// ===== Response =====
// TonlibResponse is an awaitable that will resolve with the response of the connected validator
// engine. It can be obtained from `tonlib_engine_console_request`.

// Destroys the response. If `await_suspend` was called on the response and response is not yet
// resolved, continuation will arrive as soon as scheduled.
TONLIBJSON_EXPORT void tonlib_response_destroy(TonlibResponse *response);

// Returns true if the response is resolved. You can use this immediately after creation to check if
// synchronous result is available.
TONLIBJSON_EXPORT bool tonlib_response_await_ready(TonlibResponse *response);

// Records continuation that will be returned by `tonlib_event_loop_wait` when response is resolved.
// `tonlib_event_loop_wait` will not return because of this response until this function is called.
//
// Can only be called one time on a particular response instance. Can be called on a resolved
// instance as well, in which case the continuation will be returned as soon as scheduled (this is
// allowed as `await_ready` + `await_suspend` sequence is obviously not atomic).
//
// uintptr_t(continuation) must be in [1, UINTPTR_MAX - 1] range.
TONLIBJSON_EXPORT void tonlib_response_await_suspend(TonlibResponse *response, const void *continuation);

// Returns true if the response is an error. Can only be called on resolved response. Only errors
// produced locally will be reported here; errors returned by the remote side are returned using a
// "success" path as an `engine.validator.controlQueryError` object.
TONLIBJSON_EXPORT bool tonlib_response_is_error(TonlibResponse *response);

// Returns the error code. Can only be called on resolved error response.
TONLIBJSON_EXPORT int tonlib_response_get_error_code(TonlibResponse *response);

// Returns the error message. Can only be called on resolved error response.
TONLIBJSON_EXPORT const char *tonlib_response_get_error_message(TonlibResponse *response);

// Returns the JSON-encoded remote TL response. Can only be called on resolved response. Might be
// either a successful response with type determined by the TL scheme or an
// `engine.validator.controlQueryError` object if remote has encountered an error.
TONLIBJSON_EXPORT const char *tonlib_response_get_response(TonlibResponse *response);

// ===== Engine Console =====
// TonlibEngineConsole represents an instance of the engine console client. It allows sending
// control queries to the connected validator engine.

// Creates a new engine console client instance.
//
// `config` should be a JSON-encoded `engineConsoleClient.config` object. If creation of the
// instance fails, the error can be obtained from `tonlib_engine_console_is_error` and related
// functions.
TONLIBJSON_EXPORT TonlibEngineConsole *tonlib_engine_console_create(TonlibEventLoop *loop, const char *config);

// Destroys the engine console client instance. Error instances must be destroyed as well.
TONLIBJSON_EXPORT void tonlib_engine_console_destroy(TonlibEngineConsole *console);

// Returns true if the engine console instance did not initialize properly.
TONLIBJSON_EXPORT bool tonlib_engine_console_is_error(TonlibEngineConsole *console);

// Returns the error code. Can only be called if `tonlib_engine_console_is_error` returned true.
TONLIBJSON_EXPORT int tonlib_engine_console_get_error_code(TonlibEngineConsole *console);

// Returns the error message. Can only be called if `tonlib_engine_console_is_error` returned true.
TONLIBJSON_EXPORT const char *tonlib_engine_console_get_error_message(TonlibEngineConsole *console);

// Sends a control query to the connected validator engine. Can only be called if
// `tonlib_engine_console_is_error` returned false.
//
// `query` must be a JSON-encoded control query object.
TONLIBJSON_EXPORT TonlibResponse *tonlib_engine_console_request(TonlibEngineConsole *console, const char *query);

#ifdef __cplusplus
}  // extern "C"
#endif
