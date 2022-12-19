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
#include "dht.hpp"

#include "td/utils/tl_storers.h"
#include "td/utils/crypto.h"
#include "td/utils/Random.h"
#include "td/utils/overloaded.h"

#include "td/utils/format.h"

#include "auto/tl/ton_api.hpp"

#include "dht-query.hpp"

namespace ton {

namespace dht {

void DhtQuery::send_queries() {
  VLOG(DHT_EXTRA_DEBUG) << this << ": sending new queries. active=" << active_queries_ << " max_active=" << a_;
  while (pending_ids_.size() > 0 && active_queries_ < a_) {
    active_queries_++;
    auto id_xor = *pending_ids_.begin();
    auto id = id_xor ^ key_;
    VLOG(DHT_EXTRA_DEBUG) << this << ": sending " << get_name() << " query to " << id;
    pending_ids_.erase(id_xor);

    auto it = list_.find(id_xor);
    CHECK(it != list_.end());
    td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, get_src(), it->second.adnl_id(), it->second.addr_list());
    send_one_query(id.to_adnl());
  }
  if (active_queries_ == 0) {
    CHECK(pending_ids_.size() == 0);
    DhtNodesList list;
    for (auto &node : list_) {
      list.push_back(std::move(node.second));
    }
    CHECK(list.size() <= k_);
    VLOG(DHT_EXTRA_DEBUG) << this << ": finalizing " << get_name() << " query. List size=" << list.size();
    finish(std::move(list));
    stop();
  }
}

void DhtQuery::add_nodes(DhtNodesList list) {
  VLOG(DHT_EXTRA_DEBUG) << this << ": " << get_name() << " query: received " << list.size() << " new dht nodes";
  for (auto &node : list.list()) {
    auto id = node.get_key();
    auto id_xor = key_ ^ id;
    if (list_.find(id_xor) != list_.end()) {
      continue;
    }
    td::actor::send_closure(node_, &DhtMember::add_full_node, id, node.clone());

    DhtKeyId last_id_xor;
    if (list_.size() > 0) {
      last_id_xor = list_.rbegin()->first;
    }

    if (list_.size() < k_ || id_xor < last_id_xor) {
      list_[id_xor] = std::move(node);
      pending_ids_.insert(id_xor);
      if (list_.size() > k_) {
        CHECK(id_xor != last_id_xor);
        VLOG(DHT_EXTRA_DEBUG) << this << ": " << get_name() << " query: replacing " << (last_id_xor ^ key_)
                              << " key with " << id;
        pending_ids_.erase(last_id_xor);
        list_.erase(last_id_xor);
      } else {
        VLOG(DHT_EXTRA_DEBUG) << this << ": " << get_name() << " query: adding " << id << " key";
      }
    }
  }
}

void DhtQueryFindNodes::send_one_query(adnl::AdnlNodeIdShort id) {
  auto P = create_serialize_tl_object<ton_api::dht_findNode>(get_key().tl(), get_k());
  td::BufferSlice B;
  if (client_only_) {
    B = std::move(P);
  } else {
    B = create_serialize_tl_object_suffix<ton_api::dht_query>(P.as_slice(), self_.tl());
  }

  auto Pr = td::PromiseCreator::lambda([SelfId = actor_id(this), dst = id](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DhtQueryFindNodes::on_result, std::move(R), dst);
  });

  td::actor::send_closure(adnl_, &adnl::Adnl::send_query, get_src(), id, "dht findNode", std::move(Pr),
                          td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), std::move(B));
}

void DhtQueryFindNodes::on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst) {
  if (R.is_error()) {
    VLOG(DHT_INFO) << this << ": failed find nodes query " << get_src() << "->" << dst << ": " << R.move_as_error();
    finish_query();
    return;
  }

  auto Res = fetch_tl_object<ton_api::dht_nodes>(R.move_as_ok(), true);
  if (Res.is_error()) {
    VLOG(DHT_WARNING) << this << ": incorrect result on dht.findNodes query from " << dst << ": "
                      << Res.move_as_error();
  } else {
    add_nodes(DhtNodesList{Res.move_as_ok(), our_network_id()});
  }
  finish_query();
}

void DhtQueryFindNodes::finish(DhtNodesList list) {
  promise_.set_result(std::move(list));
}

