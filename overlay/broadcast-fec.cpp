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

#include "keys/encryptor.h"

#include "broadcast-fec.hpp"
#include "overlay.hpp"

namespace ton {

namespace overlay {

static Overlay::BroadcastHash compute_broadcast_id(PublicKeyHash source, const fec::FecType &fec_type,
                                                   Overlay::BroadcastDataHash data_hash, td::uint32 size,
                                                   td::uint32 flags) {
  return get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastFec_id>(
      (flags & Overlays::BroadcastFlagAnySender()) ? PublicKeyHash::zero().tl() : source.tl(),
      get_tl_object_sha_bits256(fec_type.tl()), data_hash, size, flags));
}

static Overlay::BroadcastPartHash compute_broadcast_part_id(Overlay::BroadcastHash broadcast_hash,
                                                            Overlay::BroadcastDataHash data_hash, td::uint32 seqno) {
  return get_tl_object_sha_bits256(
      create_tl_object<ton_api::overlay_broadcastFec_partId>(broadcast_hash, data_hash, seqno));
}

class BroadcastFec : public td::ListNode {
  friend class BroadcastFecPart;
  friend class BroadcastsFec;

 public:
  BroadcastFec(Overlay::BroadcastHash hash, Overlay::BroadcastDataHash data_hash, td::uint32 flags, td::uint32 date,
               PublicKey src, fec::FecType fec_type)
      : hash_(hash)
      , data_hash_(data_hash)
      , flags_(flags)
      , date_(date)
      , src_(std::move(src))
      , fec_type_(std::move(fec_type)) {
  }

  td::Status is_eligible_sender(PublicKey src) {
    if (flags_ & Overlays::BroadcastFlagAnySender()) {
      return td::Status::OK();
    } else {
      if (src == src_) {
        return td::Status::OK();
      } else {
        return td::Status::Error(ErrorCode::protoviolation, "bad source");
      }
    }
  }

  td::Status add_part(td::uint32 seqno, td::BufferSlice data, td::BufferSlice serialized_fec_part_short,
                      td::BufferSlice serialized_fec_part) {
    if (decoder_) {
      td::fec::Symbol s;
      s.id = seqno;
      s.data = std::move(data);

      decoder_->add_symbol(std::move(s));
    }
    parts_[seqno] = std::pair<td::BufferSlice, td::BufferSlice>(std::move(serialized_fec_part_short),
                                                                std::move(serialized_fec_part));

    return td::Status::OK();
  }

  td::Result<td::BufferSlice> finish() {
    CHECK(decoder_);
    if (!decoder_->may_try_decode()) {
      return td::Status::Error(ErrorCode::notready, "need more parts");
    }
    TRY_RESULT(D, decoder_->try_decode(true));
    if (sha256_bits256(D.data.as_slice()) != data_hash_) {
      return td::Status::Error(ErrorCode::protoviolation, "bad hash");
    }
    encoder_ = std::move(D.encoder);
    CHECK(encoder_ != nullptr);
    ready_ = true;
    decoder_ = nullptr;
    data_ = D.data.clone();
    return std::move(D.data);
  }

  td::BufferSlice get_part(td::uint32 seqno) {
    CHECK(ready_);
    CHECK(encoder_ != nullptr);
    auto R = encoder_->gen_symbol(seqno);
    CHECK(R.id == seqno);
    return std::move(R.data);
  }

  td::Status init_fec_type() {
    TRY_RESULT(D, fec_type_.create_decoder());
    decoder_ = std::move(D);
    return td::Status::OK();
  }

  td::Status run_checks();

  bool neighbour_received(adnl::AdnlNodeIdShort id) const {
    return received_neighbours_.find(id) != received_neighbours_.end();
  }

  void add_received(adnl::AdnlNodeIdShort id) {
    received_neighbours_.insert(id);
  }

  bool neighbour_completed(adnl::AdnlNodeIdShort id) const {
    return completed_neighbours_.find(id) != completed_neighbours_.end();
  }

  void add_completed(adnl::AdnlNodeIdShort id) {
    completed_neighbours_.insert(id);
  }

  static BroadcastFec *from_list_node(ListNode *node) {
    return static_cast<BroadcastFec *>(node);
  }

  bool received_part(td::uint32 seqno) const {
    if (seqno + 64 < next_seqno_) {
      return true;
    }
    if (seqno >= next_seqno_) {
      return false;
    }
    return received_parts_ & (1ull << (next_seqno_ - seqno - 1));
  }

