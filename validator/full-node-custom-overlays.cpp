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
#include "ton/ton-io.hpp"
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
    VLOG(full_node, DEBUG) << "Dropping block broadcast in private overlay \"" << name_
                           << "\" from unauthorized sender " << src;
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
    VLOG(full_node, DEBUG) << "Dropping block broadcast in private overlay \"" << name_
                           << "\" from unauthorized sender " << src;
    return;
  }
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size(), k_called_from_custom);
  if (B.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << B.move_as_error();
    return;
  }
  VLOG(full_node, DEBUG) << "Received block broadcast " << (B.ok().sig_set->is_final() ? "" : "(approve signatures) ")
                         << "in custom overlay \"" << name_ << "\" from " << src << ": " << B.ok().block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok(), false,
                          BroadcastSource::custom_overlay);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockFinalityBroadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(full_node, DEBUG) << "Dropping block finality broadcast in private overlay \"" << name_
                           << "\" from unauthorized sender " << src;
    return;
  }
  auto block_id = create_block_id(query.id_);
  BlockFinalityBroadcast finality{block_id, block::BlockSignatureSet::fetch(query.signature_set_)};

  VLOG(full_node, DEBUG) << "Received blockFinalityBroadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                         << block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_finality_broadcast, std::move(finality),
                          BroadcastSource::custom_overlay);
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
  VLOG(full_node, DEBUG) << "Received block broadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                         << B.ok().block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok(), true,
                          BroadcastSource::custom_overlay);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  auto it = msg_senders_.find(adnl::AdnlNodeIdShort{src});
  if (it == msg_senders_.end()) {
    VLOG(full_node, DEBUG) << "Dropping external message broadcast in custom overlay \"" << name_
                           << "\" from unauthorized sender " << src;
    return;
  }
  VLOG(full_node, DEBUG) << "Got external message in custom overlay \"" << name_ << "\" from " << src
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
    VLOG(full_node, DEBUG) << "Dropping block candidate broadcast in private overlay \"" << name_
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
    VLOG(full_node, WARNING) << "received block candidate with too big size from " << src;
    return;
  }
  if (td::sha256_bits256(data.as_slice()) != block_id.file_hash) {
    VLOG(full_node, WARNING) << "received block candidate with incorrect file hash from " << src;
    return;
  }
  VLOG(full_node, DEBUG) << "Received newBlockCandidate in custom overlay \"" << name_ << "\" from " << src << ": "
                         << block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data), BroadcastSource::custom_overlay);
}

void FullNodeCustomOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  if (!block_senders_.count(adnl::AdnlNodeIdShort(src))) {
    VLOG(full_node, DEBUG) << "Dropping shard block description broadcast in private overlay \"" << name_
                           << "\" from unauthorized sender " << src;
    return;
  }
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(full_node, DEBUG) << "Received newShardBlockBroadcast in custom overlay \"" << name_ << "\" from " << src << ": "
                         << block_id;
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

void FullNodeCustomOverlay::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query,
                                          td::Promise<td::BufferSlice> promise) {
  if (!accept_queries_.contains(local_id_)) {
    promise.set_error(td::Status::Error("this node does not accept queries"));
    return;
  }
  td::actor::send_closure(full_node_, &FullNode::handle_query, std::move(query), src, QuerySource::custom_overlay,
                          std::move(promise));
}

void FullNodeCustomOverlay::send_external_message(td::BufferSlice data) {
  if (!inited_ || opts_.config_.ext_messages_broadcast_disabled_) {
    return;
  }
  VLOG(full_node, DEBUG) << "Sending external message to custom overlay \"" << name_ << "\"";
  auto B = create_serialize_tl_object<ton_api::tonNode_externalMessageBroadcast>(
      create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)));
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), 0, std::move(B));
}