void DhtQueryFindValue::send_one_query(adnl::AdnlNodeIdShort id) {
  auto P = create_serialize_tl_object<ton_api::dht_findValue>(get_key().tl(), get_k());
  td::BufferSlice B;
  if (client_only_) {
    B = std::move(P);
  } else {
    B = create_serialize_tl_object_suffix<ton_api::dht_query>(P.as_slice(), self_.tl());
  }

  auto Pr = td::PromiseCreator::lambda([SelfId = actor_id(this), dst = id](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DhtQueryFindValue::on_result, std::move(R), dst);
  });

  td::actor::send_closure(adnl_, &adnl::Adnl::send_query, get_src(), id, "dht findValue", std::move(Pr),
                          td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), std::move(B));
}

void DhtQueryFindValue::send_one_query_nodes(adnl::AdnlNodeIdShort id) {
  auto P = create_serialize_tl_object<ton_api::dht_findNode>(get_key().tl(), get_k());
  td::BufferSlice B;
  if (client_only_) {
    B = std::move(P);
  } else {
    B = create_serialize_tl_object_suffix<ton_api::dht_query>(P.as_slice(), self_.tl());
  }

  auto Pr = td::PromiseCreator::lambda([SelfId = actor_id(this), dst = id](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DhtQueryFindValue::on_result_nodes, std::move(R), dst);
  });

  td::actor::send_closure(adnl_, &adnl::Adnl::send_query, get_src(), id, "dht findValue", std::move(Pr),
                          td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), std::move(B));
}

void DhtQueryFindValue::on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst) {
  if (R.is_error()) {
    VLOG(DHT_INFO) << this << ": failed find value query " << get_src() << "->" << dst << ": " << R.move_as_error();
    finish_query();
    return;
  }
  auto Res = fetch_tl_object<ton_api::dht_ValueResult>(R.move_as_ok(), true);
  if (Res.is_error()) {
    VLOG(DHT_WARNING) << this << ": dropping incorrect answer on dht.findValue query from " << dst << ": "
                      << Res.move_as_error();
    finish_query();
    return;
  }

  bool need_stop = false;
  bool send_get_nodes = false;

  auto A = Res.move_as_ok();
  ton_api::downcast_call(
      *A, td::overloaded(
              [&](ton_api::dht_valueFound &v) {
                auto valueR = DhtValue::create(std::move(v.value_), true);
                if (valueR.is_error()) {
                  VLOG(DHT_WARNING) << this << ": received incorrect dht answer on find value query from " << dst
                                    << ": " << valueR.move_as_error();
                  return;
                }
                auto value = valueR.move_as_ok();
                if (value.key_id() != key_) {
                  VLOG(DHT_WARNING) << this << ": received value for bad key on find value query from " << dst;
                  return;
                }
                if (!value.check_is_acceptable()) {
                  send_get_nodes = true;
                  return;
                }
                promise_.set_value(std::move(value));
                need_stop = true;
              },
              [&](ton_api::dht_valueNotFound &v) {
                add_nodes(DhtNodesList{std::move(v.nodes_), our_network_id()});
              }));
  if (need_stop) {
    stop();
  } else if (send_get_nodes) {
    send_one_query_nodes(dst);
  } else {
    finish_query();
  }
}

void DhtQueryFindValue::on_result_nodes(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst) {
  if (R.is_error()) {
    VLOG(DHT_INFO) << this << ": failed find nodes query " << get_src() << "->" << dst << ": " << R.move_as_error();
    finish_query();
    return;
  }
  auto Res = fetch_tl_object<ton_api::dht_nodes>(R.move_as_ok(), true);
  if (Res.is_error()) {
    VLOG(DHT_WARNING) << this << ": dropping incorrect answer on dht.findNodes query from " << dst << ": "
                      << Res.move_as_error();
    finish_query();
    return;
  }
  auto r = Res.move_as_ok();
  add_nodes(DhtNodesList{create_tl_object<ton_api::dht_nodes>(std::move(r->nodes_)), our_network_id()});
  finish_query();
}

void DhtQueryFindValue::finish(DhtNodesList list) {
  promise_.set_error(td::Status::Error(ErrorCode::notready, "dht key not found"));
}

