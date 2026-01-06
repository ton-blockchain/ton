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

#include "adnl/adnl-node-id.hpp"
#include "common/util.h"
#include "keys/encryptor.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/port/Stat.h"

#include "broadcast-simple.hpp"
#include "overlay.hpp"

namespace ton {

namespace overlay {

static constexpr std::size_t MAX_BCASTS = 100;

static Overlay::BroadcastHash compute_broadcast_id(PublicKeyHash source, Overlay::BroadcastDataHash data_hash,
                                                   td::uint32 flags) {
  auto obj = create_tl_object<ton_api::overlay_broadcast_id>(
      (flags & Overlays::BroadcastFlagAnySender()) ? PublicKeyHash::zero().tl() : source.tl(), data_hash, flags);
  return get_tl_object_sha_bits256(obj);
}

class BroadcastSimple : public td::ListNode {
  friend class BroadcastsSimple;

 public:
  BroadcastSimple(Overlay::BroadcastHash broadcast_hash, PublicKey source, std::shared_ptr<Certificate> cert,
                  td::uint32 flags, td::BufferSlice data, td::uint32 date, td::BufferSlice signature, bool is_valid,
                  adnl::AdnlNodeIdShort src_peer_id)
      : broadcast_hash_(broadcast_hash)
      , source_(std::move(source))
      , cert_(std::move(cert))
      , flags_(flags)
      , data_(std::move(data))
      , date_(date)
      , signature_(std::move(signature))
      , is_valid_(is_valid)
      , src_peer_id_(src_peer_id) {
  }

  td::Status run(OverlayImpl *overlay);
  void checked(OverlayImpl *overlay, td::Result<td::Unit> R);
  void run_continue(OverlayImpl *overlay);

  td::BufferSlice serialize();
  td::BufferSlice to_sign();

  static BroadcastSimple *from_list_node(ListNode *node) {
    return static_cast<BroadcastSimple *>(node);
  }

 private:
  Overlay::BroadcastHash broadcast_hash_;
  PublicKey source_;
  std::shared_ptr<Certificate> cert_;
  td::uint32 flags_;
  td::BufferSlice data_;
  td::uint32 date_;
  td::BufferSlice signature_;
  bool is_valid_;
  adnl::AdnlNodeIdShort src_peer_id_;
};

td::Status BroadcastSimple::run(OverlayImpl *overlay) {
  auto r = overlay->check_source_eligible(source_, cert_.get(), static_cast<td::uint32>(data_.size()), false);
  if (r == BroadcastCheckResult::Forbidden) {
    return td::Status::Error(ErrorCode::error, "broadcast is forbidden");
  }
  is_valid_ = r == BroadcastCheckResult::Allowed;
  TRY_RESULT(encryptor, overlay->get_encryptor(source_));
  TRY_STATUS(encryptor->check_signature(to_sign().as_slice(), signature_.as_slice()));
  if (!is_valid_) {
    auto P = td::PromiseCreator::lambda(
        [overlay = actor_id(overlay), hash = broadcast_hash_](td::Result<td::Unit> R) mutable {
          td::actor::send_closure(overlay, &OverlayImpl::broadcast_simple_checked, std::move(hash), std::move(R));
        });
    overlay->check_broadcast(source_.compute_short_id(), data_.clone(), std::move(P));
  } else {
    run_continue(overlay);
  }
  return td::Status::OK();
}

void BroadcastSimple::checked(OverlayImpl *overlay, td::Result<td::Unit> R) {
  if (R.is_error()) {
    overlay->update_peer_err_ctr(src_peer_id_, false);
    return;
  }
  is_valid_ = true;
  run_continue(overlay);
}

void BroadcastSimple::run_continue(OverlayImpl *overlay) {
  auto B = serialize();
  auto nodes = overlay->get_neighbours(overlay->propagate_broadcast_to());
  auto manager = overlay->overlay_manager();
  for (auto &n : nodes) {
    td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay->local_id(), overlay->overlay_id(),
                            B.clone());
  }
  overlay->deliver_broadcast(source_.compute_short_id(), data_.clone());
}

td::BufferSlice BroadcastSimple::serialize() {
  return create_serialize_tl_object<ton_api::overlay_broadcast>(
      source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(), flags_, data_.clone(), date_, signature_.clone());
}

td::BufferSlice BroadcastSimple::to_sign() {
  return create_serialize_tl_object<ton_api::overlay_broadcast_toSign>(broadcast_hash_, date_);
}

BroadcastsSimple::BroadcastsSimple() = default;

BroadcastsSimple::~BroadcastsSimple() = default;

void BroadcastsSimple::send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags) {
  auto data_hash = sha256_bits256(data.as_slice());
  auto broadcast_hash = compute_broadcast_id(send_as, data_hash, flags);
  if (has(broadcast_hash) || overlay->is_delivered(broadcast_hash)) {
    LOG(DEBUG) << "failed to send simple broadcast: duplicate broadcast";
    return;
  }
  auto date = static_cast<td::uint32>(td::Clocks::system());
  auto bcast = std::make_unique<BroadcastSimple>(broadcast_hash, PublicKey{}, nullptr, flags, std::move(data), date,
                                                 td::BufferSlice{}, false, adnl::AdnlNodeIdShort::zero());
  auto to_sign = bcast->to_sign();
  auto P = td::PromiseCreator::lambda([overlay = actor_id(overlay), bcast = std::move(bcast)](
                                          td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
    td::actor::send_closure(overlay, &OverlayImpl::broadcast_simple_signed, std::move(bcast), std::move(R));
  });
  td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                          std::move(P));
}

