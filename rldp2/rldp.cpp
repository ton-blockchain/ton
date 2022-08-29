/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "rldp-in.hpp"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "td/utils/Random.h"
#include "fec/fec.h"
#include "RldpConnection.h"

namespace ton {

namespace rldp2 {

class RldpConnectionActor : public td::actor::Actor, private ConnectionCallback {
 public:
  RldpConnectionActor(td::actor::ActorId<RldpIn> rldp, adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                      td::actor::ActorId<adnl::Adnl> adnl)
      : rldp_(std::move(rldp)), src_(src), dst_(dst), adnl_(std::move(adnl)){};

  void send(TransferId transfer_id, td::BufferSlice query, td::Timestamp timeout = td::Timestamp::never()) {
    connection_.send(transfer_id, std::move(query), timeout);
    yield();
  }
  void set_receive_limits(TransferId transfer_id, td::Timestamp timeout, td::uint64 max_size) {
    connection_.set_receive_limits(transfer_id, timeout, max_size);
  }
  void receive_raw(td::BufferSlice data) {
    connection_.receive_raw(std::move(data));
    yield();
  }

 private:
  td::actor::ActorId<RldpIn> rldp_;
  adnl::AdnlNodeIdShort src_;
  adnl::AdnlNodeIdShort dst_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  RldpConnection connection_;

  void loop() override {
    alarm_timestamp() = connection_.run(*this);
  }

  void send_raw(td::BufferSlice data) override {
    send_closure(adnl_, &adnl::Adnl::send_message, src_, dst_, std::move(data));
  }
  void receive(TransferId transfer_id, td::Result<td::BufferSlice> data) override {
    send_closure(rldp_, &RldpIn::receive_message, dst_, src_, transfer_id, std::move(data));
  }
  void on_sent(TransferId transfer_id, td::Result<td::Unit> state) override {
    send_closure(rldp_, &RldpIn::on_sent, transfer_id, std::move(state));
  }
};

namespace {
TransferId get_random_transfer_id() {
  TransferId transfer_id;
  td::Random::secure_bytes(transfer_id.as_slice());
  return transfer_id;
}
TransferId get_responce_transfer_id(TransferId transfer_id) {
  return transfer_id ^ TransferId::ones();
}
}  // namespace

void RldpIn::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  return send_message_ex(src, dst, td::Timestamp::in(10.0), std::move(data));
}

void RldpIn::send_message_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                             td::BufferSlice data) {
  td::Bits256 id;
  td::Random::secure_bytes(id.as_slice());

  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_message>(id, std::move(data)), true);

  auto transfer_id = get_random_transfer_id();
  send_closure(create_connection(src, dst), &RldpConnectionActor::send, transfer_id, std::move(B), timeout);
}

void RldpIn::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                           td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                           td::uint64 max_answer_size) {
  auto query_id = adnl::AdnlQuery::random_query_id();

  auto date = static_cast<td::uint32>(timeout.at_unix()) + 1;
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_query>(query_id, max_answer_size, date, std::move(data)),
                               true);

  auto connection = create_connection(src, dst);
  auto transfer_id = get_random_transfer_id();
  auto response_transfer_id = get_responce_transfer_id(transfer_id);
  send_closure(connection, &RldpConnectionActor::set_receive_limits, response_transfer_id, timeout, max_answer_size);
  send_closure(connection, &RldpConnectionActor::send, transfer_id, std::move(B), timeout);

  queries_.emplace(response_transfer_id, std::move(promise));
}

void RldpIn::answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                          adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data) {
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_answer>(query_id, std::move(data)), true);

  send_closure(create_connection(src, dst), &RldpConnectionActor::send, transfer_id, std::move(B), timeout);
}

void RldpIn::receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data) {
  send_closure(create_connection(local_id, source), &RldpConnectionActor::receive_raw, std::move(data));
}

td::actor::ActorId<RldpConnectionActor> RldpIn::create_connection(adnl::AdnlNodeIdShort src,
                                                                  adnl::AdnlNodeIdShort dst) {
  auto it = connections_.find(std::make_pair(src, dst));
  if (it != connections_.end()) {
    return it->second.get();
  }
  auto connection = td::actor::create_actor<RldpConnectionActor>("RldpConnection", actor_id(this), src, dst, adnl_);
  auto res = connection.get();
  connections_[std::make_pair(src, dst)] = std::move(connection);
  return res;
}