  void add_received_part(td::uint32 seqno) {
    CHECK(!received_part(seqno));
    if (seqno < next_seqno_) {
      received_parts_ |= (1ull << (next_seqno_ - seqno - 1));
    } else {
      auto old = next_seqno_;
      next_seqno_ = seqno + 1;
      if (next_seqno_ - old >= 64) {
        received_parts_ = 1;
      } else {
        received_parts_ = received_parts_ << (next_seqno_ - old);
        received_parts_ |= 1;
      }
    }
  }

  void broadcast_checked(OverlayImpl *overlay, td::Result<td::Unit> R);

  void set_src_peer_id(adnl::AdnlNodeIdShort src_peer_id) {
    src_peer_id_ = src_peer_id;
  }

  td::Status distribute_part(OverlayImpl *overlay, td::uint32 seqno);

 private:
  Overlay::BroadcastHash hash_;
  Overlay::BroadcastDataHash data_hash_;
  td::uint32 flags_;
  td::uint32 date_;
  PublicKey src_;
  fec::FecType fec_type_;

  bool ready_ = false;
  bool is_checked_ = false;
  std::unique_ptr<td::fec::Decoder> decoder_;
  std::unique_ptr<td::fec::Encoder> encoder_;
  std::set<adnl::AdnlNodeIdShort> received_neighbours_;
  std::set<adnl::AdnlNodeIdShort> completed_neighbours_;
  td::uint32 next_seqno_ = 0;
  td::uint64 received_parts_ = 0;
  std::map<td::uint32, std::pair<td::BufferSlice, td::BufferSlice>> parts_;
  adnl::AdnlNodeIdShort src_peer_id_ = adnl::AdnlNodeIdShort::zero();
  td::BufferSlice data_;
};

td::Status BroadcastFec::run_checks() {
  if (fec_type_.size() > Overlays::max_fec_broadcast_size()) {
    return td::Status::Error(ErrorCode::protoviolation, "too big fec broadcast");
  }
  return td::Status::OK();
}

void BroadcastFec::broadcast_checked(OverlayImpl *overlay, td::Result<td::Unit> R) {
  if (R.is_error()) {
    td::actor::send_closure(actor_id(overlay), &OverlayImpl::update_peer_err_ctr, src_peer_id_, true);
    return;
  }
  overlay->deliver_broadcast(src_.compute_short_id(), data_.clone());
  while (!parts_.empty()) {
    distribute_part(overlay, parts_.begin()->first);
  }

  is_checked_ = true;
}

// Do we need status here??
td::Status BroadcastFec::distribute_part(OverlayImpl *overlay, td::uint32 seqno) {
  auto i = parts_.find(seqno);
  if (i == parts_.end()) {
    VLOG(OVERLAY_WARNING) << "not distibuting empty part " << seqno;
    // should not get here
    return td::Status::OK();
  }
  auto tls = std::move(i->second);
  parts_.erase(i);
  td::BufferSlice data_short = std::move(tls.first);
  td::BufferSlice data = std::move(tls.second);

  auto nodes = overlay->get_neighbours(overlay->propagate_broadcast_to());
  auto manager = overlay->overlay_manager();

  for (auto &n : nodes) {
    if (neighbour_completed(n)) {
      continue;
    }
    if (neighbour_received(n)) {
      td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay->local_id(), overlay->overlay_id(),
                              data_short.clone());
    } else {
      if (hash_.count_leading_zeroes() >= 12) {
        VLOG(OVERLAY_INFO) << "broadcast " << hash_ << ": sending part " << seqno << " to " << n;
      }
      td::actor::send_closure(manager, &OverlayManager::send_message, n, overlay->local_id(), overlay->overlay_id(),
                              data.clone());
    }
  }
  return td::Status::OK();
}

class BroadcastFecPart {
  friend class BroadcastsFec;

 public:
  BroadcastFecPart(Overlay::BroadcastHash broadcast_hash, Overlay::BroadcastPartHash part_hash, PublicKey source,
                   std::shared_ptr<Certificate> cert, Overlay::BroadcastDataHash data_hash, td::uint32 data_size,
                   td::uint32 flags, Overlay::BroadcastDataHash part_data_hash, td::BufferSlice data, td::uint32 seqno,
                   fec::FecType fec_type, td::uint32 date, td::BufferSlice signature, bool is_short,
                   adnl::AdnlNodeIdShort src_peer_id)
      : broadcast_hash_(broadcast_hash)
      , part_hash_(part_hash)
      , source_(std::move(source))
      , cert_(std::move(cert))
      , broadcast_data_hash_(data_hash)
      , broadcast_size_(data_size)
      , flags_(flags)
      , part_data_hash_(part_data_hash)
      , data_(std::move(data))
      , seqno_(seqno)
      , fec_type_(std::move(fec_type))
      , date_(date)
      , signature_(std::move(signature))
      , is_short_(is_short)
      , src_peer_id_(src_peer_id) {
  }

