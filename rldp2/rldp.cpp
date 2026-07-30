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
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "fec/fec.h"
#include "td/actor/coro_utils.h"
#include "td/utils/Random.h"

#include "RldpConnection.h"
#include "rldp-in.hpp"

DEFINE_LOG_CATEGORY(rldp2, VERBOSITY_NAME(WARNING))

namespace ton {

namespace rldp2 {

struct RldpIn::Connection {
  td::actor::ActorOwn<RldpConnectionActor> actor;
  td::Timestamp remove_at;
};

class RldpConnectionActor : public td::actor::Actor, private ConnectionCallback {
 public:
  RldpConnectionActor(td::actor::ActorId<RldpIn> rldp, adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                      td::actor::ActorId<adnl::Adnl> adnl)
      : rldp_(std::move(rldp)), src_(src), dst_(dst), adnl_(std::move(adnl)) {};

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
  void set_default_mtu(td::uint64 mtu) {
    connection_.set_default_mtu(mtu);
  }
  // Drain this connection's metrics delta into RldpIn and round-trip `done` back through the absorb,
  // so collect() only resumes once the delta is in the aggregate.
  void collect_metrics(td::Promise<td::Unit> done) {
    send_closure(rldp_, &RldpIn::absorb, connection_.drain(), std::move(done));
  }

 private:
  td::actor::ActorId<RldpIn> rldp_;
  adnl::AdnlNodeIdShort src_;
  adnl::AdnlNodeIdShort dst_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  RldpConnection connection_;

  void tear_down() override {
    // Drain whatever hasn't been scraped yet so the final counts aren't lost (fire-and-forget).
    send_closure(rldp_, &RldpIn::absorb, connection_.drain(), td::Promise<td::Unit>());
  }

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

  auto magic = metrics::resolve_tl_magic(data.as_slice());
  metrics_.app.record(metrics::Kind::message, metrics::Direction::out, magic, data.size());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_message>(id, std::move(data)), true);

  auto transfer_id = get_random_transfer_id();
  send_closure(get_or_create_connection(src, dst, false, timeout), &RldpConnectionActor::send, transfer_id,
               std::move(B), timeout);
  if (timeout) {
    // Without a timeout the connection sets no limit for the transfer, so on_sent may never fire and
    // the delivery entry would live forever (see RldpConnection::send).
    messages_.emplace(transfer_id, OutMessage{.dst = dst, .magic = magic, .timer = {}});
  }
}

void RldpIn::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                           td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                           td::uint64 max_answer_size) {
  auto query_id = adnl::AdnlQuery::random_query_id();

  auto date = static_cast<td::uint32>(timeout.at_unix()) + 1;
  auto magic = metrics::resolve_tl_magic(data.as_slice());
  metrics_.app.record(metrics::Kind::query, metrics::Direction::out, magic, data.size());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_query>(query_id, max_answer_size, date, std::move(data)),
                               true);

  auto connection = get_or_create_connection(src, dst, false, timeout);
  auto transfer_id = get_random_transfer_id();
  auto response_transfer_id = get_responce_transfer_id(transfer_id);
  send_closure(connection, &RldpConnectionActor::set_receive_limits, response_transfer_id, timeout, max_answer_size);
  send_closure(connection, &RldpConnectionActor::send, transfer_id, std::move(B), timeout);

  queries_.emplace(
      response_transfer_id,
      OutQuery{
          .promise = std::move(promise), .max_answer_size = max_answer_size, .dst = dst, .magic = magic, .timer = {}});
}

void RldpIn::answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                          adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data) {
  metrics_.app.record(metrics::Kind::answer, metrics::Direction::out, data.as_slice());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_answer>(query_id, std::move(data)), true);

  send_closure(get_or_create_connection(src, dst, false, timeout), &RldpConnectionActor::send, transfer_id,
               std::move(B), timeout);
}

void RldpIn::receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data) {
  auto connection = get_or_create_connection(local_id, source, true);
  if (connection.empty()) {
    // peer not allowed: the datagram still arrived on the wire, and we dropped it (no connection to
    // attribute it to, so it goes straight into the historical aggregate).
    aggregate_.record_wire(metrics::Direction::in, data.size());
    metrics_.app.record_dropped(metrics::Direction::in, metrics::Reason::limited);
    return;
  }
  send_closure(connection, &RldpConnectionActor::receive_raw, std::move(data));
}

