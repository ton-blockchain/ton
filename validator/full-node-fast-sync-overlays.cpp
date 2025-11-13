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
#include "common/delay.h"
#include "td/utils/JsonBuilder.h"
#include "tl/tl_json.h"
#include "ton/ton-tl.hpp"

#include "checksum.h"
#include "full-node-fast-sync-overlays.hpp"
#include "full-node-serializer.hpp"

namespace ton::validator::fullnode {

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  process_block_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query) {
  process_block_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressedV2 &query) {
  process_block_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size());
  if (B.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in fast sync overlay from " << src << ": "
                        << B.ok().block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok());
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_outMsgQueueProofBroadcast &query) {
  if (src == local_id_.pubkey_hash()) {
    return;  // dropping broadcast from self
  }
  BlockIdExt block_id = create_block_id(query.block_);
  ShardIdFull shard_id = create_shard_id(query.dst_shard_);
  if (query.proof_->get_id() != ton_api::tonNode_outMsgQueueProof::ID) {
    LOG(ERROR) << "got tonNode.outMsgQueueProofBroadcast with proof not tonNode.outMsgQueueProof";
    return;
  }
  auto tl_proof = move_tl_object_as<ton_api::tonNode_outMsgQueueProof>(query.proof_);
  auto R = OutMsgQueueProof::fetch(shard_id, {block_id},
                                   block::ImportedMsgQueueLimits{.max_bytes = td::uint32(query.limits_->max_bytes_),
                                                                 .max_msgs = td::uint32(query.limits_->max_msgs_)},
                                   *tl_proof);
  if (R.is_error()) {
    LOG(ERROR) << "got tonNode.outMsgQueueProofBroadcast with invalid proof: " << R.error();
    return;
  }
  if (R.ok().size() != 1) {
    LOG(ERROR) << "got tonNode.outMsgQueueProofBroadcast with invalid proofs count=" << R.ok().size();
    return;
  }
  auto proof = std::move(R.move_as_ok()[0]);

  LOG(INFO) << "got tonNode.outMsgQueueProofBroadcast to " << shard_id.to_str() << " from " << block_id.to_str()
            << ", msgs=" << proof->msg_count_ << ", size=" << tl_proof->queue_proofs_.size();
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::add_out_msg_queue_proof, shard_id,
                          std::move(proof));
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(FULL_NODE_DEBUG) << "Received newShardBlockBroadcast in fast sync overlay from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block_description_broadcast,
                          block_id, query.block_->cc_seqno_, std::move(query.block_->data_));
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src,
                                                ton_api::tonNode_newBlockCandidateBroadcastCompressed &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_broadcast(PublicKeyHash src,
                                                ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeFastSyncOverlay::process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  BlockIdExt block_id;
  CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  td::BufferSlice data;
  auto S = deserialize_block_candidate_broadcast(query, block_id, cc_seqno, validator_set_hash, data,
                                                 overlay::Overlays::max_fec_broadcast_size());
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
  VLOG(FULL_NODE_DEBUG) << "Received newBlockCandidate in fast sync overlay from " << src << ": " << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data));
}

void FullNodeFastSyncOverlay::process_telemetry_broadcast(
    adnl::AdnlNodeIdShort src, const tl_object_ptr<ton_api::validator_telemetry> &telemetry) {
  if (telemetry->adnl_id_ != src.bits256_value()) {
    VLOG(FULL_NODE_WARNING) << "Invalid telemetry broadcast from " << src << ": adnl_id mismatch";
    return;
  }
  auto now = (td::int32)td::Clocks::system();
  if (telemetry->timestamp_ < now - 60) {
    VLOG(FULL_NODE_WARNING) << "Invalid telemetry broadcast from " << src << ": too old ("
                            << now - telemetry->timestamp_ << "s ago)";
    return;
  }
  if (telemetry->timestamp_ > now + 60) {
    VLOG(FULL_NODE_WARNING) << "Invalid telemetry broadcast from " << src << ": too new ("
                            << telemetry->timestamp_ - now << "s in the future)";
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Got telemetry broadcast from " << src;
  auto s = td::json_encode<std::string>(td::ToJson(*telemetry), false);
  std::erase_if(s, [](char c) { return c == '\n' || c == '\r'; });
  telemetry_file_ << s << "\n";
  telemetry_file_.flush();
  if (telemetry_file_.fail()) {
    VLOG(FULL_NODE_WARNING) << "Failed to write telemetry to file";
  }
}

void FullNodeFastSyncOverlay::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    if (collect_telemetry_ && src != local_id_.pubkey_hash()) {
      auto R = fetch_tl_prefix<ton_api::validator_telemetry>(broadcast, true);
      if (R.is_ok()) {
        process_telemetry_broadcast(adnl::AdnlNodeIdShort{src}, R.ok());
      }
    }
    return;
  }

  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeFastSyncOverlay::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newShardBlockBroadcast in fast sync overlay: " << block_id.to_str();
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

