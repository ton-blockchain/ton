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
#include "metrics/metrics-types.h"
#include "metrics/prometheus-exporter.h"
#include "metrics/tl-traffic-bucket.h"
#include "td/utils/Random.h"

#include "RldpConnection.h"
#include "rldp-in.hpp"

namespace ton {

namespace rldp2 {

class RldpConnectionActor : public td::actor::Actor, private ConnectionCallback {
 public:
  RldpConnectionActor(td::actor::ActorId<RldpIn> rldp, adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                      td::actor::ActorId<adnl::Adnl> adnl, std::shared_ptr<Rldp2Metrics> metrics)
      : rldp_(std::move(rldp)), src_(src), dst_(dst), adnl_(std::move(adnl)), metrics_(std::move(metrics)) {};

  void send(TransferId transfer_id, td::BufferSlice query, td::Timestamp timeout = td::Timestamp::never()) {
    connection_.send(transfer_id, std::move(query), timeout);
    yield();
  }
  void set_receive_limits(TransferId transfer_id, td::Timestamp timeout, td::uint64 max_size) {
    connection_.set_receive_limits(transfer_id, timeout, max_size);
  }
  void receive_raw(td::BufferSlice data) {
    if (metrics_) {
      metrics_->parts_received_from_adnl.fetch_add(1, std::memory_order_relaxed);
      metrics_->bytes_received_from_adnl.fetch_add(data.size(), std::memory_order_relaxed);
    }
    connection_.receive_raw(std::move(data));
    yield();
  }
  void set_default_mtu(td::uint64 mtu) {
    connection_.set_default_mtu(mtu);
  }

 private:
  td::actor::ActorId<RldpIn> rldp_;
  adnl::AdnlNodeIdShort src_;
  adnl::AdnlNodeIdShort dst_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  std::shared_ptr<Rldp2Metrics> metrics_;
  RldpConnection connection_;

  void loop() override {
    alarm_timestamp() = connection_.run(*this);
  }

  void send_raw(td::BufferSlice data) override {
    if (metrics_) {
      metrics_->parts_sent_to_adnl.fetch_add(1, std::memory_order_relaxed);
      metrics_->bytes_sent_to_adnl.fetch_add(data.size(), std::memory_order_relaxed);
    }
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

  metrics_->app_send_msgs_message.fetch_add(1, std::memory_order_relaxed);
  metrics_->app_send_bytes_message.fetch_add(data.size(), std::memory_order_relaxed);
  app_send_by_tl_message_.account(data.as_slice());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_message>(id, std::move(data)), true);

  auto transfer_id = get_random_transfer_id();
  send_closure(create_connection(src, dst, false), &RldpConnectionActor::send, transfer_id, std::move(B), timeout);
}

void RldpIn::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                           td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                           td::uint64 max_answer_size) {
  auto query_id = adnl::AdnlQuery::random_query_id();

  auto date = static_cast<td::uint32>(timeout.at_unix()) + 1;
  metrics_->app_send_msgs_query.fetch_add(1, std::memory_order_relaxed);
  metrics_->app_send_bytes_query.fetch_add(data.size(), std::memory_order_relaxed);
  app_send_by_tl_query_.account(data.as_slice());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_query>(query_id, max_answer_size, date, std::move(data)),
                               true);

  auto connection = create_connection(src, dst, false);
  auto transfer_id = get_random_transfer_id();
  auto response_transfer_id = get_responce_transfer_id(transfer_id);
  send_closure(connection, &RldpConnectionActor::set_receive_limits, response_transfer_id, timeout, max_answer_size);
  send_closure(connection, &RldpConnectionActor::send, transfer_id, std::move(B), timeout);

  queries_.emplace(response_transfer_id, std::move(promise));
}

void RldpIn::answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                          adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data) {
  metrics_->app_send_msgs_answer.fetch_add(1, std::memory_order_relaxed);
  metrics_->app_send_bytes_answer.fetch_add(data.size(), std::memory_order_relaxed);
  app_send_by_tl_answer_.account(data.as_slice());
  auto B = serialize_tl_object(create_tl_object<ton_api::rldp_answer>(query_id, std::move(data)), true);

  send_closure(create_connection(src, dst, false), &RldpConnectionActor::send, transfer_id, std::move(B), timeout);
}

void RldpIn::receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data) {
  auto connection = create_connection(local_id, source, true);
  if (connection.empty()) {
    return;
  }
  send_closure(connection, &RldpConnectionActor::receive_raw, std::move(data));
}

