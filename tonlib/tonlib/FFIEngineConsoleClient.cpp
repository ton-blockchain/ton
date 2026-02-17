#include "FFIEngineConsoleClient.h"

namespace tonlib {

FFIEngineConsoleClient::FFIEngineConsoleClient(FFIEventLoop& loop, td::IPAddress address,
                                               ton::PublicKey server_public_key, ton::PrivateKey client_private_key)
    : loop_(loop), counter_(loop.new_actor()) {
  loop_.run_in_context([&] {
    client_ = td::actor::create_actor<EngineConsoleClient>("EngineConsoleClient", address, server_public_key,
                                                           client_private_key);
  });
}

void FFIEngineConsoleClient::request(ton::tl_object_ptr<ton::ton_api::Function> query,
                                     td::Promise<ton::tl_object_ptr<ton::ton_api::Object>> promise) {
  loop_.run_in_context(
      [client = this->client_.get(), query = std::move(query), promise = std::move(promise)]() mutable {
        td::actor::send_closure(client, &EngineConsoleClient::query, std::move(query), std::move(promise));
      });
}

}  // namespace tonlib