  td::BufferSlice to_sign();

  td::Status run_checks(OverlayImpl *overlay, BroadcastFec *bcast);
  td::Status run(OverlayImpl *overlay, BroadcastFec &bcast);

 private:
  Overlay::BroadcastHash broadcast_hash_;
  Overlay::BroadcastPartHash part_hash_;

  PublicKey source_;
  std::shared_ptr<Certificate> cert_;
  Overlay::BroadcastDataHash broadcast_data_hash_;
  td::uint32 broadcast_size_;
  td::uint32 flags_;
  Overlay::BroadcastDataHash part_data_hash_;
  td::BufferSlice data_;
  td::uint32 seqno_;
  fec::FecType fec_type_;
  td::uint32 date_;
  td::BufferSlice signature_;

  bool is_short_;
  bool untrusted_{false};

  adnl::AdnlNodeIdShort src_peer_id_ = adnl::AdnlNodeIdShort::zero();
};

td::Status BroadcastFecPart::run_checks(OverlayImpl *overlay, BroadcastFec *bcast) {
  if (bcast && bcast->received_part(seqno_)) {
    return td::Status::Error(ErrorCode::notready, "duplicate part");
  }
  auto r = overlay->check_source_eligible(source_, cert_.get(), broadcast_size_, true);
  if (r == BroadcastCheckResult::Forbidden) {
    return td::Status::Error(ErrorCode::error, "broadcast is forbidden");
  }

  if (r == BroadcastCheckResult::NeedCheck) {
    untrusted_ = true;
  } else if (bcast) {
    TRY_STATUS(bcast->is_eligible_sender(source_));
  }
  TRY_RESULT(encryptor, overlay->get_encryptor(source_));
  TRY_STATUS(encryptor->check_signature(to_sign().as_slice(), signature_.as_slice()));
  return td::Status::OK();
}

td::Status BroadcastFecPart::run(OverlayImpl *overlay, BroadcastFec &bcast) {
  if (bcast.received_part(seqno_)) {
    return td::Status::Error(ErrorCode::notready, "duplicate part");
  }
  bcast.add_received_part(seqno_);
  bcast.set_src_peer_id(src_peer_id_);

  TRY_STATUS(bcast.add_part(
      seqno_, data_.clone(),
      create_serialize_tl_object<ton_api::overlay_broadcastFecShort>(
          source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(), broadcast_hash_, part_data_hash_, seqno_,
          signature_.clone()),
      create_serialize_tl_object<ton_api::overlay_broadcastFec>(
          source_.tl(), cert_ ? cert_->tl() : Certificate::empty_tl(), bcast.data_hash_, bcast.fec_type_.size(),
          bcast.flags_, data_.clone(), seqno_, bcast.fec_type_.tl(), bcast.date_, signature_.clone())));
  if (!bcast.ready_) {
    auto R = bcast.finish();
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::notready) {
        return S;
      }
    } else {
      if (untrusted_) {
        auto P = td::PromiseCreator::lambda(
            [overlay_id = actor_id(overlay), broadcast_hash = broadcast_hash_](td::Result<td::Unit> R) mutable {
              td::actor::send_closure(overlay_id, &OverlayImpl::broadcast_fec_checked, std::move(broadcast_hash),
                                      std::move(R));
            });
        overlay->check_broadcast(bcast.src_.compute_short_id(), R.move_as_ok(), std::move(P));
      } else {
        overlay->deliver_broadcast(bcast.src_.compute_short_id(), R.move_as_ok());
      }
    }
  }
  if (!untrusted_ || bcast.is_checked_) {
    TRY_STATUS(bcast.distribute_part(overlay, seqno_));
  }
  return td::Status::OK();
}

td::BufferSlice BroadcastFecPart::to_sign() {
  auto obj = create_tl_object<ton_api::overlay_broadcast_toSign>(part_hash_, date_);
  return serialize_tl_object(obj, true);
}

class BroadcastFecActor : public td::actor::Actor {
 public:
  BroadcastFecActor(td::BufferSlice data, td::uint32 flags, td::actor::ActorId<OverlayImpl> overlay,
                    PublicKeyHash local_id, double speed_multiplier = 1.0);

  void start_up() override;
  void alarm() override;