void FullNodeCustomOverlay::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  VLOG(full_node, DEBUG) << "Sending block broadcast to custom overlay \"" << name_ << "\": " << broadcast.block_id;
  auto B = serialize_block_broadcast(broadcast, k_called_from_custom);
  if (B.is_error()) {
    VLOG(full_node, WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_block_finality_broadcast(BlockFinalityBroadcast finality) {
  if (!inited_) {
    return;
  }
  VLOG(full_node, DEBUG) << "Sending blockFinalityBroadcast to custom overlay \"" << name_
                         << "\": " << finality.block_id;
  auto B = create_serialize_tl_object<ton_api::tonNode_blockFinalityBroadcast>(create_tl_block_id(finality.block_id),
                                                                               finality.sig_set->tl());
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
}

void FullNodeCustomOverlay::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                 td::uint32 validator_set_hash, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B = serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true,
                                               k_called_from_custom);  // compression enabled
  if (B.is_error()) {
    VLOG(full_node, WARNING) << "failed to serialize block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(full_node, DEBUG) << "Sending newBlockCandidate in custom overlay \"" << name_ << "\": " << block_id;
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeCustomOverlay::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  VLOG(full_node, DEBUG) << "Sending newShardBlockBroadcast in custom overlay \"" << name_ << "\": " << block_id;
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

td::actor::Task<QuerySender> FullNodeCustomOverlay::get_query_sender() {
  class QuerySenderImpl : public QuerySenderInterface {
   public:
    QuerySenderImpl(adnl::AdnlNodeIdShort peer_id, adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                    td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<adnl::AdnlSenderInterface> adnl_sender,
                    td::actor::ActorId<FullNodeCustomOverlay> parent, std::pair<td::uint32, td::uint32> proto_version)
        : peer_id_(peer_id)
        , local_id_(local_id)
        , overlay_id_(overlay_id)
        , overlays_(std::move(overlays))
        , adnl_sender_(std::move(adnl_sender))
        , parent_(std::move(parent))
        , proto_version_(proto_version) {
    }

    void send_query(td::BufferSlice query, td::Timestamp timeout, td::uint64 max_answer_size,
                    td::Promise<td::BufferSlice> promise) const override {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, peer_id_, local_id_, overlay_id_, "q",
                              std::move(promise), timeout, std::move(query), max_answer_size, adnl_sender_);
    }

    void query_finished(bool success) const override {
      if (!success) {
        td::actor::ask(parent_, &FullNodeCustomOverlay::ping_peer, peer_id_).detach_silent();
      }
    }

    std::string to_str() const override {
      return PSTRING() << "peer " << peer_id_ << " in custom overlay";
    }

    std::pair<td::uint32, td::uint32> get_proto_version() const override {
      return proto_version_;
    }

   private:
    adnl::AdnlNodeIdShort peer_id_;
    adnl::AdnlNodeIdShort local_id_;
    overlay::OverlayIdShort overlay_id_;
    td::actor::ActorId<overlay::Overlays> overlays_;
    td::actor::ActorId<adnl::AdnlSenderInterface> adnl_sender_;
    td::actor::ActorId<FullNodeCustomOverlay> parent_;
    std::pair<td::uint32, td::uint32> proto_version_;
  };
  if (!inited_) {
    co_return td::Status::Error(ErrorCode::notready, "not inited");
  }
  if (!send_queries_) {
    co_return td::Status::Error(ErrorCode::notready, "queries not enabled");
  }
  auto peer = alive_peers_.get_random_key();
  if (peer == nullptr) {
    co_return td::Status::Error(ErrorCode::notready, "no nodes");
  }
  auto proto_version = peers_info_[*peer].proto_version;
  co_return std::make_shared<QuerySenderImpl>(*peer, local_id_, overlay_id_, overlays_, adnl_sender_, actor_id(this),
                                              proto_version);
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
  td::actor::send_closure(adnl_sender_, &adnl::AdnlSenderEx::add_id, local_id_);

  VLOG(full_node, WARNING) << "Creating custom overlay \"" << name_ << "\" for adnl id " << local_id_ << " : "
                           << nodes_.size() << " nodes, " << msg_senders_.size() << " msg senders, "
                           << block_senders_.size() << " block senders, overlay_id=" << overlay_id_;
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(node_, &FullNodeCustomOverlay::receive_query, src, std::move(data), std::move(promise));
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
  overlay_options.name_ = "custom." + name_;
  overlay_options.broadcast_speed_multiplier_ = opts_.private_broadcast_speed_multiplier_;
  overlay_options.send_twostep_broadcast_ = true;
  overlay_options.twostep_broadcast_sender_ = adnl_sender_;
  td::actor::send_closure(
      overlays_, &overlay::Overlays::create_private_overlay_ex, local_id_, overlay_id_full_.clone(), nodes_,
      std::make_unique<Callback>(actor_id(this)), rules,
      PSTRING() << R"({ "type": "custom-overlay", "name": ")" << td::format::Escaped{name_} << R"(" })",
      overlay_options);

  inited_ = true;
  if (send_queries_ && !accept_queries_.empty()) {
    alarm();
  }
}

void FullNodeCustomOverlay::tear_down() {
  VLOG(full_node, WARNING) << "Destroying custom overlay \"" << name_ << "\" for adnl id " << local_id_;
  td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

void FullNodeCustomOverlay::alarm() {
  CHECK(inited_);
  alarm_timestamp() = td::Timestamp::in(td::Random::fast(1.0, 2.0));
  size_t cnt = std::min<size_t>(accept_queries_.size(), 3);
  for (size_t iter = 0; iter < cnt; ++iter) {
    auto it = accept_queries_.upper_bound(last_pinged_peer_);
    if (it == accept_queries_.end()) {
      it = accept_queries_.begin();
    }
    adnl::AdnlNodeIdShort peer_id = last_pinged_peer_ = *it;
    if (peer_id == local_id_) {
      continue;
    }
    ping_peer(peer_id).start().detach_silent();
  }
}

td::actor::Task<> FullNodeCustomOverlay::ping_peer(adnl::AdnlNodeIdShort peer_id) {
  CHECK(inited_);
  td::BufferSlice query = create_serialize_tl_object<ton_api::tonNode_getCapabilities>();
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, peer_id, local_id_, overlay_id_, "q",
                          std::move(promise), td::Timestamp::in(1.0), std::move(query), 1024, adnl_sender_);
  auto r_response = co_await std::move(task).wrap();
  PeerInfo &peer_info = peers_info_[peer_id];
  if (r_response.is_error()) {
    VLOG(full_node, DEBUG) << "Failed to ping peer " << peer_id << ": " << r_response.move_as_error();
    if (peer_info.alive) {
      alive_peers_.remove(peer_id);
      peer_info.alive = false;
    }
    co_return {};
  }
  auto r_capabilities = fetch_tl_object<ton_api::tonNode_capabilities>(r_response.move_as_ok(), true);
  if (r_capabilities.is_error()) {
    VLOG(full_node, DEBUG) << "Failed to ping peer " << peer_id << ": " << r_capabilities.move_as_error();
    if (peer_info.alive) {
      alive_peers_.remove(peer_id);
      peer_info.alive = false;
    }
    co_return {};
  }
  auto capabilities = r_capabilities.move_as_ok();
  peer_info.proto_version =
      std::make_pair<td::uint32, td::uint32>(capabilities->version_major_, capabilities->version_minor_);
  if (!peer_info.alive) {
    alive_peers_.insert(peer_id, td::Unit{});
    peer_info.alive = true;
  }
  co_return {};
}

}  // namespace ton::validator::fullnode
