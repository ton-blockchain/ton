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

#include "auto/tl/ton_api.h"
#include "overlay/overlay.h"
#include "td/utils/List.h"
#include "fec/fec.h"
#include "common/checksum.h"

#include <set>

namespace ton {

namespace overlay {

class OverlayImpl;

class BroadcastFec : public td::ListNode {
 public:
  bool finalized() const {
    return ready_;
  }

  bool has_encoder() const {
    return encoder_ != nullptr;
  }

  auto get_hash() const {
    return hash_;
  }

  auto get_data_hash() const {
    return data_hash_;
  }

  td::uint32 get_date() const {
    return date_;
  }

  auto get_flags() const {
    return flags_;
  }

  const auto &get_fec_type() const {
    return fec_type_;
  }

  auto get_source() const {
    return src_;
  }

  auto get_size() const {
    return fec_type_.size();
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

  td::Status add_part(td::uint32 seqno, td::BufferSlice data) {
    CHECK(decoder_);
    td::fec::Symbol s;
    s.id = seqno;
    s.data = std::move(data);

    decoder_->add_symbol(std::move(s));

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

  BroadcastFec(Overlay::BroadcastHash hash, PublicKey src, Overlay::BroadcastDataHash data_hash, td::uint32 flags,
               td::uint32 date, fec::FecType fec_type)
      : hash_(hash)
      , data_hash_(data_hash)
      , flags_(flags)
      , date_(date)
      , src_(std::move(src))
      , fec_type_(std::move(fec_type)) {
  }

  static td::Result<std::unique_ptr<BroadcastFec>> create(Overlay::BroadcastHash hash, PublicKey src,
                                                          Overlay::BroadcastDataHash data_hash, td::uint32 flags,
                                                          td::uint32 date, fec::FecType fec_type);

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

  void broadcast_checked(td::Result<td::Unit> R) {
  }

 private:
  bool ready_ = false;

  Overlay::BroadcastHash hash_;
  Overlay::BroadcastDataHash data_hash_;

  td::uint32 flags_;
  td::uint32 date_;

  PublicKey src_;
  fec::FecType fec_type_;

  std::unique_ptr<td::fec::Decoder> decoder_;
  std::unique_ptr<td::fec::Encoder> encoder_;

  std::set<adnl::AdnlNodeIdShort> received_neighbours_;
  std::set<adnl::AdnlNodeIdShort> completed_neighbours_;

  td::uint32 next_seqno_ = 0;
  td::uint64 received_parts_ = 0;
};

class OverlayFecBroadcastPart : public td::ListNode {
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

  BroadcastFec *bcast_;
  OverlayImpl *overlay_;

  td::Status check_time();
  td::Status check_duplicate();
  td::Status check_source();
  td::Status check_signature();

  td::Status run_checks();
  td::Status apply();
  td::Status distribute();

 public:
  OverlayFecBroadcastPart(Overlay::BroadcastHash broadcast_hash, Overlay::BroadcastPartHash part_hash, PublicKey source,
                          std::shared_ptr<Certificate> cert, Overlay::BroadcastDataHash data_hash, td::uint32 data_size,
                          td::uint32 flags, Overlay::BroadcastDataHash part_data_hash, td::BufferSlice data,
                          td::uint32 seqno, fec::FecType fec_type, td::uint32 date, td::BufferSlice signature,
                          bool is_short, BroadcastFec *bcast, OverlayImpl *overlay)
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
      , bcast_(bcast)
      , overlay_(overlay) {
  }

  td::uint32 data_size() const {
    return static_cast<td::uint32>(data_.size());
  }

  Overlay::BroadcastPartHash get_hash() const {
    return part_hash_;
  }

  void update_source(PublicKey source) {
    source_ = source;
  }
  void update_signature(td::BufferSlice signature) {
    signature_ = std::move(signature);
  }
  void update_overlay(OverlayImpl *overlay);

  tl_object_ptr<ton_api::overlay_broadcastFec> export_tl();
  tl_object_ptr<ton_api::overlay_broadcastFecShort> export_tl_short();
  td::BufferSlice export_serialized();
  td::BufferSlice export_serialized_short();
  td::BufferSlice to_sign();

  td::Status run() {
    TRY_STATUS(run_checks());
    TRY_STATUS(apply());
    TRY_STATUS(distribute());
    return td::Status::OK();
  }

  static td::Status create(OverlayImpl *overlay, tl_object_ptr<ton_api::overlay_broadcastFec> broadcast);
  static td::Status create(OverlayImpl *overlay, tl_object_ptr<ton_api::overlay_broadcastFecShort> broadcast);
  static td::Status create_new(OverlayImpl *overlay, td::actor::ActorId<OverlayImpl> overlay_actor_id,
                               PublicKeyHash local_id, Overlay::BroadcastDataHash data_hash, td::uint32 size,
                               td::uint32 flags, td::BufferSlice part, td::uint32 seqno, fec::FecType fec_type,
                               td::uint32 date);

  static Overlay::BroadcastHash compute_broadcast_id(PublicKey source, const fec::FecType &fec_type,
                                                     Overlay::BroadcastDataHash data_hash, td::uint32 size,
                                                     td::uint32 flags);
  static Overlay::BroadcastHash compute_broadcast_id(PublicKeyHash source, const fec::FecType &fec_type,
                                                     Overlay::BroadcastDataHash data_hash, td::uint32 size,
                                                     td::uint32 flags);
  static Overlay::BroadcastPartHash compute_broadcast_part_id(Overlay::BroadcastHash broadcast_hash,
                                                              Overlay::BroadcastDataHash data_hash, td::uint32 seqno);
};

}  // namespace overlay

}  // namespace ton
