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
*/
#pragma once

#include "full-node-private-overlay.hpp"
#include "ton/ton-tl.hpp"
#include "common/delay.h"

namespace ton {

namespace validator {

namespace fullnode {

void FullNodePrivateBlockOverlay::process_broadcast(PublicKeyHash, ton_api::tonNode_blockBroadcast &query) {
  std::vector<BlockSignature> signatures;
  for (auto &sig : query.signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }

  BlockIdExt block_id = create_block_id(query.id_);
  BlockBroadcast B{block_id,
                   std::move(signatures),
                   static_cast<UnixTime>(query.catchain_seqno_),
                   static_cast<td::uint32>(query.validator_set_hash_),
                   std::move(query.data_),
                   std::move(query.proof_)};

  auto P = td::PromiseCreator::lambda([](td::Result<td::Unit> R) {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::notready) {
        LOG(DEBUG) << "dropped broadcast: " << R.move_as_error();
      } else {
        LOG(INFO) << "dropped broadcast: " << R.move_as_error();
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::prevalidate_block, std::move(B),
                          std::move(P));
}

void FullNodePrivateBlockOverlay::process_broadcast(PublicKeyHash, ton_api::tonNode_newShardBlockBroadcast &query) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block,
                          create_block_id(query.block_->block_), query.block_->cc_seqno_,
                          std::move(query.block_->data_));
}

void FullNodePrivateBlockOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }
  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodePrivateBlockOverlay::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                        td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_newShardBlockBroadcast>(
      create_tl_object<ton_api::tonNode_newShardBlock>(create_tl_block_id(block_id), cc_seqno, std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
  }
}

void FullNodePrivateBlockOverlay::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  std::vector<tl_object_ptr<ton_api::tonNode_blockSignature>> sigs;
  for (auto &sig : broadcast.signatures) {
    sigs.emplace_back(create_tl_object<ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_blockBroadcast>(
      create_tl_block_id(broadcast.block_id), broadcast.catchain_seqno, broadcast.validator_set_hash, std::move(sigs),
      broadcast.proof.clone(), broadcast.data.clone());
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
}

void FullNodePrivateBlockOverlay::start_up() {
  std::sort(nodes_.begin(), nodes_.end());
  nodes_.erase(std::unique(nodes_.begin(), nodes_.end()), nodes_.end());

  std::vector<td::Bits256> nodes;
  for (const adnl::AdnlNodeIdShort &id : nodes_) {
    nodes.push_back(id.bits256_value());
  }
  auto X = create_hash_tl_object<ton_api::tonNode_privateBlockOverlayId>(zero_state_file_hash_, std::move(nodes));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();

  try_init();
}

void FullNodePrivateBlockOverlay::try_init() {
  // Sometimes adnl id is added to validator engine later (or not at all)
  td::actor::send_closure(
      adnl_, &adnl::Adnl::check_id_exists, local_id_, [SelfId = actor_id(this)](td::Result<bool> R) {
        if (R.is_ok() && R.ok()) {
          td::actor::send_closure(SelfId, &FullNodePrivateBlockOverlay::init);
        } else {
          delay_action([SelfId]() { td::actor::send_closure(SelfId, &FullNodePrivateBlockOverlay::try_init); },
                       td::Timestamp::in(30.0));
        }
      });
}

void FullNodePrivateBlockOverlay::init() {
  LOG(FULL_NODE_INFO) << "Creating private block overlay for adnl id " << local_id_ << " : " << nodes_.size()
                      << " nodes";
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodePrivateBlockOverlay::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
    }
    Callback(td::actor::ActorId<FullNodePrivateBlockOverlay> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodePrivateBlockOverlay> node_;
  };

  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(),
                                     overlay::CertificateFlags::AllowFec | overlay::CertificateFlags::Trusted,
                                     {}};
  td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay, local_id_, overlay_id_full_.clone(),
                          nodes_, std::make_unique<Callback>(actor_id(this)), rules);

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);
  inited_ = true;
}

void FullNodePrivateBlockOverlay::tear_down() {
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
  }
}

void FullNodePrivateExtMsgOverlay::process_broadcast(PublicKeyHash, ton_api::tonNode_externalMessageBroadcast &query) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_external_message,
                          std::move(query.message_->data_), priority_);
}

void FullNodePrivateExtMsgOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }
  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodePrivateExtMsgOverlay::check_broadcast(PublicKeyHash, td::BufferSlice broadcast,
                                                   td::Promise<td::Unit> promise) {
  auto B = fetch_tl_object<ton_api::tonNode_externalMessageBroadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return promise.set_error(B.move_as_error_prefix("failed to parse external message broadcast: "));
  }

  auto q = B.move_as_ok();
  if (config_.ext_messages_broadcast_disabled_) {
    promise.set_error(td::Status::Error("rebroadcasting external messages is disabled"));
    promise = [manager = validator_manager_, message = q->message_->data_.clone(),
               priority = priority_](td::Result<td::Unit> R) mutable {
      if (R.is_ok()) {
        td::actor::send_closure(manager, &ValidatorManagerInterface::new_external_message, std::move(message),
                                priority);
      }
    };
  }
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::check_external_message,
                          std::move(q->message_->data_), promise.wrap([](td::Ref<ExtMessage>) { return td::Unit(); }));
}

void FullNodePrivateExtMsgOverlay::send_external_message(td::BufferSlice data) {
  if (config_.ext_messages_broadcast_disabled_) {
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_externalMessageBroadcast>(
      create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(B));
  }
}

void FullNodePrivateExtMsgOverlay::start_up() {
  std::vector<td::Bits256> nodes;
  for (const adnl::AdnlNodeIdShort &id : nodes_) {
    nodes.push_back(id.bits256_value());
  }
  auto X = create_hash_tl_object<ton_api::tonNode_privateExtMsgsOverlayId>(zero_state_file_hash_, std::move(nodes));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();

  LOG(FULL_NODE_INFO) << "Creating private ext msg overlay for adnl id " << local_id_ << " : " << nodes_.size()
                      << " nodes";
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodePrivateExtMsgOverlay::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
      td::actor::send_closure(node_, &FullNodePrivateExtMsgOverlay::check_broadcast, src, std::move(data),
                              std::move(promise));
    }
    Callback(td::actor::ActorId<FullNodePrivateExtMsgOverlay> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodePrivateExtMsgOverlay> node_;
  };

  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size()};
  td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay, local_id_, overlay_id_full_.clone(),
                          nodes_, std::make_unique<Callback>(actor_id(this)), rules);

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);
}

void FullNodePrivateExtMsgOverlay::tear_down() {
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
