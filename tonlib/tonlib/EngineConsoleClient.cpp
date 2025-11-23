#include "EngineConsoleClient.h"

namespace tonlib {

bool is_engine_console_query(const ton::tl_object_ptr<ton::ton_api::Function>& function) {
  switch (function->get_id()) {
    case ton::ton_api::engine_validator_getActorTextStats::ID:
      return true;
    default:
      return false;
  }
}

EngineConsoleClient::EngineConsoleClient(td::IPAddress address, ton::PublicKey server_public_key,
                                         ton::PrivateKey client_private_key) {
}

td::actor::Task<ton::tl_object_ptr<ton::ton_api::Object>> EngineConsoleClient::query(
    ton::tl_object_ptr<ton::ton_api::Function> object) {
  co_return td::Status::Error("Not implemented");
}

}  // namespace tonlib
