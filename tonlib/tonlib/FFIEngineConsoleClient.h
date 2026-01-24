#pragma once

#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "td/utils/port/IPAddress.h"

#include "EngineConsoleClient.h"
#include "FFIEventLoop.h"

namespace tonlib {

class FFIEngineConsoleClient {
 public:
  FFIEngineConsoleClient(FFIEventLoop& loop, td::IPAddress address, ton::PublicKey server_public_key,
                         ton::PrivateKey client_private_key);

  FFIEngineConsoleClient(FFIEngineConsoleClient&&) = default;

  ~FFIEngineConsoleClient() {
    if (!client_.empty()) {
      loop_.run_in_context([client = std::move(client_)]() mutable { client.reset(); });
    }
  }

  void request(ton::tl_object_ptr<ton::ton_api::Function> query,
               td::Promise<ton::tl_object_ptr<ton::ton_api::Object>> promise);

  FFIEventLoop& loop() {
    return loop_;
  }

 private:
  FFIEventLoop& loop_;
  td::actor::ActorOwn<EngineConsoleClient> client_;
};

}  // namespace tonlib
