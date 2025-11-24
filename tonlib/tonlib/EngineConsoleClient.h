#pragma once

#include "adnl/adnl-ext-client.h"
#include "keys/keys.hpp"
#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "td/utils/port/IPAddress.h"

namespace tonlib {

bool is_engine_console_query(const ton::tl_object_ptr<ton::ton_api::Function>& function);

class EngineConsoleClient : public td::actor::Actor {
 public:
  EngineConsoleClient(td::IPAddress address, ton::PublicKey server_public_key, ton::PrivateKey client_private_key);

  void on_ready();
  void on_stop_ready();

  td::actor::Task<ton::tl_object_ptr<ton::ton_api::Object>> query(ton::tl_object_ptr<ton::ton_api::Function> function);

 private:
  td::IPAddress address_;
  ton::PublicKey server_public_key_;
  ton::PrivateKey client_private_key_;
  td::actor::ActorOwn<ton::adnl::AdnlExtClient> client_;
  bool ready_ = false;
  std::vector<td::Promise<td::Unit>> pending_ready_promises_;
};

}  // namespace tonlib