td::actor::ActorId<RldpConnectionActor> RldpIn::get_or_create_connection(adnl::AdnlNodeIdShort local_id,
                                                                         adnl::AdnlNodeIdShort peer_id, bool incoming,
                                                                         td::Timestamp timeout) {
  if (!timeout) {
    timeout = td::Timestamp::now();
  }
  timeout += CONNECTION_TIMEOUT;
  auto it = connections_.find(std::make_pair(local_id, peer_id));
  if (it != connections_.end()) {
    timeout_set_.erase({it->second.remove_at, local_id, peer_id});
    it->second.remove_at = std::max(it->second.remove_at, timeout);
    timeout_set_.emplace(it->second.remove_at, local_id, peer_id);
    alarm_timestamp().relax(timeout);
    return it->second.actor.get();
  }
  td::uint64 mtu = get_peer_mtu(local_id, peer_id);
  if (mtu == 0 && incoming) {
    // Accounted by receive_message_part.
    VLOG(rldp2, INFO) << "dropping incoming packet " << local_id << " <- " << peer_id << " : peer not allowed";
    return {};
  }
  auto connection =
      td::actor::create_actor<RldpConnectionActor>("RldpConnection", actor_id(this), local_id, peer_id, adnl_);
  td::actor::send_closure(connection, &RldpConnectionActor::set_default_mtu, mtu);
  auto res = connection.get();
  connections_[std::make_pair(local_id, peer_id)] = {std::move(connection), timeout};
  timeout_set_.emplace(timeout, local_id, peer_id);
  alarm_timestamp().relax(timeout);
  VLOG(rldp2, INFO) << "creating connection " << local_id << " , " << peer_id << " ("
                    << (incoming ? "inbound" : "outbound") << ")";
  return res;
}

static State transfer_outcome(const td::Status &error) {
  return error.code() == ErrorCode::timeout ? State::timeout : State::failed;
}

void RldpIn::receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             td::Result<td::BufferSlice> r_data) {
  if (r_data.is_error()) {
    metrics_.transport.transfers.at(metrics::Direction::in, transfer_outcome(r_data.error())).inc();
    if (auto it = queries_.find(transfer_id); it != queries_.end()) {
      finish_query(it, r_data.move_as_error());
    } else {
      VLOG(rldp2, INFO) << "received error to unknown transfer_id " << transfer_id << " " << r_data.error();
    }
    return;
  }
  metrics_.transport.transfers.at(metrics::Direction::in, State::completed).inc();

  auto data = r_data.move_as_ok();
  auto F = fetch_tl_object<ton_api::rldp_Message>(std::move(data), true);
  if (F.is_error()) {
    aggregate_.record_dropped(metrics::Direction::in, metrics::Reason::invalid);
    VLOG(rldp2, INFO) << "failed to parse rldp packet [" << source << "->" << local_id << "]: " << F.error();
    if (auto it = queries_.find(transfer_id); it != queries_.end()) {
      finish_query(it, F.move_as_error_prefix("received invalid rldp query answer: "));
    }
    return;
  }

  ton_api::downcast_call(*F.move_as_ok().get(),
                         [&](auto &obj) { this->process_message(source, local_id, transfer_id, obj); });

  if (auto it = queries_.find(transfer_id); it != queries_.end()) {
    finish_query(it, td::Status::Error("received invalid rldp query answer"));
  }
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_message &message) {
  metrics_.app.record(metrics::Kind::message, metrics::Direction::in, message.data_.as_slice());
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, source, local_id, std::move(message.data_));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_query &message) {
  metrics_.app.record(metrics::Kind::query, metrics::Direction::in, message.data_.as_slice());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), source, local_id,
                                       timeout = td::Timestamp::at_unix(message.timeout_), query_id = message.query_id_,
                                       max_answer_size = static_cast<td::uint64>(message.max_answer_size_),
                                       transfer_id](td::Result<td::BufferSlice> R) mutable {
    if (R.is_ok()) {
      auto data = R.move_as_ok();
      if (data.size() > max_answer_size) {
        td::actor::send_closure(SelfId, &RldpIn::on_outbound_answer_dropped);
        VLOG(rldp2, INFO) << "rldp query failed: answer too big";
      } else {
        if (!timeout || td::Timestamp::in(60.0) < timeout) {
          timeout = td::Timestamp::in(60.0);
        }
        td::actor::send_closure(SelfId, &RldpIn::answer_query, local_id, source, timeout, query_id,
                                transfer_id ^ TransferId::ones(), std::move(data));
      }
    } else {
      VLOG(rldp2, INFO) << "rldp query failed: " << R.move_as_error();
    }
  });
  VLOG(rldp2, DEBUG) << "delivering rldp query";
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver_query, source, local_id, std::move(message.data_),
                          std::move(P));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_answer &message) {
  auto it = queries_.find(transfer_id);
  if (it != queries_.end()) {
    if (message.data_.size() <= it->second.max_answer_size) {
      metrics_.app.record(metrics::Kind::answer, metrics::Direction::in, message.data_.as_slice());
      finish_query(it, std::move(message.data_));
    } else {
      metrics_.app.record_dropped(metrics::Direction::in, metrics::Reason::limited);
      finish_query(it, td::Status::Error("received too big answer"));
    }
  } else {
    VLOG(rldp2, INFO) << "received answer to unknown query " << message.query_id_;
  }
}