void FullNodeFastSyncOverlay::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending block broadcast in fast sync overlay (with compression): "
                        << broadcast.block_id.to_str();
  auto B = serialize_block_broadcast(broadcast, true);  // compression_enabled = true
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeFastSyncOverlay::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                   td::uint32 validator_set_hash, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B =
      serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true);  // compression enabled
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newBlockCandidate in fast sync overlay (with compression): " << block_id.to_str();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodeFastSyncOverlay::send_validator_telemetry(tl_object_ptr<ton_api::validator_telemetry> telemetry) {
  if (collect_telemetry_) {
    process_telemetry_broadcast(local_id_, telemetry);
  }
  auto data = serialize_tl_object(telemetry, true);
  if (data.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(data));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(data));
  }
}

void FullNodeFastSyncOverlay::collect_validator_telemetry(std::string filename) {
  if (collect_telemetry_) {
    telemetry_file_.close();
  }
  collect_telemetry_ = true;
  LOG(FULL_NODE_WARNING) << "Collecting validator telemetry to " << filename << " (local id: " << local_id_ << ")";
  telemetry_file_.open(filename, std::ios_base::app);
  if (!telemetry_file_.is_open()) {
    LOG(WARNING) << "Cannot open file " << filename << " for validator telemetry";
  }
}