td::actor::ActorId<RldpConnectionActor> RldpIn::create_connection(adnl::AdnlNodeIdShort local_id,
                                                                  adnl::AdnlNodeIdShort peer_id, bool incoming) {
  auto it = connections_.find(std::make_pair(local_id, peer_id));
  if (it != connections_.end()) {
    return it->second.get();
  }
  td::uint64 mtu = get_peer_mtu(local_id, peer_id);
  if (mtu == 0 && incoming) {
    VLOG(RLDP_INFO) << "dropping incoming packet " << local_id << " <- " << peer_id << " : peer not allowed";
    return {};
  }
  auto connection = td::actor::create_actor<RldpConnectionActor>("RldpConnection", actor_id(this), local_id, peer_id,
                                                                 adnl_, metrics_);
  td::actor::send_closure(connection, &RldpConnectionActor::set_default_mtu, mtu);
  auto res = connection.get();
  connections_[std::make_pair(local_id, peer_id)] = std::move(connection);
  return res;
}

void RldpIn::receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             td::Result<td::BufferSlice> r_data) {
  if (r_data.is_error()) {
    metrics_->transfers_received_err.fetch_add(1, std::memory_order_relaxed);
    auto it = queries_.find(transfer_id);
    if (it != queries_.end()) {
      it->second.set_error(r_data.move_as_error());
      queries_.erase(it);
    } else {
      VLOG(RLDP_INFO) << "received error to unknown transfer_id " << transfer_id << " " << r_data.error();
    }
    return;
  }
  metrics_->transfers_received_ok.fetch_add(1, std::memory_order_relaxed);

  auto data = r_data.move_as_ok();
  //LOG(ERROR) << "RECEIVE MESSAGE " << data.size();
  auto F = fetch_tl_object<ton_api::rldp_Message>(std::move(data), true);
  if (F.is_error()) {
    metrics_->parse_errors.fetch_add(1, std::memory_order_relaxed);
    VLOG(RLDP_INFO) << "failed to parse rldp packet [" << source << "->" << local_id << "]: " << F.move_as_error();
    return;
  }

  ton_api::downcast_call(*F.move_as_ok().get(),
                         [&](auto &obj) { this->process_message(source, local_id, transfer_id, obj); });
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_message &message) {
  metrics_->app_deliver_msgs_message.fetch_add(1, std::memory_order_relaxed);
  metrics_->app_deliver_bytes_message.fetch_add(message.data_.size(), std::memory_order_relaxed);
  app_deliver_by_tl_message_.account(message.data_.as_slice());
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, source, local_id, std::move(message.data_));
}

