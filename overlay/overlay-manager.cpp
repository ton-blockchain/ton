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
#include "overlay-manager.h"
#include "auto/tl/ton_api.h"
#include "overlay.h"

#include "adnl/utils.hpp"
#include "td/actor/actor.h"
#include "td/actor/common.h"
#include "td/utils/Random.h"

#include "td/db/RocksDb.h"

#include "td/utils/Status.h"
#include "td/utils/overloaded.h"

#include "keys/encryptor.h"
#include "td/utils/port/Poll.h"
#include <vector>

namespace ton {

namespace overlay {

void OverlayManager::update_dht_node(td::actor::ActorId<dht::Dht> dht) {
  dht_node_ = dht;
  for (auto &X : overlays_) {
    for (auto &Y : X.second) {
      td::actor::send_closure(Y.second, &Overlay::update_dht_node, dht);
    }
  }
}

void OverlayManager::register_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                      td::actor::ActorOwn<Overlay> overlay) {
  auto it = overlays_.find(local_id);
  VLOG(OVERLAY_INFO) << this << ": registering overlay " << overlay_id << "@" << local_id;
  if (it == overlays_.end()) {
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id,
                            adnl::Adnl::int_to_bytestring(ton_api::overlay_message::ID),
                            std::make_unique<AdnlCallback>(actor_id(this)));
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id,
                            adnl::Adnl::int_to_bytestring(ton_api::overlay_query::ID),
                            std::make_unique<AdnlCallback>(actor_id(this)));
  }
  overlays_[local_id][overlay_id] = std::move(overlay);

  auto P = td::PromiseCreator::lambda([id = overlays_[local_id][overlay_id].get()](td::Result<DbType::GetResult> R) {
    R.ensure();
    auto value = R.move_as_ok();
    if (value.status == td::KeyValue::GetStatus::Ok) {
      auto F = fetch_tl_object<ton_api::overlay_db_nodes>(std::move(value.value), true);
      F.ensure();
      auto nodes = std::move(F.move_as_ok()->nodes_);
      td::actor::send_closure(id, &Overlay::receive_nodes_from_db, std::move(nodes));
    }
  });
  auto key = create_hash_tl_object<ton_api::overlay_db_key_nodes>(local_id.bits256_value(), overlay_id.bits256_value());
  db_.get(key, std::move(P));
}

void OverlayManager::delete_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id) {
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    it->second.erase(overlay_id);
    if (it->second.size() == 0) {
      td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id,
                              adnl::Adnl::int_to_bytestring(ton_api::overlay_message::ID));
      td::actor::send_closure(adnl_, &adnl::Adnl::unsubscribe, local_id,
                              adnl::Adnl::int_to_bytestring(ton_api::overlay_query::ID));
      overlays_.erase(it);
    }
  }
}

void OverlayManager::create_public_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                           std::unique_ptr<Callback> callback, OverlayPrivacyRules rules,
                                           td::string scope) {
  create_public_overlay_ex(local_id, std::move(overlay_id), std::move(callback), std::move(rules), std::move(scope),
                           true);
}

void OverlayManager::create_public_overlay_ex(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                              std::unique_ptr<Callback> callback, OverlayPrivacyRules rules,
                                              td::string scope, bool announce_self) {
  CHECK(!dht_node_.empty());
  auto id = overlay_id.compute_short_id();
  register_overlay(local_id, id,
                   Overlay::create(keyring_, adnl_, actor_id(this), dht_node_, local_id, std::move(overlay_id),
                                   std::move(callback), std::move(rules), scope, announce_self));
}

void OverlayManager::create_private_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                            std::vector<adnl::AdnlNodeIdShort> nodes,
                                            std::unique_ptr<Callback> callback, OverlayPrivacyRules rules) {
  auto id = overlay_id.compute_short_id();
  register_overlay(local_id, id,
                   Overlay::create(keyring_, adnl_, actor_id(this), dht_node_, local_id, std::move(overlay_id),
                                   std::move(nodes), std::move(callback), std::move(rules)));
}