 private:
  const td::uint32 symbol_size_ = 768;
  td::uint32 to_send_;
  td::uint32 seqno_ = 0;
  PublicKeyHash local_id_;
  Overlay::BroadcastDataHash data_hash_;
  td::uint32 flags_ = 0;
  double delay_ = 0.010;
  td::int32 date_;
  std::unique_ptr<td::fec::Encoder> encoder_;
  td::actor::ActorId<OverlayImpl> overlay_;
  fec::FecType fec_type_;
};

BroadcastFecActor::BroadcastFecActor(td::BufferSlice data, td::uint32 flags, td::actor::ActorId<OverlayImpl> overlay,
                                     PublicKeyHash local_id, double speed_multiplier)
    : flags_(flags) {
  delay_ /= speed_multiplier;
  CHECK(data.size() <= (1 << 27));
  local_id_ = local_id;
  overlay_ = std::move(overlay);
  date_ = static_cast<td::int32>(td::Clocks::system());
  to_send_ = (static_cast<td::uint32>(data.size()) / symbol_size_ + 1) * 2;

  data_hash_ = td::sha256_bits256(data);

  fec_type_ = td::fec::RaptorQEncoder::Parameters{data.size(), symbol_size_, 0};
  auto E = fec_type_.create_encoder(std::move(data));
  E.ensure();
  encoder_ = E.move_as_ok();
}

void BroadcastFecActor::start_up() {
  encoder_->prepare_more_symbols();
  alarm();
}

void BroadcastFecActor::alarm() {
  for (td::uint32 i = 0; i < 4; i++) {
    auto X = encoder_->gen_symbol(seqno_++);
    CHECK(X.data.size() <= 1000);
    td::actor::send_closure(overlay_, &OverlayImpl::send_new_fec_broadcast_part, local_id_, data_hash_,
                            fec_type_.size(), flags_, std::move(X.data), X.id, fec_type_, date_);
  }

  alarm_timestamp() = td::Timestamp::in(delay_);

  if (seqno_ >= to_send_) {
    stop();
  }
}

BroadcastsFec::BroadcastsFec() = default;

BroadcastsFec::~BroadcastsFec() = default;

void BroadcastsFec::send(OverlayImpl *overlay, PublicKeyHash send_as, td::BufferSlice data, td::uint32 flags,
                         double speed_multiplier) {
  td::actor::create_actor<BroadcastFecActor>(td::actor::ActorOptions().with_name("bcast"), std::move(data), flags,
                                             actor_id(overlay), send_as, speed_multiplier)
      .release();
}

void BroadcastsFec::send_part(OverlayImpl *overlay, PublicKeyHash send_as, Overlay::BroadcastDataHash data_hash,
                              td::uint32 size, td::uint32 flags, td::BufferSlice part, td::uint32 seqno,
                              fec::FecType fec_type, td::uint32 date) {
  auto broadcast_hash = compute_broadcast_id(send_as, fec_type, data_hash, size, flags);
  auto part_data_hash = sha256_bits256(part.as_slice());
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, seqno);
  auto part_obj = std::make_unique<BroadcastFecPart>(
      broadcast_hash, part_hash, PublicKey{}, overlay->get_certificate(send_as), data_hash, size, flags, part_data_hash,
      std::move(part), seqno, std::move(fec_type), date, td::BufferSlice{}, false, adnl::AdnlNodeIdShort::zero());
  auto to_sign = part_obj->to_sign();
  auto P = td::PromiseCreator::lambda([overlay = actor_id(overlay), part = std::move(part_obj)](
                                          td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
    td::actor::send_closure(overlay, &OverlayImpl::broadcast_fec_signed, std::move(part), std::move(R));
  });
  td::actor::send_closure(overlay->keyring(), &keyring::Keyring::sign_add_get_public_key, send_as, std::move(to_sign),
                          std::move(P));
}

void BroadcastsFec::signed_(OverlayImpl *overlay, std::unique_ptr<BroadcastFecPart> &&part,
                            td::Result<std::pair<td::BufferSlice, PublicKey>> &&R) {
  if (R.is_error()) {
    td::Status reason = R.move_as_error();
    if (reason.code() == ErrorCode::notready) {
      LOG(DEBUG) << "failed to send fec broadcast: " << reason;
    } else {
      LOG(WARNING) << "failed to send fec broadcast: " << reason;
    }
    return;
  }
  auto V = R.move_as_ok();
  part->source_ = std::move(V.second);
  part->signature_ = std::move(V.first);
  part->cert_ = overlay->get_certificate(part->source_.compute_short_id());
  td::Status S = process(overlay, *part);
  if (S.is_error() && S.code() != ErrorCode::notready) {
    LOG(WARNING) << "failed to process fec broadcast: " << S;
  }
}