void FullNodeFastSyncOverlay::send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) {
  if (!inited_) {
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_outMsgQueueProofBroadcast>(
      create_tl_shard_id(broadcast->dst_shard), create_tl_block_id(broadcast->block_id),
      create_tl_object<ton_api::tonNode_importedMsgQueueLimits>(broadcast->max_bytes, broadcast->max_msgs),
      create_tl_object<ton_api::tonNode_outMsgQueueProof>(broadcast->queue_proofs.clone(),
                                                          broadcast->block_state_proofs.clone(),
                                                          std::vector<td::int32>(1, broadcast->msg_count)));
  VLOG(FULL_NODE_DEBUG) << "Sending outMsgQueueProof in fast sync overlay to " << broadcast->dst_shard.to_str()
                        << " from " << broadcast->block_id.to_str() << ", msgs=" << broadcast->msg_count
                        << " bytes=" << broadcast->queue_proofs.size();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
}

void FullNodeFastSyncOverlay::start_up() {
  auto X = create_hash_tl_object<ton_api::tonNode_fastSyncOverlayId>(zero_state_file_hash_, create_tl_shard_id(shard_));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();

  try_init();
}

void FullNodeFastSyncOverlay::try_init() {
  // Sometimes adnl id is added to validator engine later (or not at all)
  td::actor::send_closure(
      adnl_, &adnl::Adnl::check_id_exists, local_id_, [SelfId = actor_id(this)](td::Result<bool> R) {
        if (R.is_ok() && R.ok()) {
          td::actor::send_closure(SelfId, &FullNodeFastSyncOverlay::init);
        } else {
          delay_action([SelfId]() { td::actor::send_closure(SelfId, &FullNodeFastSyncOverlay::try_init); },
                       td::Timestamp::in(30.0));
        }
      });
}

void FullNodeFastSyncOverlay::init() {
  LOG(INFO) << "Creating fast sync overlay for shard " << shard_.to_str() << ", adnl_id=" << local_id_;
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeFastSyncOverlay::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
    }
    void get_stats_extra(td::Promise<std::string> promise) override {
      td::actor::send_closure(node_, &FullNodeFastSyncOverlay::get_stats_extra, std::move(promise));
    }
    explicit Callback(td::actor::ActorId<FullNodeFastSyncOverlay> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodeFastSyncOverlay> node_;
  };

  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(),
                                     overlay::CertificateFlags::AllowFec | overlay::CertificateFlags::Trusted,
                                     {}};
  std::string scope = PSTRING() << R"({ "type": "fast-sync", "shard_id": )" << shard_.shard
                                << ", \"workchain_id\": " << shard_.workchain << " }";
  overlay::OverlayOptions options;
  bool is_validator = std::find(current_validators_adnl_.begin(), current_validators_adnl_.end(), local_id_) !=
                      current_validators_adnl_.end();
  if (!shard_.is_masterchain()) {
    options.default_permanent_members_flags_ = overlay::OverlayMemberFlags::DoNotReceiveBroadcasts;
  }
  options.local_overlay_member_flags_ = receive_broadcasts_ ? 0 : overlay::OverlayMemberFlags::DoNotReceiveBroadcasts;
  options.max_slaves_in_semiprivate_overlay_ = FullNode::MAX_FAST_SYNC_OVERLAY_CLIENTS;
  td::actor::send_closure(overlays_, &overlay::Overlays::create_semiprivate_overlay, local_id_,
                          overlay_id_full_.clone(), current_validators_adnl_, root_public_keys_, member_certificate_,
                          std::make_unique<Callback>(actor_id(this)), rules, std::move(scope), options);

  inited_ = true;
  if (shard_.is_masterchain()) {
    class TelemetryCallback : public ValidatorTelemetry::Callback {
     public:
      explicit TelemetryCallback(td::actor::ActorId<FullNodeFastSyncOverlay> id) : id_(id) {
      }
      void send_telemetry(tl_object_ptr<ton_api::validator_telemetry> telemetry) override {
        td::actor::send_closure(id_, &FullNodeFastSyncOverlay::send_validator_telemetry, std::move(telemetry));
      }

     private:
      td::actor::ActorId<FullNodeFastSyncOverlay> id_;
    };
    telemetry_sender_ = td::actor::create_actor<ValidatorTelemetry>(
        "telemetry", local_id_, std::make_unique<TelemetryCallback>(actor_id(this)));
  }
}

void FullNodeFastSyncOverlay::tear_down() {
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
  }
}

void FullNodeFastSyncOverlay::set_validators(std::vector<PublicKeyHash> root_public_keys,
                                             std::vector<adnl::AdnlNodeIdShort> current_validators_adnl) {
  root_public_keys_ = std::move(root_public_keys);
  current_validators_adnl_ = std::move(current_validators_adnl);
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
    init();
  }
}

void FullNodeFastSyncOverlay::set_member_certificate(overlay::OverlayMemberCertificate member_certificate) {
  member_certificate_ = std::move(member_certificate);
  if (inited_) {
    td::actor::send_closure(overlays_, &overlay::Overlays::update_member_certificate, local_id_, overlay_id_,
                            member_certificate_);
  }
}

void FullNodeFastSyncOverlay::set_receive_broadcasts(bool value) {
  if (value == receive_broadcasts_) {
    return;
  }
  receive_broadcasts_ = value;
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
    init();
  }
}

void FullNodeFastSyncOverlay::get_stats_extra(td::Promise<std::string> promise) {
  auto res = create_tl_object<ton_api::engine_validator_fastSyncOverlayStats>();
  res->shard_ = shard_.to_str();
  for (const auto &x : current_validators_adnl_) {
    res->validators_adnl_.push_back(x.bits256_value());
  }
  for (const auto &x : root_public_keys_) {
    res->root_public_keys_.push_back(x.bits256_value());
  }
  if (!member_certificate_.empty()) {
    res->member_certificate_ = member_certificate_.tl();
  }
  res->receive_broadcasts_ = receive_broadcasts_;
  promise.set_result(td::json_encode<std::string>(td::ToJson(*res), true));
}

