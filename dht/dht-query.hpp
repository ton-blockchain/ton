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

#include <set>
#include <map>

#include "td/utils/int_types.h"
#include "td/actor/actor.h"

#include "adnl/adnl.h"
#include "td/actor/PromiseFuture.h"

#include "auto/tl/ton_api.hpp"

#include "dht.hpp"

namespace ton {

namespace dht {

class DhtMember;

class DhtQuery : public td::actor::Actor {
 protected:
  DhtKeyId key_;
  DhtNode self_;
  bool client_only_;

 public:
  DhtQuery(DhtKeyId key, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list, td::uint32 k,
           td::uint32 a, td::int32 our_network_id, DhtNode self, bool client_only, td::actor::ActorId<DhtMember> node,
           td::actor::ActorId<adnl::Adnl> adnl)
      : key_(key)
      , self_(std::move(self))
      , client_only_(client_only)
      , print_id_(print_id)
      , src_(src)
      , k_(k)
      , a_(a)
      , our_network_id_(our_network_id)
      , node_(node)
      , adnl_(adnl) {
    add_nodes(std::move(list));
  }
  DhtMember::PrintId print_id() const {
    return print_id_;
  }
  void send_queries();
  void add_nodes(DhtNodesList list);
  void finish_query() {
    active_queries_--;
    CHECK(active_queries_ <= k_);
    send_queries();
  }
  DhtKeyId get_key() const {
    return key_;
  }
  adnl::AdnlNodeIdShort get_src() const {
    return src_;
  }
  td::uint32 get_k() const {
    return k_;
  }
  td::int32 our_network_id() const {
    return our_network_id_;
  }
  void start_up() override {
    send_queries();
  }
  virtual void send_one_query(adnl::AdnlNodeIdShort id) = 0;
  virtual void finish(DhtNodesList list) = 0;
  virtual std::string get_name() const = 0;

 private:
  DhtMember::PrintId print_id_;
  adnl::AdnlNodeIdShort src_;
  std::map<DhtKeyId, DhtNode> list_;
  std::set<DhtKeyId> pending_ids_;
  td::uint32 k_;
  td::uint32 a_;
  td::int32 our_network_id_;
  td::actor::ActorId<DhtMember> node_;
  td::uint32 active_queries_ = 0;

 protected:
  td::actor::ActorId<adnl::Adnl> adnl_;
};

class DhtQueryFindNodes : public DhtQuery {
 private:
  td::Promise<DhtNodesList> promise_;

 public:
  DhtQueryFindNodes(DhtKeyId key, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list,
                    td::uint32 k, td::uint32 a, td::int32 our_network_id, DhtNode self, bool client_only,
                    td::actor::ActorId<DhtMember> node, td::actor::ActorId<adnl::Adnl> adnl,
                    td::Promise<DhtNodesList> promise)
      : DhtQuery(key, print_id, src, std::move(list), k, a, our_network_id, std::move(self), client_only, node, adnl)
      , promise_(std::move(promise)) {
  }
  void send_one_query(adnl::AdnlNodeIdShort id) override;
  void on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst);
  void finish(DhtNodesList list) override;
  std::string get_name() const override {
    return "find nodes";
  }
};

class DhtQueryFindValue : public DhtQuery {
 private:
  td::Promise<DhtValue> promise_;

 public:
  DhtQueryFindValue(DhtKeyId key, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list,
                    td::uint32 k, td::uint32 a, td::int32 our_network_id, DhtNode self, bool client_only,
                    td::actor::ActorId<DhtMember> node, td::actor::ActorId<adnl::Adnl> adnl,
                    td::Promise<DhtValue> promise)
      : DhtQuery(key, print_id, src, std::move(list), k, a, our_network_id, std::move(self), client_only, node, adnl)
      , promise_(std::move(promise)) {
  }
  void send_one_query(adnl::AdnlNodeIdShort id) override;
  void send_one_query_nodes(adnl::AdnlNodeIdShort id);
  void on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst);
  void on_result_nodes(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst);
  void finish(DhtNodesList list) override;
  std::string get_name() const override {
    return "find value";
  }
};