void RldpIn::process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                             ton_api::rldp_query &message) {
  metrics_->app_deliver_msgs_query.fetch_add(1, std::memory_order_relaxed);
  metrics_->app_deliver_bytes_query.fetch_add(message.data_.size(), std::memory_order_relaxed);
  app_deliver_by_tl_query_.account(message.data_.as_slice());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), source, local_id,
                                       timeout = td::Timestamp::at_unix(message.timeout_), query_id = message.query_id_,
                                       max_answer_size = static_cast<td::uint64>(message.max_answer_size_),
                                       transfer_id](td::Result<td::BufferSlice> R) mutable {
    if (R.is_ok()) {
      auto data = R.move_as_ok();
      if (data.size() > max_answer_size) {
        VLOG(RLDP_NOTICE) << "rldp query failed: answer too big";
      } else {
        if (!timeout || td::Timestamp::in(60.0) < timeout) {
          timeout = td::Timestamp::in(60.0);
        }
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
    metrics_->app_deliver_msgs_answer.fetch_add(1, std::memory_order_relaxed);
    metrics_->app_deliver_bytes_answer.fetch_add(message.data_.size(), std::memory_order_relaxed);
    app_deliver_by_tl_answer_.account(message.data_.as_slice());
    it->second.set_value(std::move(message.data_));
    queries_.erase(it);
  } else {
    VLOG(RLDP_INFO) << "received answer to unknown query " << message.query_id_;
  }
}

void RldpIn::on_sent(TransferId transfer_id, td::Result<td::Unit> state) {
  if (state.is_ok()) {
    metrics_->transfers_sent_ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    metrics_->transfers_sent_err.fetch_add(1, std::memory_order_relaxed);
  }
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
    td::actor::send_closure(connection, &RldpConnectionActor::set_default_mtu, get_peer_mtu(p.first, p.second));
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

void RldpIn::collect(metrics::MetricsPromise P) {
  using metrics::MetricFamily;
  using metrics::MetricSet;
  MetricSet set;
  auto load = [](const std::atomic<td::uint64> &a) { return static_cast<double>(a.load(std::memory_order_relaxed)); };
  auto labeled = [&](std::string name, std::string type, std::string label_key,
                     std::vector<std::pair<std::string, double>> entries, std::optional<std::string> help = {}) {
    metrics::MetricFamily fam{.name = std::move(name), .type = std::move(type), .help = std::move(help), .metrics = {}};
    for (auto &[label, value] : entries) {
      fam.metrics.push_back(metrics::Metric{
          .suffix = "",
          .label_set = metrics::LabelSet{.labels = {{label_key, label}}},
          .samples = {metrics::Sample{.label_set = {}, .value = value}},
      });
    }
    set.families.push_back(std::move(fam));
  };
  labeled("app_send_bytes_total", "counter", "kind",
          {
              {"message", load(metrics_->app_send_bytes_message)},
              {"query", load(metrics_->app_send_bytes_query)},
              {"answer", load(metrics_->app_send_bytes_answer)},
          },
          "Bytes the application asked RLDP2 to send (raw payload, by kind).");
  labeled("app_send_messages_total", "counter", "kind",
          {
              {"message", load(metrics_->app_send_msgs_message)},
              {"query", load(metrics_->app_send_msgs_query)},
              {"answer", load(metrics_->app_send_msgs_answer)},
          },
          "Messages the application asked RLDP2 to send.");
  labeled("app_deliver_bytes_total", "counter", "kind",
          {
              {"message", load(metrics_->app_deliver_bytes_message)},
              {"query", load(metrics_->app_deliver_bytes_query)},
              {"answer", load(metrics_->app_deliver_bytes_answer)},
          },
          "Bytes RLDP2 delivered to the application.");
  labeled("app_deliver_messages_total", "counter", "kind",
          {
              {"message", load(metrics_->app_deliver_msgs_message)},
              {"query", load(metrics_->app_deliver_msgs_query)},
              {"answer", load(metrics_->app_deliver_msgs_answer)},
          },
          "Messages RLDP2 delivered to the application.");
  set.families.push_back(MetricFamily::make_scalar("bytes_sent_to_adnl_total", "counter",
                                                   load(metrics_->bytes_sent_to_adnl),
                                                   "RLDP2 serialized bytes handed to ADNL."));
  set.families.push_back(MetricFamily::make_scalar(
      "parts_sent_to_adnl_total", "counter", load(metrics_->parts_sent_to_adnl), "RLDP2 messages handed to ADNL."));
  set.families.push_back(MetricFamily::make_scalar("bytes_received_from_adnl_total", "counter",
                                                   load(metrics_->bytes_received_from_adnl),
                                                   "RLDP2 serialized bytes received from ADNL."));
  set.families.push_back(MetricFamily::make_scalar("parts_received_from_adnl_total", "counter",
                                                   load(metrics_->parts_received_from_adnl),
                                                   "RLDP2 messages received from ADNL."));
  set.families.push_back(MetricFamily::make_scalar("parse_errors_total", "counter", load(metrics_->parse_errors),
                                                   "RLDP2 message TL parse failures."));
  labeled("transfers_received_total", "counter", "result",
          {
              {"ok", load(metrics_->transfers_received_ok)},
              {"error", load(metrics_->transfers_received_err)},
          },
          "Inbound RLDP2 transfers concluded (success or error).");
  labeled("transfers_sent_total", "counter", "result",
          {
              {"ok", load(metrics_->transfers_sent_ok)},
              {"error", load(metrics_->transfers_sent_err)},
          },
          "Outbound RLDP2 transfers concluded (success or error).");
  set.families.push_back(MetricFamily::make_scalar(
      "connections_active", "gauge", static_cast<double>(connections_.size()), "Active RLDP2 connections."));
  set.families.push_back(MetricFamily::make_scalar("queries_pending", "gauge", static_cast<double>(queries_.size()),
                                                   "Pending RLDP2 queries awaiting answers."));

  auto append_tl = [&](std::vector<MetricFamily> families) {
    for (auto &fam : families) {
      set.families.push_back(std::move(fam));
    }
  };
  append_tl(metrics::render_tl_bucket("app_send", "message", app_send_by_tl_message_,
                                      "Bytes the application sent via RLDP2 rldp.message wrappers, by inner TL.",
                                      "Messages the application sent via RLDP2 rldp.message wrappers, by inner TL."));
  append_tl(metrics::render_tl_bucket("app_send", "query", app_send_by_tl_query_));
  append_tl(metrics::render_tl_bucket("app_send", "answer", app_send_by_tl_answer_));
  append_tl(metrics::render_tl_bucket(
      "app_deliver", "message", app_deliver_by_tl_message_,
      "Bytes RLDP2 delivered to the application from rldp.message wrappers, by inner TL.",
      "Messages RLDP2 delivered to the application from rldp.message wrappers, by inner TL."));
  append_tl(metrics::render_tl_bucket("app_deliver", "query", app_deliver_by_tl_query_));
  append_tl(metrics::render_tl_bucket("app_deliver", "answer", app_deliver_by_tl_answer_));
  P.set_value(std::move(set).wrap("rldp2"));
}

td::actor::ActorOwn<Rldp> Rldp::create(td::actor::ActorId<adnl::Adnl> adnl) {
  return td::actor::create_actor<RldpIn>("rldp", td::actor::actor_dynamic_cast<adnl::AdnlPeerTable>(adnl));
}

void Rldp::register_metrics(td::actor::ActorId<Rldp> rldp, td::actor::ActorId<PrometheusExporter> exporter) {
  auto impl = td::actor::actor_dynamic_cast<RldpIn>(rldp);
  if (impl.empty()) {
    return;
  }
  td::actor::send_closure(exporter, &PrometheusExporter::register_collector<RldpIn>, impl);
}

}  // namespace rldp2

}  // namespace ton
