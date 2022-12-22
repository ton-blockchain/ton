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

#include "dht.hpp"
#include "td/db/KeyValueAsync.h"

#include <map>

namespace ton {

namespace dht {

class DhtRemoteNode;
class DhtBucket;

class DhtMemberImpl : public DhtMember {
 private:
  class DhtKeyValueLru : public td::ListNode {
   public:
    DhtKeyValueLru(DhtValue value) : kv_(std::move(value)) {
    }
    DhtValue kv_;
    static inline DhtKeyValueLru *from_list_node(ListNode *node) {
      return static_cast<DhtKeyValueLru *>(node);
    }
  };
  //std::unique_ptr<adnl::AdnlDecryptor> decryptor_;
  adnl::AdnlNodeIdShort id_;
  DhtKeyId key_;
  td::uint32 k_;
  td::uint32 a_;
  td::int32 network_id_{-1};
  td::uint32 max_cache_time_ = 60;
  td::uint32 max_cache_size_ = 100;

  std::vector<DhtBucket> buckets_;

  std::string db_root_;

  // to be republished once in a while
  std::map<DhtKeyId, DhtValue> our_values_;

  std::map<DhtKeyId, DhtKeyValueLru> cached_values_;
  td::ListNode cached_values_lru_;

  std::map<DhtKeyId, DhtValue> values_;

  td::Timestamp fill_att_ = td::Timestamp::in(0);
  td::Timestamp republish_att_ = td::Timestamp::in(0);

  DhtKeyId last_republish_key_ = DhtKeyId::zero();
  DhtKeyId last_check_key_ = DhtKeyId::zero();
  adnl::AdnlNodeIdShort last_check_reverse_conn_ = adnl::AdnlNodeIdShort::zero();

  struct ReverseConnection {
    adnl::AdnlNodeIdShort dht_node_;
    DhtKeyId key_id_;
    td::Timestamp ttl_;
  };
  std::map<adnl::AdnlNodeIdShort, ReverseConnection> reverse_connections_;
  std::set<adnl::AdnlNodeIdShort> our_reverse_connections_;

  class Callback : public adnl::Adnl::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      CHECK(dst == id_);
      td::actor::send_closure(self_, &DhtMemberImpl::receive_message, src, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      CHECK(dst == id_);
      td::actor::send_closure(self_, &DhtMemberImpl::receive_query, src, std::move(data), std::move(promise));
    }

    Callback(td::actor::ActorId<DhtMemberImpl> self, adnl::AdnlNodeIdShort id) : self_(self), id_(id) {
    }

   private:
    td::actor::ActorId<DhtMemberImpl> self_;
    adnl::AdnlNodeIdShort id_;
  };

  void receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise);

  void receive_message(adnl::AdnlNodeIdShort src, td::BufferSlice data);

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;

  bool client_only_{false};

  td::uint64 ping_queries_{0};
  td::uint64 find_node_queries_{0};
  td::uint64 find_value_queries_{0};
  td::uint64 store_queries_{0};
  td::uint64 get_addr_list_queries_{0};

  using DbType = td::KeyValueAsync<td::Bits256, td::BufferSlice>;
  DbType db_;
  td::Timestamp next_save_to_db_at_ = td::Timestamp::in(10.0);

  void save_to_db();

  DhtNodesList get_nearest_nodes(DhtKeyId id, td::uint32 k);
  void check();

  template <class T>
  void process_query(adnl::AdnlNodeIdShort src, T &query, td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad DHT query"));
  }

  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_ping &query, td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_findNode &query, td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_findValue &query, td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_store &query, td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_getSignedAddressList &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_registerReverseConnection &query,
                     td::Promise<td::BufferSlice> promise);
  void process_query(adnl::AdnlNodeIdShort src, ton_api::dht_requestReversePing &query,
                     td::Promise<td::BufferSlice> promise);

 public:
  DhtMemberImpl(adnl::AdnlNodeIdShort id, std::string db_root, td::actor::ActorId<keyring::Keyring> keyring,
                td::actor::ActorId<adnl::Adnl> adnl, td::int32 network_id, td::uint32 k, td::uint32 a = 3,
                bool client_only = false)
      : id_(id)
      , key_{id_}
      , k_(k)
      , a_(a)
      , network_id_(network_id)
      , db_root_(db_root)
      , keyring_(keyring)
      , adnl_(adnl)
      , client_only_(client_only) {
    for (size_t i = 0; i < 256; i++) {
      buckets_.emplace_back(k_);
    }
  }

  void add_full_node(DhtKeyId id, DhtNode node) override {
    add_full_node_impl(id, std::move(node));
  }
  void add_full_node_impl(DhtKeyId id, DhtNode node, bool set_active = false);

  adnl::AdnlNodeIdShort get_id() const override {
    return id_;
  }

  void receive_ping(DhtKeyId id, DhtNode result) override;

  void set_value(DhtValue key_value, td::Promise<td::Unit> result) override;
  td::uint32 distance(DhtKeyId key_id, td::uint32 max_value);

  void register_reverse_connection(adnl::AdnlNodeIdFull client, td::Promise<td::Unit> promise) override;
  void request_reverse_ping(adnl::AdnlNode target, adnl::AdnlNodeIdShort client,
                            td::Promise<td::Unit> promise) override;
  void request_reverse_ping_cont(adnl::AdnlNode target, td::BufferSlice signature, adnl::AdnlNodeIdShort client,
                                 td::Promise<td::Unit> promise);

  td::Status store_in(DhtValue value) override;
  void send_store(DhtValue value, td::Promise<td::Unit> promise);

  void get_value_in(DhtKeyId key, td::Promise<DhtValue> result) override;
  void get_value(DhtKey key, td::Promise<DhtValue> result) override {
    get_value_in(key.compute_key_id(), std::move(result));
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(1.0);
    check();
  }
  void start_up() override;
  void tear_down() override;
  void dump(td::StringBuilder &sb) const override;
  PrintId print_id() const override {
    return PrintId{id_};
  }

  void get_self_node(td::Promise<DhtNode> promise) override;
};

}  // namespace dht

}  // namespace ton