DhtQueryStore::DhtQueryStore(DhtValue key_value, DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src,
                             DhtNodesList list, td::uint32 k, td::uint32 a, td::int32 our_network_id, DhtNode self,
                             bool client_only, td::actor::ActorId<DhtMember> node, td::actor::ActorId<adnl::Adnl> adnl,
                             td::Promise<td::Unit> promise)
    : print_id_(print_id)
    , k_(k)
    , a_(a)
    , our_network_id_(our_network_id)
    , promise_(std::move(promise))
    , value_(std::move(key_value))
    , list_(std::move(list))
    , self_(std::move(self))
    , client_only_(client_only) {
  node_ = node;
  adnl_ = adnl;
  src_ = src;
}

void DhtQueryStore::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<DhtNodesList> res) {
    td::actor::send_closure(SelfId, &DhtQueryStore::send_stores, std::move(res));
  });

  auto key = value_.key_id();
  auto A = td::actor::create_actor<DhtQueryFindNodes>("FindNodesQuery", key, print_id_, src_, std::move(list_), k_, a_,
                                                      our_network_id_, self_.clone(), client_only_, node_, adnl_,
                                                      std::move(P));
  A.release();
}

void DhtQueryStore::send_stores(td::Result<DhtNodesList> R) {
  if (R.is_error()) {
    auto S = R.move_as_error();
    VLOG(DHT_NOTICE) << this << ": failed to get nearest nodes to " << value_.key_id() << ": " << S;
    promise_.set_error(std::move(S));
    stop();
    return;
  }
  auto list = R.move_as_ok();
  if (list.size() < k_) {
    td::actor::send_closure(node_, &DhtMember::store_in, value_.clone());
  } else {
    auto last_key = list.list().rbegin()->get_key();
    auto value_key = value_.key_id();
    if ((value_key ^ src_) < (value_key ^ last_key)) {
      td::actor::send_closure(node_, &DhtMember::store_in, value_.clone());
    }
  }

  remaining_ = static_cast<td::uint32>(list.size());

  for (auto &node : list.list()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &DhtQueryStore::store_ready, std::move(R));
    });
    auto M = create_serialize_tl_object<ton_api::dht_store>(value_.tl());
    td::actor::send_closure(adnl_, &adnl::Adnl::send_query, src_, node.adnl_id().compute_short_id(), "dht store",
                            std::move(P), td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), std::move(M));
  }
}

void DhtQueryStore::store_ready(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    fail_++;
    VLOG(DHT_INFO) << this << ": failed store query: " << R.move_as_error();
  } else {
    auto R2 = fetch_tl_object<ton_api::dht_stored>(R.move_as_ok(), true);
    if (R2.is_error()) {
      fail_++;
      VLOG(DHT_WARNING) << this << ": can not parse answer (expected dht.stored): " << R2.move_as_error();
    } else {
      success_++;
    }
  }
  CHECK(remaining_ > 0);
  remaining_--;
  if (remaining_ == 0) {
    if (success_ > 0) {
      promise_.set_value(td::Unit());
    } else {
      promise_.set_result(td::Status::Error("failed to make actual store query"));
    }
    stop();
  }
}

DhtQueryRegisterReverseConnection::DhtQueryRegisterReverseConnection(
    DhtKeyId key_id, adnl::AdnlNodeIdFull client, td::uint32 ttl, td::BufferSlice signature,
    DhtMember::PrintId print_id, adnl::AdnlNodeIdShort src, DhtNodesList list, td::uint32 k, td::uint32 a,
    td::int32 our_network_id, DhtNode self, bool client_only, td::actor::ActorId<DhtMember> node,
    td::actor::ActorId<adnl::Adnl> adnl, td::Promise<td::Unit> promise)
    : print_id_(print_id)
    , k_(k)
    , a_(a)
    , our_network_id_(our_network_id)
    , promise_(std::move(promise))
    , key_id_(key_id)
    , list_(std::move(list))
    , self_(std::move(self))
    , client_only_(client_only) {
  node_ = node;
  adnl_ = adnl;
  src_ = src;
  query_ = create_serialize_tl_object<ton_api::dht_registerReverseConnection>(client.tl(), ttl, std::move(signature));
}