void RldpIn::receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             td::Result<td::BufferSlice> r_data) {
  if (r_data.is_error()) {
    auto it = queries_.find(transfer_id);
    if (it != queries_.end()) {
      it->second.set_error(r_data.move_as_error());
      queries_.erase(it);
    } else {
      VLOG(RLDP_INFO) << "received error to unknown transfer_id " << transfer_id << " " << r_data.error();
    }
    return;
  }

  auto data = r_data.move_as_ok();
  //LOG(ERROR) << "RECEIVE MESSAGE " << data.size();
  auto F = fetch_tl_object<ton_api::rldp_Message>(std::move(data), true);
  if (F.is_error()) {
    VLOG(RLDP_INFO) << "failed to parse rldp packet [" << source << "->" << local_id << "]: " << F.move_as_error();
    return;
  }

  ton_api::downcast_call(*F.move_as_ok().get(),
                         [&](auto &obj) { this->process_message(source, local_id, transfer_id, obj); });
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_message &message) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, source, local_id, std::move(message.data_));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_query &message) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), source, local_id,
                                       timeout = td::Timestamp::at_unix(message.timeout_), query_id = message.query_id_,
                                       max_answer_size = static_cast<td::uint64>(message.max_answer_size_),
                                       transfer_id](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      auto data = R.move_as_ok();
      if (data.size() > max_answer_size) {
        VLOG(RLDP_NOTICE) << "rldp query failed: answer too big";
      } else {
        td::actor::send_closure(SelfId, &RldpIn::answer_query, local_id, source, timeout, query_id,
                                transfer_id ^ TransferId::ones(), std::move(data));
      }
    } else {
      VLOG(RLDP_NOTICE) << "rldp query failed: " << R.move_as_error();
    }
  });
  VLOG(RLDP_DEBUG) << "delivering rldp query";
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver_query, source, local_id, std::move(message.data_),
                          std::move(P));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_answer &message) {
  auto it = queries_.find(transfer_id);
  if (it != queries_.end()) {
    it->second.set_value(std::move(message.data_));
    queries_.erase(it);
  } else {
    VLOG(RLDP_INFO) << "received answer to unknown query " << message.query_id_;
  }
}

void RldpIn::on_sent(TransferId transfer_id, td::Result<td::Unit> state) {
  //TODO: completed transfer
}

void RldpIn::add_id(adnl::AdnlNodeIdShort local_id) {
  if (local_ids_.count(local_id) == 1) {
    return;
  }

  std::vector<std::string> X{adnl::Adnl::int_to_bytestring(ton_api::rldp2_messagePart::ID),
                             adnl::Adnl::int_to_bytestring(ton_api::rldp2_confirm::ID),
                             adnl::Adnl::int_to_bytestring(ton_api::rldp2_complete::ID)};
  for (auto &x : X) {
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id, x, make_adnl_callback());
  }

  local_ids_.insert(local_id);
}

void RldpIn::get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id, td::Promise<td::string> promise) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::get_conn_ip_str, l_id, p_id, std::move(promise));
}

std::unique_ptr<adnl::Adnl::Callback> RldpIn::make_adnl_callback() {
  class Callback : public adnl::Adnl::Callback {
   private:
    td::actor::ActorId<RldpIn> id_;

   public:
    Callback(td::actor::ActorId<RldpIn> id) : id_(id) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &RldpIn::receive_message_part, src, dst, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      promise.set_error(td::Status::Error(ErrorCode::notready, "rldp does not support queries"));
    }
  };

  return std::make_unique<Callback>(actor_id(this));
}

td::actor::ActorOwn<Rldp> Rldp::create(td::actor::ActorId<adnl::Adnl> adnl) {
  return td::actor::create_actor<RldpIn>("rldp", td::actor::actor_dynamic_cast<adnl::AdnlPeerTable>(adnl));
}

}  // namespace rldp2

}  // namespace ton