void BroadcastsSimple::signed_(OverlayImpl *overlay, std::unique_ptr<BroadcastSimple> &&bcast,
                               td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (R.is_error()) {
    td::Status reason = R.move_as_error();
    if (reason.code() == ErrorCode::notready) {
      LOG(DEBUG) << "failed to send simple broadcast: " << reason;
    } else {
      LOG(WARNING) << "failed to send simple broadcast: " << reason;
    }
    return;
  }
  auto V = R.move_as_ok();
  bcast->source_ = std::move(V.second);
  bcast->signature_ = std::move(V.first);
  bcast->cert_ = overlay->get_certificate(bcast->source_.compute_short_id());
  auto S = bcast->run(overlay);
  if (S.is_error() && S.code() != ErrorCode::notready) {
    LOG(WARNING) << "failed to send simple broadcast: " << S;
  }
  register_(overlay, std::move(bcast));
}

td::Status BroadcastsSimple::process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                               tl_object_ptr<ton_api::overlay_broadcast> broadcast) {
  TRY_STATUS(overlay->check_date(broadcast->date_));
  auto src = PublicKey{broadcast->src_};
  auto data_hash = sha256_bits256(broadcast->data_.as_slice());
  auto broadcast_hash = compute_broadcast_id(src.compute_short_id(), data_hash, broadcast->flags_);
  if (has(broadcast_hash) || overlay->is_delivered(broadcast_hash)) {
    return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
  }
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));
  auto B = std::make_unique<BroadcastSimple>(broadcast_hash, src, std::move(cert), broadcast->flags_,
                                             std::move(broadcast->data_), broadcast->date_,
                                             std::move(broadcast->signature_), false, src_peer_id);
  TRY_STATUS(B->run(overlay));
  register_(overlay, std::move(B));
  return td::Status::OK();
}

void BroadcastsSimple::process_query(adnl::AdnlNodeIdShort src, ton_api::overlay_getBroadcast &query,
                                     td::Promise<td::BufferSlice> promise) {
  auto it = broadcasts_.find(query.hash_);
  if (it == broadcasts_.end()) {
    VLOG(OVERLAY_NOTICE) << this << ": received getBroadcastQuery(" << query.hash_ << ") from " << src
                         << " but broadcast is unknown";
    promise.set_value(create_serialize_tl_object<ton_api::overlay_broadcastNotFound>());
    return;
  }
  VLOG(OVERLAY_DEBUG) << this << ": received getBroadcastQuery(" << query.hash_ << ") from " << src
                      << " sending broadcast";
  promise.set_value(it->second->serialize());
}

void BroadcastsSimple::checked(OverlayImpl *overlay, Overlay::BroadcastHash &&hash, td::Result<td::Unit> &&R) {
  auto it = broadcasts_.find(hash);
  if (it != broadcasts_.end()) {
    it->second->checked(overlay, std::move(R));
  }
}

void BroadcastsSimple::gc(OverlayImpl *overlay) {
  while (broadcasts_.size() > MAX_BCASTS) {
    auto bcast = BroadcastSimple::from_list_node(lru_.get());
    CHECK(bcast);
    auto hash = bcast->broadcast_hash_;
    broadcasts_.erase(hash);
    overlay->register_delivered_broadcast(hash);
  }
}

bool BroadcastsSimple::has(const Overlay::BroadcastHash &hash) {
  return broadcasts_.find(hash) != broadcasts_.end();
}

void BroadcastsSimple::register_(OverlayImpl *overlay, std::unique_ptr<BroadcastSimple> bcast) {
  auto hash = bcast->broadcast_hash_;
  lru_.put(bcast.get());
  broadcasts_.emplace(hash, std::move(bcast));
  gc(overlay);
}

}  // namespace overlay

}  // namespace ton
