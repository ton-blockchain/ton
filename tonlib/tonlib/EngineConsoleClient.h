#pragma once

#include "keys/keys.hpp"
#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "td/utils/port/IPAddress.h"

namespace tonlib {

bool is_engine_console_query(const ton::tl_object_ptr<ton::ton_api::Function>& function);

class EngineConsoleClient : public td::actor::Actor {
 public:
  EngineConsoleClient(td::IPAddress address, ton::PublicKey server_public_key, ton::PrivateKey client_private_key);

  td::actor::Task<ton::tl_object_ptr<ton::ton_api::Object>> query(ton::tl_object_ptr<ton::ton_api::Function> function);
};

}  // namespace tonlib