std::pair<td::actor::ActorId<FullNodeFastSyncOverlay>, adnl::AdnlNodeIdShort> FullNodeFastSyncOverlays::choose_overlay(
    ShardIdFull shard) {
  for (auto &p : id_to_overlays_) {
    auto &overlays = p.second.overlays_;
    ShardIdFull cur_shard = shard;
    while (true) {
      auto it = overlays.find(cur_shard);
      if (it != overlays.end()) {
        return {it->second.get(), p.first};
      }
      if (cur_shard.pfx_len() == 0) {
        break;
      }
      cur_shard = shard_parent(cur_shard);
    }
  }
  return {td::actor::ActorId<FullNodeFastSyncOverlay>{}, adnl::AdnlNodeIdShort::zero()};
}

td::actor::ActorId<FullNodeFastSyncOverlay> FullNodeFastSyncOverlays::get_masterchain_overlay_for(
    adnl::AdnlNodeIdShort adnl_id) {
  auto it = id_to_overlays_.find(adnl_id);
  if (it == id_to_overlays_.end()) {
    return {};
  }
  auto it2 = it->second.overlays_.find(ShardIdFull{masterchainId});
  if (it2 == it->second.overlays_.end()) {
    return {};
  }
  return it2->second.get();
}

void FullNodeFastSyncOverlays::update_overlays(td::Ref<MasterchainState> state,
                                               std::set<adnl::AdnlNodeIdShort> my_adnl_ids,
                                               std::set<ShardIdFull> monitoring_shards,
                                               const FileHash &zero_state_file_hash,
                                               const td::actor::ActorId<keyring::Keyring> &keyring,
                                               const td::actor::ActorId<adnl::Adnl> &adnl,
                                               const td::actor::ActorId<overlay::Overlays> &overlays,
                                               const td::actor::ActorId<ValidatorManagerInterface> &validator_manager,
                                               const td::actor::ActorId<FullNode> &full_node) {
  monitoring_shards.insert(ShardIdFull{masterchainId});
  std::set<ShardIdFull> all_shards;
  all_shards.insert(ShardIdFull{masterchainId});
  for (const auto &desc : state->get_shards()) {
    ShardIdFull shard = desc->shard();
    td::uint32 monitor_min_split = state->monitor_min_split_depth(shard.workchain);
    if (shard.pfx_len() > monitor_min_split) {
      shard = shard_prefix(shard, monitor_min_split);
    }
    all_shards.insert(shard);
  }

  // Remove overlays for removed adnl ids and shards
  for (auto it = id_to_overlays_.begin(); it != id_to_overlays_.end();) {
    if (my_adnl_ids.count(it->first)) {
      auto &overlays_info = it->second;
      for (auto it2 = overlays_info.overlays_.begin(); it2 != overlays_info.overlays_.end();) {
        if (all_shards.count(it2->first)) {
          ++it2;
        } else {
          it2 = overlays_info.overlays_.erase(it2);
        }
      }
      ++it;
    } else {
      it = id_to_overlays_.erase(it);
    }
  }

  // On new keyblock - update validator set
  bool updated_validators = false;
  if (!last_key_block_seqno_ || last_key_block_seqno_.value() != state->last_key_block_id().seqno()) {
    updated_validators = true;
    last_key_block_seqno_ = state->last_key_block_id().seqno();
    root_public_keys_.clear();
    current_validators_adnl_.clear();
    // Previous, current and next validator sets
    for (int i = -1; i <= 1; ++i) {
      auto val_set = state->get_total_validator_set(i);
      if (val_set.is_null()) {
        continue;
      }
      for (const ValidatorDescr &val : val_set->export_vector()) {
        PublicKeyHash public_key_hash = ValidatorFullId{val.key}.compute_short_id();
        root_public_keys_.push_back(public_key_hash);
        current_validators_adnl_.emplace_back(val.addr.is_zero() ? public_key_hash.bits256_value() : val.addr);
      }
    }
    std::sort(root_public_keys_.begin(), root_public_keys_.end());
    root_public_keys_.erase(std::unique(root_public_keys_.begin(), root_public_keys_.end()), root_public_keys_.end());
    std::sort(current_validators_adnl_.begin(), current_validators_adnl_.end());
    current_validators_adnl_.erase(std::unique(current_validators_adnl_.begin(), current_validators_adnl_.end()),
                                   current_validators_adnl_.end());

    for (auto &[local_id, overlays_info] : id_to_overlays_) {
      overlays_info.is_validator_ =
          std::binary_search(current_validators_adnl_.begin(), current_validators_adnl_.end(), local_id);
      for (auto &[shard, overlay] : overlays_info.overlays_) {
        td::actor::send_closure(overlay, &FullNodeFastSyncOverlay::set_validators, root_public_keys_,
                                current_validators_adnl_);
      }
    }
  }

  // Cleanup outdated certificates
  double now = td::Clocks::system();
  for (auto &[_, certificates] : member_certificates_) {
    certificates.erase(std::remove_if(certificates.begin(), certificates.end(),
                                      [&](const overlay::OverlayMemberCertificate &certificate) {
                                        return certificate.is_expired(now);
                                      }),
                       certificates.end());
  }

  for (adnl::AdnlNodeIdShort local_id : my_adnl_ids) {
    bool is_new = !id_to_overlays_.count(local_id);
    auto &overlays_info = id_to_overlays_[local_id];
    // Update is_validator and current_certificate
    if (is_new) {
      overlays_info.is_validator_ =
          std::binary_search(current_validators_adnl_.begin(), current_validators_adnl_.end(), local_id);
    }
    bool changed_certificate = false;
    // Check if certificate is outdated or no longer authorized by current root keys
    if (!overlays_info.current_certificate_.empty() && overlays_info.current_certificate_.is_expired(now)) {
      changed_certificate = true;
      overlays_info.current_certificate_ = {};
    }
    if (!overlays_info.current_certificate_.empty() && updated_validators &&
        !std::binary_search(root_public_keys_.begin(), root_public_keys_.end(),
                            overlays_info.current_certificate_.issued_by().compute_short_id())) {
      changed_certificate = true;
      overlays_info.current_certificate_ = {};
    }
    if (overlays_info.current_certificate_.empty()) {
      auto it = member_certificates_.find(local_id);
      if (it != member_certificates_.end()) {
        for (const overlay::OverlayMemberCertificate &certificate : it->second) {
          if (std::binary_search(root_public_keys_.begin(), root_public_keys_.end(),
                                 certificate.issued_by().compute_short_id())) {
            changed_certificate = true;
            overlays_info.current_certificate_ = it->second.front();
            break;
          }
        }
      }
    }

    // Remove if it is not authorized
    if (!overlays_info.is_validator_ && overlays_info.current_certificate_.empty()) {
      id_to_overlays_.erase(local_id);
      continue;
    }

    // Update shard overlays
    for (ShardIdFull shard : all_shards) {
      bool receive_broadcasts = monitoring_shards.contains(shard);
      auto &overlay = overlays_info.overlays_[shard];
      if (overlay.empty()) {
        overlay = td::actor::create_actor<FullNodeFastSyncOverlay>(
            PSTRING() << "FastSyncOv" << shard.to_str(), local_id, shard, zero_state_file_hash, root_public_keys_,
            current_validators_adnl_, overlays_info.current_certificate_, receive_broadcasts, keyring, adnl, overlays,
            validator_manager, full_node);
      } else {
        td::actor::send_closure(overlay, &FullNodeFastSyncOverlay::set_receive_broadcasts, receive_broadcasts);
        if (changed_certificate) {
          td::actor::send_closure(overlay, &FullNodeFastSyncOverlay::set_member_certificate,
                                  overlays_info.current_certificate_);
        }
      }
    }
  }
}

void FullNodeFastSyncOverlays::add_member_certificate(adnl::AdnlNodeIdShort local_id,
                                                      overlay::OverlayMemberCertificate member_certificate) {
  if (member_certificate.empty() || member_certificate.is_expired()) {
    return;
  }
  member_certificates_[local_id].push_back(std::move(member_certificate));
  // Overlays will be updated in the next update_overlays
}

}  // namespace ton::validator::fullnode