void DhtQueryRegisterReverseConnection::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<DhtNodesList> res) {
    td::actor::send_closure(SelfId, &DhtQueryRegisterReverseConnection::send_queries, std::move(res));
  });

  auto A = td::actor::create_actor<DhtQueryFindNodes>("FindNodesQuery", key_id_, print_id_, src_, std::move(list_), k_,
                                                      a_, our_network_id_, self_.clone(), client_only_, node_, adnl_,
                                                      std::move(P));
  A.release();
}

void DhtQueryRegisterReverseConnection::send_queries(td::Result<DhtNodesList> R) {
  if (R.is_error()) {
    auto S = R.move_as_error();
    VLOG(DHT_NOTICE) << this << ": failed to get nearest nodes to " << key_id_ << ": " << S;
    promise_.set_error(std::move(S));
    stop();
    return;
  }
  auto list = R.move_as_ok();

  remaining_ = static_cast<td::uint32>(list.size());
  if (remaining_ == 0) {
    VLOG(DHT_NOTICE) << this << ": failed to get nearest nodes to " << key_id_ << ": no nodes";
    promise_.set_error(td::Status::Error("no dht nodes"));
    stop();
    return;
  }

  for (auto &node : list.list()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &DhtQueryRegisterReverseConnection::ready, std::move(R));
    });
    td::actor::send_closure(adnl_, &adnl::Adnl::send_query, src_, node.adnl_id().compute_short_id(), "dht regrevcon",
                            std::move(P), td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), query_.clone());
  }
}

void DhtQueryRegisterReverseConnection::ready(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    fail_++;
    VLOG(DHT_INFO) << this << ": failed register reverse connection query: " << R.move_as_error();
  } else {
    auto R2 = fetch_tl_object<ton_api::dht_stored>(R.move_as_ok(), true);
    if (R2.is_error()) {
      fail_++;
      VLOG(DHT_WARNING) << this << ": can not parse answer (expected dht.stored): " << R2.move_as_error();
    } else {
      success_++;
    }
  }
  CHECK(remaining_ > 0);
  remaining_--;
  if (remaining_ == 0) {
    if (success_ > 0) {
      promise_.set_value(td::Unit());
    } else {
      promise_.set_result(td::Status::Error("failed to make actual query"));
    }
    stop();
  }
}

void DhtQueryRequestReversePing::send_one_query(adnl::AdnlNodeIdShort id) {
  td::BufferSlice B;
  if (client_only_) {
    B = query_.clone();
  } else {
    B = create_serialize_tl_object_suffix<ton_api::dht_query>(query_.as_slice(), self_.tl());
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), dst = id](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &DhtQueryRequestReversePing::on_result, std::move(R), dst);
  });
  td::actor::send_closure(adnl_, &adnl::Adnl::send_query, get_src(), id, "dht requestReversePing", std::move(P),
                          td::Timestamp::in(2.0 + td::Random::fast(0, 20) * 0.1), std::move(B));
}

void DhtQueryRequestReversePing::on_result(td::Result<td::BufferSlice> R, adnl::AdnlNodeIdShort dst) {
  if (R.is_error()) {
    VLOG(DHT_INFO) << this << ": failed reverse ping query " << get_src() << "->" << dst << ": " << R.move_as_error();
    finish_query();
    return;
  }
  auto Res = fetch_tl_object<ton_api::dht_ReversePingResult>(R.move_as_ok(), true);
  if (Res.is_error()) {
    VLOG(DHT_WARNING) << this << ": dropping incorrect answer on dht.requestReversePing query from " << dst << ": "
                      << Res.move_as_error();
    finish_query();
    return;
  }

  auto A = Res.move_as_ok();
  ton_api::downcast_call(*A, td::overloaded(
                                 [&](ton_api::dht_reversePingOk &v) {
                                   promise_.set_value(td::Unit());
                                   stop();
                                 },
                                 [&](ton_api::dht_clientNotFound &v) {
                                   add_nodes(DhtNodesList{std::move(v.nodes_), our_network_id()});
                                   finish_query();
                                 }));
}

void DhtQueryRequestReversePing::finish(DhtNodesList list) {
  promise_.set_error(td::Status::Error(ErrorCode::notready, "dht key not found"));
}

}  // namespace dht

}  // namespace ton
