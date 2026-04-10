#include "auto/tl/ton_api_json.h"
#include "tl-utils/tl-utils.hpp"
#include "tl/tl_json.h"

#include "FFIAwaitable.h"
#include "FFIEngineConsoleClient.h"
#include "FFIEventLoop.h"
#include "tonlib_engine_console.h"

// ===== Event loop =====
TonlibEventLoop *tonlib_event_loop_create(int threads) {
  return new tonlib::FFIEventLoop{threads};
}

void tonlib_event_loop_destroy(TonlibEventLoop *loop) {
  delete loop;
}

void tonlib_event_loop_cancel(TonlibEventLoop *loop) {
  loop->cancel();
}

const void *tonlib_event_loop_wait(TonlibEventLoop *loop, double timeout) {
  auto result = loop->wait(timeout);
  if (!result.has_value()) {
    return nullptr;
  }
  return result->ptr();
}

// ===== Response =====
void tonlib_response_destroy(TonlibResponse *response) {
  response->destroy();
}

bool tonlib_response_await_ready(TonlibResponse *response) {
  return response->await_ready();
}

void tonlib_response_await_suspend(TonlibResponse *response, const void *continuation) {
  response->await_suspend({continuation});
}

bool tonlib_response_is_error(TonlibResponse *response) {
  return response->result().is_error();
}

int tonlib_response_get_error_code(TonlibResponse *response) {
  return response->result().error().code();
}

const char *tonlib_response_get_error_message(TonlibResponse *response) {
  return response->result().error().message().data();
}

const char *tonlib_response_get_response(TonlibResponse *response) {
  return response->result().ok().data();
}

// ===== Engine Console =====
namespace {

td::Result<tonlib::FFIEngineConsoleClient> create_ffi_client(TonlibEventLoop *loop, const char *config) {
  std::string config_str = config;
  TRY_RESULT(json, td::json_decode(config_str));
  if (json.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("Config must be a JSON object");
  }

  ton::ton_api::engineConsoleClient_config parsed_config;
  TRY_STATUS(from_json(parsed_config, json.get_object()));

  td::IPAddress parsed_address;
  TRY_STATUS(parsed_address.init_host_port(parsed_config.address_));

  if (!parsed_config.server_public_key_) {
    return td::Status::Error("server_public_key is required in config");
  }
  auto server_public_key_slice = ton::serialize_tl_object(parsed_config.server_public_key_.get(), true);
  TRY_RESULT(parsed_server_public_key, ton::PublicKey::import(server_public_key_slice));

  if (!parsed_config.client_private_key_) {
    return td::Status::Error("client_private_key is required in config");
  }
  auto client_private_key_slice = ton::serialize_tl_object(parsed_config.client_private_key_.get(), true);
  TRY_RESULT(parsed_client_private_key, ton::PrivateKey::import(client_private_key_slice));

  return tonlib::FFIEngineConsoleClient{*loop, parsed_address, parsed_server_public_key, parsed_client_private_key};
}

td::Result<ton::tl_object_ptr<ton::ton_api::Function>> parse_query(const char *query) {
  std::string query_str = query;
  TRY_RESULT(json, td::json_decode(query_str));
  if (json.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("Query must be a JSON object");
  }

  ton::tl_object_ptr<ton::ton_api::Function> parsed_query;
  TRY_STATUS(from_json(parsed_query, std::move(json)));

  if (!tonlib::is_engine_console_query(parsed_query)) {
    return td::Status::Error("Query is not an engine console query");
  }

  return parsed_query;
}

}  // namespace

struct TonlibEngineConsole {
  td::Result<tonlib::FFIEngineConsoleClient> client;
};

TonlibEngineConsole *tonlib_engine_console_create(TonlibEventLoop *loop, const char *config) {
  return new TonlibEngineConsole{create_ffi_client(loop, config)};
}

void tonlib_engine_console_destroy(TonlibEngineConsole *console) {
  delete console;
}

bool tonlib_engine_console_is_error(TonlibEngineConsole *console) {
  return console->client.is_error();
}

int tonlib_engine_console_get_error_code(TonlibEngineConsole *console) {
  return console->client.error().code();
}

const char *tonlib_engine_console_get_error_message(TonlibEngineConsole *console) {
  return console->client.error().message().data();
}

TonlibResponse *tonlib_engine_console_request(TonlibEngineConsole *console, const char *query) {
  auto &client = console->client.ok_ref();

  auto query_or_sync_error = parse_query(query);

  if (query_or_sync_error.is_error()) {
    return TonlibResponse::create_resolved(client.loop(), query_or_sync_error.move_as_error());
  }

  auto transform = [](ton::tl_object_ptr<ton::ton_api::Object> object) -> std::string {
    return td::json_encode<std::string>(td::ToJson(object));
  };

  auto [response, promise] =
      TonlibResponse::create_bridge<ton::tl_object_ptr<ton::ton_api::Object>>(client.loop(), transform);
  client.request(query_or_sync_error.move_as_ok(), std::move(promise));
  return response;
}