class DhtQueryStore : public td::actor::Actor {
 private:
  DhtMember::PrintId print_id_;
  td::uint32 k_;
  td::uint32 a_;
  td::int32 our_network_id_;
  td::Promise<td::Unit> promise_;
  td::actor::ActorId<DhtMember> node_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  adnl::AdnlNodeIdShort src_;
  DhtValue value_;
  td::uint32 success_ = 0;
  td::uint32 fail_ = 0;
  td::uint32 remaining_;
  DhtNodesList list_;
  DhtNode self_;
  bool client_only_;

 public:
  DhtQueryStore(DhtValue key_value, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list,
                td::uint32 k, td::uint32 a, td::int32 our_network_id, DhtNode self, bool client_only, td::actor::ActorId<DhtMember> node,
                td::actor::ActorId<adnl::Adnl> adnl, td::Promise<td::Unit> promise);
  void send_stores(td::Result<DhtNodesList> res);
  void store_ready(td::Result<td::BufferSlice> res);
  void start_up() override;
  DhtMember::PrintId print_id() const {
    return print_id_;
  }
};

class DhtQueryRegisterReverseConnection : public td::actor::Actor {
 private:
  DhtMember::PrintId print_id_;
  td::uint32 k_;
  td::uint32 a_;
  td::int32 our_network_id_;
  td::Promise<td::Unit> promise_;
  td::actor::ActorId<DhtMember> node_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  adnl::AdnlNodeIdShort src_;
  DhtKeyId key_id_;
  td::BufferSlice query_;
  td::uint32 success_ = 0;
  td::uint32 fail_ = 0;
  td::uint32 remaining_;
  DhtNodesList list_;
  DhtNode self_;
  bool client_only_;

 public:
  DhtQueryRegisterReverseConnection(DhtKeyId key_id, adnl::AdnlNodeIdFull client, td::uint32 ttl,
                                    td::BufferSlice signature, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src,
                                    DhtNodesList list, td::uint32 k, td::uint32 a, td::int32 our_network_id,
                                    DhtNode self, bool client_only, td::actor::ActorId<DhtMember> node,
                                    td::actor::ActorId<adnl::Adnl> adnl, td::Promise<td::Unit> promise);
  void send_queries(td::Result<DhtNodesList> R);
  void ready(td::Result<td::BufferSlice> R);
  void start_up() override;
  DhtMember::PrintId print_id() const {
    return print_id_;
  }
};

class DhtQueryRequestReversePing : public DhtQuery {
 private:
  td::Promise<td::Unit> promise_;
  td::BufferSlice query_;

 public:
  DhtQueryRequestReversePing(adnl::AdnlNodeIdShort client, adnl::AdnlNode target, td::BufferSlice signature,
                             DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list, td::uint32 k,
                             td::uint32 a, td::int32 our_network_id, DhtNode self, bool client_only,
                             td::actor::ActorId<DhtMember> node, td::actor::ActorId<adnl::Adnl> adnl,
                             td::Promise<td::Unit> promise)
      : DhtQuery(DhtMember::get_reverse_connection_key(client).compute_key_id(), print_id, src, std::move(list), k, a,
                 our_network_id, std::move(self), client_only, node, adnl)
      , promise_(std::move(promise))
      , query_(create_serialize_tl_object<ton_api::dht_requestReversePing>(target.tl(), std::move(signature),
                                                                           client.bits256_value(), k)) {
  }
  void send_one_query(adnl::AdnlNodeIdShort id) override;
  void on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst);
  void finish(DhtNodesList list) override;
  std::string get_name() const override {
    return "request remote ping";
  }
};

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtQuery &dht) {
  sb << dht.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtQuery *dht) {
  sb << dht->print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtQueryStore &dht) {
  sb << dht.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtQueryStore *dht) {
  sb << dht->print_id();
  return sb;
}

}  // namespace dht

}  // namespace ton
