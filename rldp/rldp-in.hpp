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

#include "rldp-peer.h"
#include "rldp.hpp"

namespace ton {

namespace rldp {

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

class RldpIn : public RldpImpl {
 public:
  static constexpr td::uint64 global_mtu() {
    return (1ull << 37);
  }
  static constexpr td::uint32 lru_size() {
    return 128;
  }
  TransferId transfer(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout, td::BufferSlice data,
                      TransferId t = TransferId::zero());
  void transfer_completed(TransferId transfer_id) override;

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_message_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                       td::BufferSlice data) override;

  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    send_query_ex(src, dst, name, std::move(promise), timeout, std::move(data), adnl::Adnl::get_mtu());
  }
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void answer_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::Timestamp timeout,
                    adnl::AdnlQueryId query_id, TransferId transfer_id, td::BufferSlice data);

  void alarm_query(adnl::AdnlQueryId query_id, TransferId transfer_id);

  void process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id,
                            ton_api::rldp_messagePart &part);
  void process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, ton_api::rldp_confirm &part);
  void process_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, ton_api::rldp_complete &part);
  void receive_message_part(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, td::BufferSlice data);

  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_message &message);
  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_query &message);
  void process_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       ton_api::rldp_answer &message);
  void receive_message(adnl::AdnlNodeIdShort source, adnl::AdnlNodeIdShort local_id, TransferId transfer_id,
                       td::BufferSlice data);

  void in_transfer_completed(TransferId transfer_id, bool success);

  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

  explicit RldpIn(td::actor::ActorId<adnl::AdnlPeerTable> adnl) : adnl_(adnl) {
  }

 protected:
  void start_up() override;
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                      td::optional<adnl::AdnlNodeIdShort> peer_id) override {
  }

 private:
  std::unique_ptr<adnl::Adnl::Callback> make_adnl_callback();

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;

  std::map<adnl::AdnlQueryId, td::actor::ActorOwn<adnl::AdnlQuery>> queries_;
  std::map<TransferId, td::actor::ActorOwn<RldpTransferSender>> senders_;
  std::map<TransferId, td::actor::ActorOwn<RldpTransferReceiver>> receivers_;

  std::set<TransferId> lru_set_;
  RldpLru lru_;
  td::uint32 lru_size_ = 0;

  std::map<TransferId, td::uint64> max_size_;

  std::set<adnl::AdnlNodeIdShort> local_ids_;

  using CounterPtr = std::shared_ptr<metrics::AtomicCounter<td::uint64>>;
  metrics::MultiCollector::Own metrics_collector_ = metrics::MultiCollector::create("rldp");
  metrics::AppTrafficMetrics::Ptr app_metrics_ = metrics::AppTrafficMetrics::make();
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr transfers_completed_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "direction", "transfers_completed_total",
          "RLDP transfers that finished (out includes both success and timeout-completion).");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr transfers_failed_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "direction", "transfers_failed_total", "RLDP transfers that failed.");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr bytes_sent_to_adnl_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "kind", "bytes_sent_to_adnl_total", "Serialized RLDP message bytes handed to ADNL (post FEC encoding).");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr parts_sent_to_adnl_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "kind", "parts_sent_to_adnl_total", "RLDP messages handed to ADNL.");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr bytes_received_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make(
          "kind", "bytes_received_total", "Serialized RLDP message bytes received from ADNL (pre FEC decoding).");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr parts_received_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make("kind", "parts_received_total",
                                                                               "RLDP messages received from ADNL.");
  metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::Ptr parse_errors_ =
      metrics::Labeled<std::string, metrics::AtomicCounter<td::uint64>>::make("where", "parse_errors_total",
                                                                               "RLDP TL parse failures.");
  metrics::AtomicCounter<td::uint64>::Ptr transfers_started_ =
      metrics::AtomicCounter<td::uint64>::make("transfers_started_total", "RLDP outbound transfers initiated.");
  metrics::LambdaGauge::Ptr outbound_transfers_ = metrics::LambdaGauge::make(
      "outbound_transfers",
      [this] {
        return std::vector<metrics::Sample>{
            metrics::Sample{.label_set = {}, .value = static_cast<double>(senders_.size())}};
      },
      "Active outbound RLDP transfers.");
  metrics::LambdaGauge::Ptr inbound_transfers_ = metrics::LambdaGauge::make(
      "inbound_transfers",
      [this] {
        return std::vector<metrics::Sample>{
            metrics::Sample{.label_set = {}, .value = static_cast<double>(receivers_.size())}};
      },
      "Active inbound RLDP transfers.");
  metrics::LambdaGauge::Ptr lru_size_gauge_ = metrics::LambdaGauge::make(
      "lru_size",
      [this] {
        return std::vector<metrics::Sample>{metrics::Sample{.label_set = {}, .value = static_cast<double>(lru_size_)}};
      },
      "Recent completed transfers in dedup LRU.");

  std::shared_ptr<RldpMetrics> metrics_ = std::make_shared<RldpMetrics>(RldpMetrics{
      .sent_to_adnl_part = {.bytes = bytes_sent_to_adnl_->label("part"), .msgs = parts_sent_to_adnl_->label("part")},
      .sent_to_adnl_confirm =
          {.bytes = bytes_sent_to_adnl_->label("confirm"), .msgs = parts_sent_to_adnl_->label("confirm")},
      .sent_to_adnl_complete =
          {.bytes = bytes_sent_to_adnl_->label("complete"), .msgs = parts_sent_to_adnl_->label("complete")},
      .received_part = {.bytes = bytes_received_->label("part"), .msgs = parts_received_->label("part")},
      .received_confirm = {.bytes = bytes_received_->label("confirm"), .msgs = parts_received_->label("confirm")},
      .received_complete = {.bytes = bytes_received_->label("complete"), .msgs = parts_received_->label("complete")},
      .transfers_started = transfers_started_,
      .transfers_completed_out = transfers_completed_->label("out"),
      .transfers_completed_in = transfers_completed_->label("in"),
      .transfers_failed_in = transfers_failed_->label("in"),
      .parse_errors_part = parse_errors_->label("part"),
      .parse_errors_message = parse_errors_->label("message"),
  });
};

}  // namespace rldp

}  // namespace ton
