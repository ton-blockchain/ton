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
#include "auto/tl/ton_api_json.h"
#include "common/checksum.h"
#include "common/delay.h"
#include "td/utils/JsonBuilder.h"
#include "tl/tl_json.h"
#include "ton/ton-tl.hpp"

#include "full-node-custom-overlays.hpp"
#include "full-node-serializer.hpp"

namespace ton::validator::fullnode {

namespace {

constexpr const char *k_called_from_custom = "custom";

}  // namespace

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  process_block_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query) {
  process_block_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block broadcast in private overlay \"" << name_ << "\" from unauthorized sender "
                          << src;
    return;
  }

  auto R_requires_state = need_state_for_decompression(query);
  if (R_requires_state.is_error()) {
    LOG(DEBUG) << "Failed to check if state is required for broadcast: " << R_requires_state.move_as_error();
    return;
  }

  if (R_requires_state.move_as_ok()) {
    auto block_wo_data = get_block_broadcast_without_data(query);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), src,
                                         query = std::move(query)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        LOG(DEBUG) << "Dropped V2 broadcast because of signatures validation error: " << R.move_as_error();
        return;
      }

      td::actor::send_closure(SelfId, &FullNodeCustomOverlay::obtain_state_for_decompression, src, std::move(query));
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_broadcast_signatures,
                            std::move(block_wo_data), std::move(P));
    return;
  }

  process_block_broadcast(src, query);
}

void FullNodeCustomOverlay::process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block broadcast in private overlay \"" << name_ << "\" from unauthorized sender "
                          << src;
    return;
  }
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom);
  if (B.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                        << B.ok().block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok(), false);
}

void FullNodeCustomOverlay::obtain_state_for_decompression(PublicKeyHash src,
                                                           ton_api::tonNode_blockBroadcastCompressedV2 query) {
  auto id = create_block_id(query.id_);
  auto R_prev = extract_prev_blocks_from_proof(query.proof_.as_slice(), id);
  if (R_prev.is_error()) {
    LOG(DEBUG) << "Failed to extract prev blocks for V2 broadcast: " << R_prev.move_as_error();
    return;
  }
  auto prev_blocks = R_prev.move_as_ok();
  auto P_state = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), src, query = std::move(query)](td::Result<td::Ref<ShardState>> R_state) mutable {
        if (R_state.is_error()) {
          LOG(DEBUG) << "Failed to get state for V2 broadcast: " << R_state.move_as_error();
          return;
        }
        td::actor::send_closure(SelfId, &FullNodeCustomOverlay::process_block_broadcast_with_state, src,
                                std::move(query), R_state.move_as_ok());
      });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::wait_state_by_prev_blocks, id,
                          std::move(prev_blocks), std::move(P_state));
}

void FullNodeCustomOverlay::process_block_broadcast_with_state(PublicKeyHash src,
                                                               ton_api::tonNode_blockBroadcastCompressedV2 query,
                                                               td::Ref<ShardState> state) {
  td::Ref<vm::Cell> state_root = state->root_cell();
  auto B =
      deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom, state_root);
  if (B.is_error()) {
    LOG(DEBUG) << "Failed to deserialize block broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                        << B.ok().block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok(), true);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  auto it = msg_senders_.find(adnl::AdnlNodeIdShort{src});
  if (it == msg_senders_.end()) {
    VLOG(FULL_NODE_DEBUG) << "Dropping external message broadcast in custom overlay \"" << name_
                          << "\" from unauthorized sender " << src;
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Got external message in custom overlay \"" << name_ << "\" from " << src
                        << " (priority=" << it->second << ")";
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_external_message_broadcast,
                 std::move(query.message_->data_), it->second)
      .detach();
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src,
                                              ton_api::tonNode_newBlockCandidateBroadcastCompressed &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src,
                                              ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeCustomOverlay::process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(FULL_NODE_DEBUG) << "Dropping block candidate broadcast in private overlay \"" << name_
                          << "\" from unauthorized sender " << src;
    return;
  }
  BlockIdExt block_id;
  CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  td::BufferSlice data;
  auto S = deserialize_block_candidate_broadcast(query, block_id, cc_seqno, validator_set_hash, data,
                                                 overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom);
  if (S.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << S;
    return;
  }
  if (data.size() > FullNode::max_block_size()) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with too big size from " << src;
    return;
  }
  if (td::sha256_bits256(data.as_slice()) != block_id.file_hash) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with incorrect file hash from " << src;
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received newBlockCandidate in custom overlay \"" << name_ << "\" from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data));
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(FULL_NODE_DEBUG) << "Received newShardBlockBroadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_shard_block_info_broadcast, block_id, query.block_->cc_seqno_,
                          std::move(query.block_->data_));
}

void FullNodeCustomOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  if (adnl::AdnlNodeIdShort{src} == local_id_) {
    return;
  }
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }
  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeCustomOverlay::send_external_message(td::BufferSlice data) {
  if (!inited_ || opts_.config_.ext_messages_broadcast_disabled_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending external message to custom overlay \"" << name_ << "\"";
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

void FullNodeCustomOverlay::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending block broadcast to custom overlay \"" << name_
                        << "\": " << broadcast.block_id.to_str();
  auto B = serialize_block_broadcast(broadcast, true, k_called_from_custom, StateUsage::DecompressOnly);
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                 td::uint32 validator_set_hash, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B =
      serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true, k_called_from_custom);
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newBlockCandidate in custom overlay \"" << name_ << "\": " << block_id.to_str();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  VLOG(FULL_NODE_DEBUG) << "Sending newShardBlockBroadcast in custom overlay \"" << name_
                        << "\": " << block_id.to_str();
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
  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);

  LOG(FULL_NODE_WARNING) << "Creating custom overlay \"" << name_ << "\" for adnl id " << local_id_ << " : "
                         << nodes_.size() << " nodes, " << msg_senders_.size() << " msg senders, "
                         << block_senders_.size() << " block senders, overlay_id=" << overlay_id_;
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
  for (const auto &sender : msg_senders_) {
    authorized_keys[sender.first.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  for (const auto &sender : block_senders_) {
    authorized_keys[sender.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(), 0, std::move(authorized_keys)};
  overlay::OverlayOptions overlay_options;
  overlay_options.broadcast_speed_multiplier_ = opts_.private_broadcast_speed_multiplier_;
  overlay_options.send_twostep_broadcast_ = true;
  overlay_options.twostep_broadcast_sender_ = rldp2_;
  td::actor::send_closure(
      overlays_, &overlay::Overlays::create_private_overlay_ex, local_id_, overlay_id_full_.clone(), nodes_,
      std::make_unique<Callback>(actor_id(this)), rules,
      PSTRING() << R"({ "type": "custom-overlay", "name": ")" << td::format::Escaped{name_} << R"(" })",
      overlay_options);
  rldp_limit_guard_ = rldp2::PeersMtuLimitGuard(rldp2_, local_id_, nodes_, FullNode::max_block_size() + 1024);

  inited_ = true;
}

void FullNodeCustomOverlay::tear_down() {
  LOG(FULL_NODE_WARNING) << "Destroying custom overlay \"" << name_ << "\" for adnl id " << local_id_;
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

}  // namespace ton::validator::fullnode