td::Status BroadcastsFec::process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                            tl_object_ptr<ton_api::overlay_broadcastFec> broadcast) {
  TRY_STATUS(overlay->check_date(broadcast->date_));
  PublicKey source(broadcast->src_);
  auto part_data_hash = sha256_bits256(broadcast->data_.as_slice());
  TRY_RESULT(fec_type, fec::FecType::create(std::move(broadcast->fec_)));
  auto broadcast_hash = compute_broadcast_id(source.compute_short_id(), fec_type, broadcast->data_hash_,
                                             broadcast->data_size_, broadcast->flags_);
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, broadcast->seqno_);
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));
  BroadcastFecPart part(broadcast_hash, part_hash, source, std::move(cert), broadcast->data_hash_,
                        static_cast<td::uint32>(broadcast->data_size_), static_cast<td::uint32>(broadcast->flags_),
                        part_data_hash, std::move(broadcast->data_), static_cast<td::uint32>(broadcast->seqno_),
                        std::move(fec_type), static_cast<td::uint32>(broadcast->date_),
                        std::move(broadcast->signature_), false, src_peer_id);
  TRY_STATUS(process(overlay, part));
  return td::Status::OK();
}

td::Status BroadcastsFec::process_broadcast(OverlayImpl *overlay, adnl::AdnlNodeIdShort src_peer_id,
                                            tl_object_ptr<ton_api::overlay_broadcastFecShort> broadcast) {
  auto it = broadcasts_.find(broadcast->broadcast_hash_);
  if (it == broadcasts_.end()) {
    return td::Status::Error(ErrorCode::notready, "short part of unknown broadcast");
  }
  auto &bcast = *it->second;
  if (!bcast.ready_) {
    return td::Status::Error(ErrorCode::protoviolation, "short part of not finished broadcast");
  }
  TRY_STATUS(overlay->check_date(bcast.date_));

  auto source = PublicKey{broadcast->src_};
  auto part_data_hash = broadcast->part_data_hash_;
  auto broadcast_hash = bcast.hash_;
  auto part_hash = compute_broadcast_part_id(broadcast_hash, part_data_hash, broadcast->seqno_);
  TRY_RESULT(cert, Certificate::create(std::move(broadcast->certificate_)));
  td::uint32 seqno = static_cast<td::uint32>(broadcast->seqno_);
  BroadcastFecPart part(broadcast_hash, part_hash, source, std::move(cert), bcast.data_hash_, bcast.fec_type_.size(),
                        bcast.flags_, part_data_hash, bcast.get_part(seqno), seqno, bcast.fec_type_, bcast.date_,
                        std::move(broadcast->signature_), true, src_peer_id);
  TRY_STATUS(part.run_checks(overlay, &bcast));
  TRY_STATUS(part.run(overlay, bcast));
  return td::Status::OK();
}

void BroadcastsFec::checked(OverlayImpl *overlay, Overlay::BroadcastHash &&hash, td::Result<td::Unit> &&R) {
  auto it = broadcasts_.find(hash);
  if (it != broadcasts_.end()) {
    it->second->broadcast_checked(overlay, std::move(R));
  }
}

void BroadcastsFec::gc(OverlayImpl *overlay) {
  while (!broadcasts_.empty()) {
    auto bcast = BroadcastFec::from_list_node(lru_.prev);
    CHECK(bcast);
    if (bcast->date_ > td::Clocks::system() - 60) {
      break;
    }
    auto hash = bcast->hash_;
    CHECK(broadcasts_.count(hash) == 1);
    broadcasts_.erase(hash);
    overlay->register_delivered_broadcast(hash);
  }
}

td::Status BroadcastsFec::process(OverlayImpl *overlay, BroadcastFecPart &part) {
  auto it = broadcasts_.find(part.broadcast_hash_);
  if (it == broadcasts_.end()) {
    if (overlay->is_delivered(part.broadcast_hash_)) {
      return td::Status::Error(ErrorCode::notready, "duplicate broadcast");
    }
    TRY_STATUS(part.run_checks(overlay, nullptr));
    auto bcast = std::make_unique<BroadcastFec>(part.broadcast_hash_, part.broadcast_data_hash_, part.flags_,
                                                part.date_, part.source_, part.fec_type_);
    TRY_STATUS(bcast->run_checks());
    TRY_STATUS(bcast->init_fec_type());
    lru_.put(bcast.get());
    it = broadcasts_.emplace(part.broadcast_hash_, std::move(bcast)).first;
  } else {
    TRY_STATUS(part.run_checks(overlay, it->second.get()));
  }
  TRY_STATUS(part.run(overlay, *it->second));
  return td::Status::OK();
}

}  // namespace overlay

}  // namespace ton
