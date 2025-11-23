#include "FFIEngineConsoleClient.h"

namespace tonlib {

namespace {

class ClientWrapper : public EngineConsoleClient {
 public:
  ClientWrapper(td::IPAddress address, ton::PublicKey server_public_key, ton::PrivateKey client_private_key,
                td::unique_ptr<td::Guard> actor_counter)
      : EngineConsoleClient(address, server_public_key, client_private_key), actor_counter_(std::move(actor_counter)) {
  }

 private:
  td::unique_ptr<td::Guard> actor_counter_;
};

}  // namespace

FFIEngineConsoleClient::FFIEngineConsoleClient(FFIEventLoop& loop, td::IPAddress address, ton::PublicKey public_key,
                                               ton::PrivateKey private_key)
    : loop_(loop) {
  loop_.run_in_context([&] {
    client_ = td::actor::create_actor<ClientWrapper>("EngineConsoleClient", address, public_key, private_key,
                                                     loop.new_actor());
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
