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
#pragma once

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "adnl/adnl-peer-table.h"
#include "adnl/adnl-query.h"
#include "metrics/app-traffic-metrics.h"
#include "metrics/metrics-collectors.h"
#include "td/utils/List.h"
#include "tl-utils/tl-utils.hpp"

#include "rldp.hpp"

namespace ton {

namespace rldp2 {

class RldpLru : public td::ListNode {
 public:
  TransferId transfer_id() {
    return transfer_id_;
  }

  RldpLru(TransferId transfer_id) : transfer_id_(transfer_id) {
  }
  RldpLru() {
  }

  static RldpLru *from_list_node(td::ListNode *node) {
    return static_cast<RldpLru *>(node);
  }

 private:
  TransferId transfer_id_;
};

class RldpConnectionActor;
class RldpIn : public RldpImpl {
 public:
  static constexpr td::uint64 mtu() {
    return (1ull << 37);
  }
  static constexpr td::uint32 lru_size() {
    return 128;
  }
  void on_sent(TransferId transfer_id, td::Result<td::Unit> state);

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_message_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                       td::BufferSlice data) override;

  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    send_query_ex(src, dst, name, std::move(promise), timeout, std::move(data), default_mtu());
  }
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                    adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data);

  void receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data);

  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_message &message);
  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_query &message);
  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_answer &message);
  void receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       td::Result<td::BufferSlice> data);

  void add_id(adnl::AdnlNodeIdShort local_id) override;

  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

  explicit RldpIn(td::actor::ActorId<adnl::AdnlPeerTable> adnl) : adnl_(adnl) {
  }

 protected:
  void start_up() override;
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                      td::optional<adnl::AdnlNodeIdShort> peer_id) override;

  void alarm() override;

 private:
  std::unique_ptr<adnl::Adnl::Callback> make_adnl_callback();

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;

  struct Connection;
  std::map<std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>, Connection> connections_;
  std::set<std::tuple<td::Timestamp, adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>> timeout_set_;

  std::map<TransferId, td::Promise<td::BufferSlice>> queries_;

  std::set<adnl::AdnlNodeIdShort> local_ids_;

  metrics::MultiCollector::Own metrics_collector_ = metrics::MultiCollector::create("rldp2");
  metrics::AppTrafficMetrics::Ptr app_metrics_ = metrics::AppTrafficMetrics::make();
  metrics::AtomicCounter<td::uint64>::Ptr bytes_sent_to_adnl_ =
      metrics::AtomicCounter<td::uint64>::make("bytes_sent_to_adnl_total", "RLDP2 serialized bytes handed to ADNL.");
  metrics::AtomicCounter<td::uint64>::Ptr parts_sent_to_adnl_ =
      metrics::AtomicCounter<td::uint64>::make("parts_sent_to_adnl_total", "RLDP2 messages handed to ADNL.");
  metrics::AtomicCounter<td::uint64>::Ptr bytes_received_from_adnl_ = metrics::AtomicCounter<td::uint64>::make(
      "bytes_received_from_adnl_total", "RLDP2 serialized bytes received from ADNL.");
  metrics::AtomicCounter<td::uint64>::Ptr parts_received_from_adnl_ =
      metrics::AtomicCounter<td::uint64>::make("parts_received_from_adnl_total", "RLDP2 messages received from ADNL.");
  metrics::AtomicCounter<td::uint64>::Ptr parse_errors_ =
      metrics::AtomicCounter<td::uint64>::make("parse_errors_total", "RLDP2 message TL parse failures.");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr transfers_received_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "result", "transfers_received_total", "Inbound RLDP2 transfers concluded (success or error).");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr transfers_sent_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "result", "transfers_sent_total", "Outbound RLDP2 transfers concluded (success or error).");
  metrics::LambdaGauge::Ptr connections_active_ = metrics::LambdaGauge::make(
      "connections_active",
      [this] {
        return std::vector<metrics::Sample>{
            metrics::Sample{.label_set = {}, .value = static_cast<double>(connections_.size())}};
      },
      "Active RLDP2 connections.");
  metrics::LambdaGauge::Ptr queries_pending_ = metrics::LambdaGauge::make(
      "queries_pending",
      [this] {
        return std::vector<metrics::Sample>{
            metrics::Sample{.label_set = {}, .value = static_cast<double>(queries_.size())}};
      },
      "Pending RLDP2 queries awaiting answers.");

  std::shared_ptr<Rldp2Metrics> metrics_ = std::make_shared<Rldp2Metrics>(Rldp2Metrics{
      .bytes_sent_to_adnl = bytes_sent_to_adnl_,
      .parts_sent_to_adnl = parts_sent_to_adnl_,
      .bytes_received_from_adnl = bytes_received_from_adnl_,
      .parts_received_from_adnl = parts_received_from_adnl_,
      .parse_errors = parse_errors_,
      .transfers_received_ok = transfers_received_->label("ok"),
      .transfers_received_err = transfers_received_->label("error"),
      .transfers_sent_ok = transfers_sent_->label("ok"),
      .transfers_sent_err = transfers_sent_->label("error"),
  });

  td::actor::ActorId<RldpConnectionActor> get_or_create_connection(adnl::AdnlNodeIdShort local_id,
                                                                   adnl::AdnlNodeIdShort peer_id, bool incoming,
                                                                   td::Timestamp timeout = {});

  static constexpr double CONNECTION_TIMEOUT = 120.0;
};

}  // namespace rldp2

}  // namespace ton
