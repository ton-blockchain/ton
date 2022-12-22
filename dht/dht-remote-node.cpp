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

#include "td/utils/format.h"

#include "auto/tl/ton_api.hpp"

#include "dht-remote-node.hpp"

namespace ton {

namespace dht {

static const double PING_INTERVAL_DEFAULT = 60.0;
static const double PING_INTERVAL_MULTIPLIER = 1.1;
static const double PING_INTERVAL_MAX = 3600.0 * 4;

DhtRemoteNode::DhtRemoteNode(DhtNode node, td::uint32 max_missed_pings, td::int32 our_network_id)
    : node_(std::move(node))
    , max_missed_pings_(max_missed_pings)
    , our_network_id_(our_network_id)
    , ping_interval_(PING_INTERVAL_DEFAULT) {
  failed_from_ = td::Time::now_cached();
  id_ = node_.get_key();
}

td::Status DhtRemoteNode::receive_ping(DhtNode node, td::actor::ActorId<adnl::Adnl> adnl,
                                       adnl::AdnlNodeIdShort self_id) {
  TRY_STATUS(update_value(std::move(node), adnl, self_id));
  receive_ping();
  return td::Status::OK();
}

void DhtRemoteNode::receive_ping() {
  missed_pings_ = 0;
  ping_interval_ = PING_INTERVAL_DEFAULT;
  if (ready_from_ == 0) {
    ready_from_ = td::Time::now_cached();
  }
}

td::Status DhtRemoteNode::update_value(DhtNode node, td::actor::ActorId<adnl::Adnl> adnl,
                                       adnl::AdnlNodeIdShort self_id) {
  if (node.adnl_id() != node_.adnl_id()) {
    return td::Status::Error("Wrong adnl id");
  }
  if (node.version() <= node_.version()) {
    return td::Status::OK();
  }
  TRY_STATUS(node.check_signature());

  node_ = std::move(node);
  td::actor::send_closure(adnl, &adnl::Adnl::add_peer, self_id, node_.adnl_id(), node_.addr_list());
  return td::Status::OK();
}

void DhtRemoteNode::send_ping(bool client_only, td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<DhtMember> node,
                              adnl::AdnlNodeIdShort src) {
  missed_pings_++;
  if (missed_pings_ > max_missed_pings_) {
    ping_interval_ = std::min(ping_interval_ * PING_INTERVAL_MULTIPLIER, PING_INTERVAL_MAX);
    if (ready_from_ > 0) {
      ready_from_ = 0;
      failed_from_ = td::Time::now_cached();
    }
  }

  last_ping_at_ = td::Time::now_cached();

  td::actor::send_closure(adnl, &adnl::Adnl::add_peer, src, node_.adnl_id(), node_.addr_list());

  auto P = td::PromiseCreator::lambda([key = id_, id = node_.adnl_id().compute_short_id(), client_only, node, src, adnl,
                                       our_network_id = our_network_id_](td::Result<DhtNode> R) mutable {
    if (R.is_error()) {
      LOG(ERROR) << "[dht]: failed to get self node";
      return;
    }
    auto P = td::PromiseCreator::lambda([key, node, adnl, our_network_id](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        VLOG(DHT_INFO) << "[dht]: received error for query to " << key << ": " << R.move_as_error();
        return;
      }
      auto F = fetch_tl_object<ton_api::dht_node>(R.move_as_ok(), true);

      if (F.is_ok()) {
        auto N = DhtNode::create(F.move_as_ok(), our_network_id);
        if (N.is_ok()) {
          td::actor::send_closure(node, &DhtMember::receive_ping, key, N.move_as_ok());
        } else {
          VLOG(DHT_WARNING) << "[dht]: bad answer from " << key
                            << ": dropping bad getSignedAddressList() query answer: " << N.move_as_error();
        }
      } else {
        VLOG(DHT_WARNING) << "[dht]: bad answer from " << key
                          << ": dropping invalid getSignedAddressList() query answer: " << F.move_as_error();
      }
    });
    auto Q = create_serialize_tl_object<ton_api::dht_getSignedAddressList>();
    td::BufferSlice B;
    if (client_only) {
      B = std::move(Q);
    } else {
      B = create_serialize_tl_object_suffix<ton_api::dht_query>(Q.as_slice(), R.move_as_ok().tl());
    }
    td::actor::send_closure(adnl, &adnl::Adnl::send_query, src, id, "dht ping", std::move(P),
                            td::Timestamp::in(10.0 + td::Random::fast(0, 100) * 0.1), std::move(B));
  });

  td::actor::send_closure(node, &DhtMember::get_self_node, std::move(P));
}

adnl::AdnlAddressList DhtRemoteNode::get_addr_list() const {
  return node_.addr_list();
}

adnl::AdnlNodeIdFull DhtRemoteNode::get_full_id() const {
  return node_.adnl_id();
}

td::Result<std::unique_ptr<DhtRemoteNode>> DhtRemoteNode::create(DhtNode node, td::uint32 max_missed_pings,
                                                                 td::int32 our_network_id) {
  TRY_RESULT(enc, node.adnl_id().pubkey().create_encryptor());
  auto tl = node.tl();
  auto sig = std::move(tl->signature_);

  TRY_STATUS_PREFIX(enc->check_signature(serialize_tl_object(tl, true).as_slice(), sig.as_slice()),
                    "bad node signature: ");

  return std::make_unique<DhtRemoteNode>(std::move(node), max_missed_pings, our_network_id);
}

}  // namespace dht

}  // namespace ton