void OverlayManager::receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  auto R = fetch_tl_prefix<ton_api::overlay_message>(data, true);

  if (R.is_error()) {
    VLOG(OVERLAY_WARNING) << this << ": can not parse overlay message: " << R.move_as_error();
    return;
  }

  auto M = R.move_as_ok();

  auto it = overlays_.find(dst);
  if (it == overlays_.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": message to unknown overlay " << M->overlay_ << "@" << dst;
    return;
  }
  auto it2 = it->second.find(OverlayIdShort{M->overlay_});
  if (it2 == it->second.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": message to localid is not in overlay " << M->overlay_ << "@" << dst;
    return;
  }

  td::actor::send_closure(it2->second, &Overlay::update_throughput_in_ctr, src, (td::uint32)data.size(), false);
  td::actor::send_closure(it2->second, &Overlay::receive_message, src, std::move(data));
}

void OverlayManager::receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                                   td::Promise<td::BufferSlice> promise) {
  auto R = fetch_tl_prefix<ton_api::overlay_query>(data, true);

  if (R.is_error()) {
    VLOG(OVERLAY_WARNING) << this << ": can not parse overlay query [" << src << "->" << dst
                          << "]: " << R.move_as_error();
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad overlay query header"));
    return;
  }

  auto M = R.move_as_ok();

  auto it = overlays_.find(dst);
  if (it == overlays_.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": query to unknown overlay " << M->overlay_ << "@" << dst << " from " << src;
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad local_id " << dst));
    return;
  }
  auto it2 = it->second.find(OverlayIdShort{M->overlay_});
  if (it2 == it->second.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": query to localid not in overlay " << M->overlay_ << "@" << dst << " from " << src;
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad overlay_id " << M->overlay_));
    return;
  }

  td::actor::send_closure(it2->second, &Overlay::update_throughput_in_ctr, src, (td::uint32)data.size(), true);
  td::actor::send_closure(it2->second, &Overlay::receive_query, src, std::move(data), std::move(promise));
}

void OverlayManager::send_query_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                                    std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                    td::BufferSlice query, td::uint64 max_answer_size,
                                    td::actor::ActorId<adnl::AdnlSenderInterface> via) {
  CHECK(query.size() <= adnl::Adnl::huge_packet_max_size());
  
  auto it = overlays_.find(src);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::update_throughput_out_ctr, dst, (td::uint32)query.size(), true);
    }
  }
  
  td::actor::send_closure(
      via, &adnl::AdnlSenderInterface::send_query_ex, src, dst, std::move(name), std::move(promise), timeout,
      create_serialize_tl_object_suffix<ton_api::overlay_query>(query.as_slice(), overlay_id.tl()), max_answer_size);
}

void OverlayManager::send_message_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                                      td::BufferSlice object, td::actor::ActorId<adnl::AdnlSenderInterface> via) {
  CHECK(object.size() <= adnl::Adnl::huge_packet_max_size());
  
  auto it = overlays_.find(src);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::update_throughput_out_ctr, dst, (td::uint32)object.size(), false);
    }
  }
  
  td::actor::send_closure(
      via, &adnl::AdnlSenderInterface::send_message, src, dst,
      create_serialize_tl_object_suffix<ton_api::overlay_message>(object.as_slice(), overlay_id.tl()));
}

void OverlayManager::send_broadcast(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, td::BufferSlice object) {
  send_broadcast_ex(local_id, overlay_id, local_id.pubkey_hash(), 0, std::move(object));
}

void OverlayManager::send_broadcast_ex(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, PublicKeyHash send_as,
                                       td::uint32 flags, td::BufferSlice object) {
  CHECK(object.size() <= Overlays::max_simple_broadcast_size());
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::send_broadcast, send_as, flags, std::move(object));
    }
  }
}

void OverlayManager::send_broadcast_fec(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                        td::BufferSlice object) {
  send_broadcast_fec_ex(local_id, overlay_id, local_id.pubkey_hash(), 0, std::move(object));
}

void OverlayManager::send_broadcast_fec_ex(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                           PublicKeyHash send_as, td::uint32 flags, td::BufferSlice object) {
  CHECK(object.size() <= Overlays::max_fec_broadcast_size());
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::send_broadcast_fec, send_as, flags, std::move(object));
    }
  }
}

