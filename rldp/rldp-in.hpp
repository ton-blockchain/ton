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

#include "rldp.hpp"
#include "rldp-peer.h"

#include "tl-utils/tl-utils.hpp"
#include "adnl/adnl-query.h"
#include "adnl/adnl-peer-table.h"

#include "td/utils/List.h"

#include <map>
#include <set>

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
  static constexpr td::uint64 mtu() {
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
    send_query_ex(src, dst, name, std::move(promise), timeout, std::move(data), default_mtu());
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

  void in_transfer_completed(TransferId transfer_id);

  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id, td::Promise<td::string> promise) override;

  RldpIn(td::actor::ActorId<adnl::AdnlPeerTable> adnl) : adnl_(adnl) {
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
};

}  // namespace rldp

}  // namespace ton
