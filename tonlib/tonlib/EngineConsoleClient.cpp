#include "adnl/adnl-ext-client.h"
#include "auto/tl/ton_api.hpp"

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

class EngineConsoleClientCallback : public ton::adnl::AdnlExtClient::Callback {
 public:
  EngineConsoleClientCallback(td::actor::ActorId<EngineConsoleClient> id) : id_(std::move(id)) {
  }

  void on_ready() override {
    td::actor::send_closure(id_, &EngineConsoleClient::on_ready);
  }

  void on_stop_ready() override {
    td::actor::send_closure(id_, &EngineConsoleClient::on_stop_ready);
  }

 private:
  td::actor::ActorId<EngineConsoleClient> id_;
};

EngineConsoleClient::EngineConsoleClient(td::IPAddress address, ton::PublicKey server_public_key,
                                         ton::PrivateKey client_private_key)
    : address_(address)
    , server_public_key_(std::move(server_public_key))
    , client_private_key_(std::move(client_private_key)) {
}

void EngineConsoleClient::on_ready() {
  ready_ = true;
  for (auto& promise : pending_ready_promises_) {
    promise.set_value(td::Unit());
  }
  pending_ready_promises_.clear();
}

void EngineConsoleClient::on_stop_ready() {
  for (auto& promise : pending_ready_promises_) {
    promise.set_error(td::Status::Error("Connection closed"));
  }
  pending_ready_promises_.clear();
  ready_ = false;
  client_ = {};
}

td::actor::Task<ton::tl_object_ptr<ton::ton_api::Object>> EngineConsoleClient::query(
    ton::tl_object_ptr<ton::ton_api::Function> object) {
  if (!ready_) {
    if (client_.empty()) {
      client_ =
          ton::adnl::AdnlExtClient::create(ton::adnl::AdnlNodeIdFull{server_public_key_}, client_private_key_, address_,
                                           std::make_unique<EngineConsoleClientCallback>(actor_id(this)));
    }

    auto [ready_awaiter, ready_promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    pending_ready_promises_.push_back(std::move(ready_promise));
    co_await std::move(ready_awaiter);
  }

  auto query_bytes = ton::serialize_tl_object(object, true);
  auto wrapped_query = ton::serialize_tl_object(
      ton::create_tl_object<ton::ton_api::engine_validator_controlQuery>(std::move(query_bytes)), true);

  auto [response_awaiter, response_promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::send_query, "query", std::move(wrapped_query),
                          td::Timestamp::in(10.0), std::move(response_promise));
  auto response = co_await std::move(response_awaiter);

  auto result_obj = co_await ton::fetch_tl_object<ton::ton_api::Object>(response, true);
  co_return std::move(result_obj);
}

}  // namespace tonlib