void OverlayManager::set_privacy_rules(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                       OverlayPrivacyRules rules) {
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::set_privacy_rules, std::move(rules));
    }
  }
}

void OverlayManager::update_certificate(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, PublicKeyHash key,
                                        std::shared_ptr<Certificate> cert) {
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::add_certificate, key, std::move(cert));
    }
  }
}

void OverlayManager::get_overlay_random_peers(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                              td::uint32 max_peers,
                                              td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  auto it = overlays_.find(local_id);
  if (it != overlays_.end()) {
    auto it2 = it->second.find(overlay_id);
    if (it2 != it->second.end()) {
      td::actor::send_closure(it2->second, &Overlay::get_overlay_random_peers, max_peers, std::move(promise));
    }
  }
}

td::actor::ActorOwn<Overlays> Overlays::create(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring,
                                               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<dht::Dht> dht) {
  return td::actor::create_actor<OverlayManager>("overlaymanager", db_root, keyring, adnl, dht);
}

OverlayManager::OverlayManager(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring,
                               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<dht::Dht> dht)
    : db_root_(db_root), keyring_(keyring), adnl_(adnl), dht_node_(dht) {
}

void OverlayManager::start_up() {
  std::shared_ptr<td::KeyValue> kv =
      std::make_shared<td::RocksDb>(td::RocksDb::open(PSTRING() << db_root_ << "/overlays").move_as_ok());
  db_ = DbType{std::move(kv)};
}

void OverlayManager::save_to_db(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                std::vector<OverlayNode> nodes) {
  std::vector<tl_object_ptr<ton_api::overlay_node>> nodes_vec;
  for (auto &n : nodes) {
    nodes_vec.push_back(n.tl());
  }
  auto obj = create_tl_object<ton_api::overlay_nodes>(std::move(nodes_vec));

  auto key = create_hash_tl_object<ton_api::overlay_db_key_nodes>(local_id.bits256_value(), overlay_id.bits256_value());
  db_.set(key, create_serialize_tl_object<ton_api::overlay_db_nodes>(std::move(obj)));
}

void OverlayManager::get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlaysStats>> promise) {
  class Cb : public td::actor::Actor {
   public:
    Cb(td::Promise<tl_object_ptr<ton_api::engine_validator_overlaysStats>> promise) : promise_(std::move(promise)) {
    }
    void incr_pending() {
      pending_++;
    }
    void decr_pending() {
      if (!--pending_) {
        promise_.set_result(create_tl_object<ton_api::engine_validator_overlaysStats>(std::move(res_)));
        stop();
      }
    }
    void receive_answer(tl_object_ptr<ton_api::engine_validator_overlayStats> res) {
      if (res) {
        res_.push_back(std::move(res));
      }
      decr_pending();
    }

   private:
    std::vector<tl_object_ptr<ton_api::engine_validator_overlayStats>> res_;
    size_t pending_{1};
    td::Promise<tl_object_ptr<ton_api::engine_validator_overlaysStats>> promise_;
  };

  auto act = td::actor::create_actor<Cb>("overlaysstatsmerger", std::move(promise)).release();

  for (auto &a : overlays_) {
    for (auto &b : a.second) {
      td::actor::send_closure(act, &Cb::incr_pending);
      td::actor::send_closure(b.second, &Overlay::get_stats,
                              [act](td::Result<tl_object_ptr<ton_api::engine_validator_overlayStats>> R) {
                                if (R.is_ok()) {
                                  td::actor::send_closure(act, &Cb::receive_answer, R.move_as_ok());
                                } else {
                                  td::actor::send_closure(act, &Cb::receive_answer, nullptr);
                                }
                              });
    }
  }

  td::actor::send_closure(act, &Cb::decr_pending);
}

Certificate::Certificate(PublicKey issued_by, td::int32 expire_at, td::uint32 max_size, td::uint32 flags,
                         td::BufferSlice signature)
    : issued_by_(issued_by)
    , expire_at_(expire_at)
    , max_size_(max_size)
    , flags_(flags)
    , signature_(td::SharedSlice(signature.as_slice())) {
}

Certificate::Certificate(PublicKeyHash issued_by, td::int32 expire_at, td::uint32 max_size, td::uint32 flags,
                         td::BufferSlice signature)
    : issued_by_(issued_by)
    , expire_at_(expire_at)
    , max_size_(max_size)
    , flags_(flags)
    , signature_(td::SharedSlice(signature.as_slice())) {
}

