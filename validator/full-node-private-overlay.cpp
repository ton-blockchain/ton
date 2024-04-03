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
#include "full-node-private-overlay.hpp"
#include "ton/ton-tl.hpp"
#include "common/delay.h"
#include "full-node-serializer.hpp"

namespace ton::validator::fullnode {

void FullNodePrivateBlockOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  process_block_broadcast(src, query);
}

void FullNodePrivateBlockOverlay::process_broadcast(PublicKeyHash src,
                                                    ton_api::tonNode_blockBroadcastCompressed &query) {
  process_block_broadcast(src, query);
}

void FullNodePrivateBlockOverlay::process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size());
  if (B.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in private overlay from " << src << ": "
                        << B.ok().block_id.to_str();
  auto P = td::PromiseCreator::lambda([](td::Result<td::Unit> R) {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::notready) {
        LOG(DEBUG) << "dropped broadcast: " << R.move_as_error();
      } else {
        LOG(INFO) << "dropped broadcast: " << R.move_as_error();
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::prevalidate_block, B.move_as_ok(),
                          std::move(P));
}

void FullNodePrivateBlockOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(FULL_NODE_DEBUG) << "Received newShardBlockBroadcast in private overlay from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block, block_id,
                          query.block_->cc_seqno_, std::move(query.block_->data_));
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
  VLOG(FULL_NODE_DEBUG) << "Sending newShardBlockBroadcast in private overlay: " << block_id.to_str();
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
  VLOG(FULL_NODE_DEBUG) << "Sending block broadcast in private overlay"
                        << (enable_compression_ ? " (with compression)" : "") << ": " << broadcast.block_id.to_str();
  auto B = serialize_block_broadcast(broadcast, enable_compression_);
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
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
  LOG(FULL_NODE_WARNING) << "Creating private block overlay for adnl id " << local_id_ << " : " << nodes_.size()
                         << " nodes, overlay_id=" << overlay_id_;
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
                          nodes_, std::make_unique<Callback>(actor_id(this)), rules, R"({ "type": "private-blocks" })");

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);
  inited_ = true;
}

void FullNodePrivateBlockOverlay::tear_down() {
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
  }
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  auto it = senders_.find(adnl::AdnlNodeIdShort{src});
  if (it == senders_.end()) {
    return;
  }
  LOG(FULL_NODE_DEBUG) << "Got external message in private overlay \"" << name_ << "\" from " << src
                       << " (priority=" << it->second << ")";
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_external_message,
                          std::move(query.message_->data_), it->second);
}

void FullNodeCustomOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }
  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeCustomOverlay::send_external_message(td::BufferSlice data) {
  if (!inited_ || config_.ext_messages_broadcast_disabled_) {
    return;
  }
  LOG(FULL_NODE_DEBUG) << "Sending external message to private overlay \"" << name_ << "\"";
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

void FullNodeCustomOverlay::start_up() {
  std::sort(nodes_.begin(), nodes_.end());
  nodes_.erase(std::unique(nodes_.begin(), nodes_.end()), nodes_.end());
  std::vector<td::Bits256> nodes;
  for (const adnl::AdnlNodeIdShort &id : nodes_) {
    nodes.push_back(id.bits256_value());
  }
  auto X = create_hash_tl_object<ton_api::tonNode_customOverlayId>(zero_state_file_hash_, name_, std::move(nodes));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();
  try_init();
}

void FullNodeCustomOverlay::try_init() {
  // Sometimes adnl id is added to validator engine later (or not at all)
  td::actor::send_closure(
      adnl_, &adnl::Adnl::check_id_exists, local_id_, [SelfId = actor_id(this)](td::Result<bool> R) {
        if (R.is_ok() && R.ok()) {
          td::actor::send_closure(SelfId, &FullNodeCustomOverlay::init);
        } else {
          delay_action([SelfId]() { td::actor::send_closure(SelfId, &FullNodeCustomOverlay::try_init); },
                       td::Timestamp::in(30.0));
        }
      });
}

void FullNodeCustomOverlay::init() {
  LOG(FULL_NODE_WARNING) << "Creating custom overlay \"" << name_ << "\" for adnl id " << local_id_ << " : "
                         << nodes_.size() << " nodes, overlay_id=" << overlay_id_;
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeCustomOverlay::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
    }
    Callback(td::actor::ActorId<FullNodeCustomOverlay> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodeCustomOverlay> node_;
  };

  std::map<PublicKeyHash, td::uint32> authorized_keys;
  for (const auto &sender : senders_) {
    authorized_keys[sender.first.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(), 0, std::move(authorized_keys)};
  td::actor::send_closure(
      overlays_, &overlay::Overlays::create_private_overlay, local_id_, overlay_id_full_.clone(), nodes_,
      std::make_unique<Callback>(actor_id(this)), rules,
      PSTRING() << R"({ "type": "custom-overlay", "name": ")" << td::format::Escaped{name_} << R"(" })");

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);
  inited_ = true;
}

void FullNodeCustomOverlay::tear_down() {
  LOG(FULL_NODE_WARNING) << "Destroying custom overlay \"" << name_ << "\" for adnl id " << local_id_;
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

}  // namespace ton::validator::fullnode