void RldpIn::finish_query(std::map<TransferId, OutQuery>::iterator it, td::Result<td::BufferSlice> result) {
  auto query = std::move(it->second);
  queries_.erase(it);
  metrics_.query_roundtrip.record(query.magic, query.dst, query.timer.elapsed(), result.is_ok());
  query.promise.set_result(std::move(result));
}

void RldpIn::on_sent(TransferId transfer_id, td::Result<td::Unit> state) {
  metrics_.transport.transfers
      .at(metrics::Direction::out, state.is_ok() ? State::completed : transfer_outcome(state.error()))
      .inc();
  // The peer confirming the transfer is what makes a message delivered; a timed-out transfer counts
  // as an undelivered one, however it ends up classified above.
  if (auto it = messages_.find(transfer_id); it != messages_.end()) {
    metrics_.message_delivery.record(it->second.magic, it->second.dst, it->second.timer.elapsed(), state.is_ok());
    messages_.erase(it);
  }
}

void RldpIn::absorb(RldpConnMetrics delta, td::Promise<td::Unit> done) {
  aggregate_ += delta;
  done.set_value(td::Unit{});
}

void RldpIn::on_outbound_answer_dropped() {
  metrics_.app.record_dropped(metrics::Direction::out, metrics::Reason::limited);
}

td::actor::Task<> RldpIn::collect(metrics::Context ctx) {
  // Schedule a drain on every active connection first (synchronous loop — the map is never mutated
  // across a suspension), then wait for all the round-trips to land in aggregate_. A connection that
  // dies mid-drain has already drained via its tear_down, so a failed ask is harmless.
  std::vector<td::actor::StartedTask<td::Unit>> drains;
  for (auto &[key, conn] : connections_) {
    drains.push_back(td::actor::ask(conn.actor.get(), &RldpConnectionActor::collect_metrics));
  }
  co_await td::actor::all_wrap(std::move(drains));

  metrics_.transport.connections.set(connections_.size());
  metrics_.transport.queries_pending.set(queries_.size());

  auto rldp = ctx.with_name("rldp2");
  rldp.collect(aggregate_.wire, "wire");
  rldp.collect(aggregate_.transport_dropped, "transport_dropped");
  rldp.collect(metrics_);
  co_return {};
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

void RldpIn::on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id, td::optional<adnl::AdnlNodeIdShort> peer_id) {
  auto update_mtu = [&](const auto &it) {
    auto &[p, connection] = *it;
    td::actor::send_closure(connection.actor, &RldpConnectionActor::set_default_mtu, get_peer_mtu(p.first, p.second));
  };
  if (local_id && peer_id) {
    auto it = connections_.find({local_id.value(), peer_id.value()});
    if (it != connections_.end()) {
      update_mtu(it);
    }
    return;
  }
  auto it =
      local_id ? connections_.lower_bound({local_id.value(), adnl::AdnlNodeIdShort::zero()}) : connections_.begin();
  while (it != connections_.end()) {
    if (local_id && it->first.second != local_id.value()) {
      break;
    }
    update_mtu(it);
    ++it;
  }
}

void RldpIn::alarm() {
  for (auto it = timeout_set_.begin(); it != timeout_set_.end();) {
    auto &[timeout, local_id, peer_id] = *it;
    if (timeout.is_in_past()) {
      VLOG(rldp2, INFO) << "removing old connection " << local_id << " , " << peer_id;
      connections_.erase({local_id, peer_id});
      it = timeout_set_.erase(it);
    } else {
      alarm_timestamp() = timeout;
      break;
    }
  }
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