void Certificate::set_signature(td::BufferSlice signature) {
  signature_ = td::SharedSlice{signature.as_slice()};
}

void Certificate::set_issuer(PublicKey issuer) {
  issued_by_ = issuer;
}

constexpr td::uint32 cert_default_flags(td::uint32 max_size) {
  return (max_size > Overlays::max_simple_broadcast_size() ? CertificateFlags::AllowFec : 0) |
         CertificateFlags::Trusted;
}

td::BufferSlice Certificate::to_sign(OverlayIdShort overlay_id, PublicKeyHash issued_to) const {
  if (flags_ == cert_default_flags(max_size_)) {
    return create_serialize_tl_object<ton_api::overlay_certificateId>(overlay_id.tl(), issued_to.tl(), expire_at_,
                                                                      max_size_);
  } else {
    return create_serialize_tl_object<ton_api::overlay_certificateIdV2>(overlay_id.tl(), issued_to.tl(), expire_at_,
                                                                        max_size_, flags_);
  }
}

const PublicKeyHash Certificate::issuer_hash() const {
  PublicKeyHash r;
  issued_by_.visit(
      td::overloaded([&](const PublicKeyHash &x) { r = x; }, [&](const PublicKey &x) { r = x.compute_short_id(); }));
  return r;
}
const PublicKey &Certificate::issuer() const {
  return issued_by_.get<PublicKey>();
}

td::Result<std::shared_ptr<Certificate>> Certificate::create(tl_object_ptr<ton_api::overlay_Certificate> cert) {
  std::shared_ptr<Certificate> res;
  ton_api::downcast_call(*cert.get(),
                         td::overloaded([&](ton_api::overlay_emptyCertificate &obj) { res = nullptr; },
                                        [&](ton_api::overlay_certificate &obj) {
                                          res = std::make_shared<Certificate>(PublicKey{obj.issued_by_}, obj.expire_at_,
                                                                              static_cast<td::uint32>(obj.max_size_),
                                                                              cert_default_flags(obj.max_size_),
                                                                              std::move(obj.signature_));
                                        },
                                        [&](ton_api::overlay_certificateV2 &obj) {
                                          res = std::make_shared<Certificate>(PublicKey{obj.issued_by_}, obj.expire_at_,
                                                                              static_cast<td::uint32>(obj.max_size_),
                                                                              static_cast<td::uint32>(obj.flags_),
                                                                              std::move(obj.signature_));
                                        }));
  return std::move(res);
}

BroadcastCheckResult Certificate::check(PublicKeyHash node, OverlayIdShort overlay_id, td::int32 unix_time,
                                        td::uint32 size, bool is_fec) const {
  if (size > max_size_) {
    return BroadcastCheckResult::Forbidden;
  }
  if (unix_time > expire_at_) {
    return BroadcastCheckResult::Forbidden;
  }
  if (is_fec && !(flags_ & CertificateFlags::AllowFec)) {
    return BroadcastCheckResult::Forbidden;
  }

  auto R1 = issued_by_.get<PublicKey>().create_encryptor();
  if (R1.is_error()) {
    return BroadcastCheckResult::Forbidden;
  }
  auto E = R1.move_as_ok();

  auto B = to_sign(overlay_id, node);

  if (E->check_signature(B.as_slice(), signature_.as_slice()).is_error()) {
    return BroadcastCheckResult::Forbidden;
  }

  return (flags_ & CertificateFlags::Trusted) ? BroadcastCheckResult::Allowed : BroadcastCheckResult::NeedCheck;
}

tl_object_ptr<ton_api::overlay_Certificate> Certificate::tl() const {
  return create_tl_object<ton_api::overlay_certificate>(issued_by_.get<PublicKey>().tl(), expire_at_, max_size_,
                                                        signature_.clone_as_buffer_slice());
}

tl_object_ptr<ton_api::overlay_Certificate> Certificate::empty_tl() {
  return create_tl_object<ton_api::overlay_emptyCertificate>();
}

}  // namespace overlay

}  // namespace ton
