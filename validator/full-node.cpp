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
#include "full-node.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-io.hpp"
#include "td/actor/MultiPromise.h"
#include "full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

void FullNodeImpl::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }

  local_keys_.insert(key);
  create_private_block_overlay(key);
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (!sign_cert_by_.is_zero()) {
    promise.set_value(td::Unit());
    return;
  }

  for (auto &x : all_validators_) {
    if (x == key) {
      sign_cert_by_ = key;
    }
  }

  for (auto &shard : shards_) {
    td::actor::send_closure(shard.second, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (!local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }
  local_keys_.erase(key);
  private_block_overlays_.erase(key);
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (sign_cert_by_ != key) {
    promise.set_value(td::Unit());
    return;
  }
  sign_cert_by_ = PublicKeyHash::zero();

  for (auto &x : all_validators_) {
    if (local_keys_.count(x)) {
      sign_cert_by_ = x;
    }
  }

  for (auto &shard : shards_) {
    td::actor::send_closure(shard.second, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                  td::uint32 expiry_at, td::uint32 max_size,
                                                  td::Promise<td::BufferSlice> promise) {
    auto it = shards_.find(shard_id);
    if(it == shards_.end()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
      return;
    }
    td::actor::send_closure(it->second, &FullNodeShard::sign_overlay_certificate, signed_key, expiry_at, max_size, std::move(promise));
}

void FullNodeImpl::import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                    std::shared_ptr<ton::overlay::Certificate> cert,
                                                    td::Promise<td::Unit> promise) {
    auto it = shards_.find(shard_id);
    if(it == shards_.end()) {
      promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    }
    td::actor::send_closure(it->second, &FullNodeShard::import_overlay_certificate, signed_key, cert, std::move(promise));
}

void FullNodeImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  adnl_id_ = adnl_id;

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &s : shards_) {
    td::actor::send_closure(s.second, &FullNodeShard::update_adnl_id, adnl_id, ig.get_promise());
  }
  local_id_ = adnl_id_.pubkey_hash();

  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }
}

void FullNodeImpl::set_config(FullNodeConfig config) {
  config_ = config;
  for (auto& shard : shards_) {
    td::actor::send_closure(shard.second, &FullNodeShard::set_config, config);
  }
  for (auto& overlay : private_block_overlays_) {
    td::actor::send_closure(overlay.second, &FullNodePrivateBlockOverlay::set_config, config);
  }
  for (auto& overlay : custom_overlays_) {
    for (auto &actor : overlay.second.actors_) {
      td::actor::send_closure(actor.second, &FullNodeCustomOverlay::set_config, config);
    }
  }
}

void FullNodeImpl::add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) {
  if (params.nodes_.empty()) {
    promise.set_error(td::Status::Error("list of nodes is empty"));
    return;
  }
  std::string name = params.name_;
  if (custom_overlays_.count(name)) {
    promise.set_error(td::Status::Error(PSTRING() << "duplicate custom overlay name \"" << name << "\""));
    return;
  }
  VLOG(FULL_NODE_WARNING) << "Adding custom overlay \"" << name << "\", " << params.nodes_.size() << " nodes";
  auto &p = custom_overlays_[name];
  p.params_ = std::move(params);
  update_custom_overlay(p);
  promise.set_result(td::Unit());
}

void FullNodeImpl::del_custom_overlay(std::string name, td::Promise<td::Unit> promise) {
  auto it = custom_overlays_.find(name);
  if (it == custom_overlays_.end()) {
    promise.set_error(td::Status::Error(PSTRING() << "no such overlay \"" << name << "\""));
    return;
  }
  custom_overlays_.erase(it);
  promise.set_result(td::Unit());
}

void FullNodeImpl::initial_read_complete(BlockHandle top_handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &FullNodeImpl::sync_completed);
  });
  auto it = shards_.find(ShardIdFull{masterchainId});
  CHECK(it != shards_.end());
  td::actor::send_closure(it->second, &FullNodeShard::set_handle, top_handle, std::move(P));
}

void FullNodeImpl::add_shard(ShardIdFull shard) {
  while (true) {
    if (shards_.count(shard) == 0) {
      shards_.emplace(shard,
                      FullNodeShard::create(shard, local_id_, adnl_id_, zero_state_file_hash_, config_, keyring_, adnl_,
                                            rldp_, rldp2_, overlays_, validator_manager_, client_, actor_id(this)));
      if (all_validators_.size() > 0) {
        td::actor::send_closure(shards_[shard], &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
      }
    } else {
      break;
    }
    if (shard.shard == shardIdAll) {
      break;
    }
    shard = shard_parent(shard);
  }
}

void FullNodeImpl::del_shard(ShardIdFull shard) {
  LOG(FATAL) << "deleting shards not implemented: shard=" << shard;
  shards_.erase(shard);
}

void FullNodeImpl::sync_completed() {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::sync_complete, [](td::Unit) {});
}

void FullNodeImpl::send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  auto shard = get_shard(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT ihr message to unknown shard";
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::send_ihr_message, std::move(data));
}

void FullNodeImpl::send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  auto shard = get_shard(dst);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT ext message to unknown shard";
    return;
  }
  for (auto &private_overlay : custom_overlays_) {
    for (auto &actor : private_overlay.second.actors_) {
      auto local_id = actor.first;
      if (private_overlay.second.params_.msg_senders_.count(local_id)) {
        td::actor::send_closure(actor.second, &FullNodeCustomOverlay::send_external_message, data.clone());
      }
    }
  }
  td::actor::send_closure(shard, &FullNodeShard::send_external_message, std::move(data));
}

void FullNodeImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  auto shard = get_shard(ShardIdFull{masterchainId, shardIdAll});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  if (!private_block_overlays_.empty()) {
    td::actor::send_closure(private_block_overlays_.begin()->second,
                            &FullNodePrivateBlockOverlay::send_shard_block_info, block_id, cc_seqno, data.clone());
  }
  td::actor::send_closure(shard, &FullNodeShard::send_shard_block_info, block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                        td::BufferSlice data) {
  send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  auto shard = get_shard(ShardIdFull{masterchainId, shardIdAll});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  if (!private_block_overlays_.empty()) {
    td::actor::send_closure(private_block_overlays_.begin()->second, &FullNodePrivateBlockOverlay::send_block_candidate,
                            block_id, cc_seqno, validator_set_hash, data.clone());
  }
  if (broadcast_block_candidates_in_public_overlay_) {
    td::actor::send_closure(shard, &FullNodeShard::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                            std::move(data));
  }
}

void FullNodeImpl::send_broadcast(BlockBroadcast broadcast, bool custom_overlays_only) {
  send_block_broadcast_to_custom_overlays(broadcast);
  if (custom_overlays_only) {
    return;
  }
  auto shard = get_shard(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT broadcast to unknown shard";
    return;
  }
  if (broadcast.block_id.is_masterchain() && !private_block_overlays_.empty()) {
    td::actor::send_closure(private_block_overlays_.begin()->second, &FullNodePrivateBlockOverlay::send_broadcast,
                            broadcast.clone());
  }
  td::actor::send_closure(shard, &FullNodeShard::send_broadcast, std::move(broadcast));
}

void FullNodeImpl::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                  td::Promise<ReceivedBlock> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download block query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                       td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_zero_state, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                             td::Timestamp timeout, td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state diff query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_persistent_state, id, masterchain_block_id, priority, timeout,
                          std::move(promise));
}

void FullNodeImpl::download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                        td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof, block_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof_link, block_id, priority, timeout,
                          std::move(promise));
}

void FullNodeImpl::get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                       td::Promise<std::vector<BlockIdExt>> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::get_next_key_blocks, block_id, timeout, std::move(promise));
}

void FullNodeImpl::download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                                    td::Promise<std::string> promise) {
  auto shard = get_shard(ShardIdFull{masterchainId});
  CHECK(!shard.empty());
  td::actor::send_closure(shard, &FullNodeShard::download_archive, masterchain_seqno, std::move(tmp_dir), timeout,
                          std::move(promise));
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(ShardIdFull shard) {
  add_shard(ShardIdFull{shard.workchain, shardIdAll});
  while (shards_.count(shard) == 0) {
    if (shard.shard == shardIdAll) {
      return td::actor::ActorId<FullNodeShard>{};
    }
    shard = shard_parent(shard);
  }
  return shards_[shard].get();
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(AccountIdPrefixFull dst) {
  return get_shard(shard_prefix(dst, 60));
}

void FullNodeImpl::got_key_block_state(td::Ref<ShardState> state) {
  auto m = td::Ref<MasterchainState>{std::move(state)};

  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators;
  for (td::int32 i = -1; i <= 1; i++) {
    auto r = m->get_total_validator_set(i < 0 ? i : 1 - i);
    if (r.not_null()) {
      auto vec = r->export_vector();
      for (auto &el : vec) {
        auto key = ValidatorFullId{el.key}.compute_short_id();
        keys.push_back(key);
        if (local_keys_.count(key)) {
          l = key;
        }
        if (i == 1) {
          current_validators[key] = adnl::AdnlNodeIdShort{el.addr.is_zero() ? key.bits256_value() : el.addr};
        }
      }
    }
  }

  set_private_block_overlays_enable_compression(m->get_consensus_config().proto_version >= 3);

  if (current_validators != current_validators_) {
    current_validators_ = std::move(current_validators);
    update_private_overlays();
  }

  if (keys == all_validators_) {
    return;
  }

  all_validators_ = keys;
  sign_cert_by_ = l;
  CHECK(all_validators_.size() > 0);

  for (auto &shard : shards_) {
    td::actor::send_closure(shard.second, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
  }
}

void FullNodeImpl::new_key_block(BlockHandle handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      VLOG(FULL_NODE_WARNING) << "failed to get key block state: " << R.move_as_error();
    } else {
      td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_state, R.move_as_ok());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle,
                          std::move(P));
}

void FullNodeImpl::process_block_broadcast(BlockBroadcast broadcast) {
  send_block_broadcast_to_custom_overlays(broadcast);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::prevalidate_block, std::move(broadcast),
                          [](td::Result<td::Unit> R) {
                            if (R.is_error()) {
                              if (R.error().code() == ErrorCode::notready) {
                                LOG(DEBUG) << "dropped broadcast: " << R.move_as_error();
                              } else {
                                LOG(INFO) << "dropped broadcast: " << R.move_as_error();
                              }
                            }
                          });
}

void FullNodeImpl::process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                     td::uint32 validator_set_hash, td::BufferSlice data) {
  send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  // ignore cc_seqno and validator_hash for now
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_block_candidate, block_id,
                          std::move(data));
}

void FullNodeImpl::start_up() {
  add_shard(ShardIdFull{masterchainId});
  if (local_id_.is_zero()) {
    if(adnl_id_.is_zero()) {
      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      local_id_ = pk.compute_short_id();

      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Unit) {});
    } else {
      local_id_ = adnl_id_.pubkey_hash();
    }
  }
  class Callback : public ValidatorManagerInterface::Callback {
   public:
    void initial_read_complete(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::initial_read_complete, handle);
    }
    void add_shard(ShardIdFull shard) override {
      td::actor::send_closure(id_, &FullNodeImpl::add_shard, shard);
    }
    void del_shard(ShardIdFull shard) override {
      td::actor::send_closure(id_, &FullNodeImpl::del_shard, shard);
    }
    void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ihr_message, dst, std::move(data));
    }
    void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ext_message, dst, std::move(data));
    }
    void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_shard_block_info, block_id, cc_seqno, std::move(data));
    }
    void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                              td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                              std::move(data));
    }
    void send_broadcast(BlockBroadcast broadcast, bool custom_overlays_only) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_broadcast, std::move(broadcast), custom_overlays_only);
    }
    void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<ReceivedBlock> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block, id, priority, timeout, std::move(promise));
    }
    void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_zero_state, id, priority, timeout, std::move(promise));
    }
    void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                   td::Timestamp timeout, td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_persistent_state, id, masterchain_block_id, priority,
                              timeout, std::move(promise));
    }
    void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof, block_id, priority, timeout,
                              std::move(promise));
    }
    void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof_link, block_id, priority, timeout,
                              std::move(promise));
    }
    void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                             td::Promise<std::vector<BlockIdExt>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::get_next_key_blocks, block_id, timeout, std::move(promise));
    }
    void download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                          td::Promise<std::string> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_archive, masterchain_seqno, std::move(tmp_dir), timeout,
                              std::move(promise));
    }

    void new_key_block(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::new_key_block, std::move(handle));
    }

    Callback(td::actor::ActorId<FullNodeImpl> id) : id_(id) {
    }

   private:
    td::actor::ActorId<FullNodeImpl> id_;
  };

  auto P = td::PromiseCreator::lambda([](td::Unit R) {});
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                          std::make_unique<Callback>(actor_id(this)), std::move(P));
}

void FullNodeImpl::update_private_overlays() {
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  private_block_overlays_.clear();
  if (local_keys_.empty()) {
    return;
  }
  for (const auto &key : local_keys_) {
    create_private_block_overlay(key);
  }
}

void FullNodeImpl::set_private_block_overlays_enable_compression(bool value) {
  if (private_block_overlays_enable_compression_ == value) {
    return;
  }
  private_block_overlays_enable_compression_ = true;
  for (auto &p : private_block_overlays_) {
    td::actor::send_closure(p.second, &FullNodePrivateBlockOverlay::set_enable_compression, value);
  }
}

void FullNodeImpl::create_private_block_overlay(PublicKeyHash key) {
  CHECK(local_keys_.count(key));
  if (current_validators_.count(key)) {
    std::vector<adnl::AdnlNodeIdShort> nodes;
    for (const auto &p : current_validators_) {
      nodes.push_back(p.second);
    }
    private_block_overlays_[key] = td::actor::create_actor<FullNodePrivateBlockOverlay>(
        "BlocksPrivateOverlay", current_validators_[key], std::move(nodes), zero_state_file_hash_, config_,
        private_block_overlays_enable_compression_, keyring_, adnl_, rldp_, rldp2_, overlays_, validator_manager_,
        actor_id(this));
  }
}

void FullNodeImpl::update_custom_overlay(CustomOverlayInfo &overlay) {
  auto old_actors = std::move(overlay.actors_);
  overlay.actors_.clear();
  CustomOverlayParams &params = overlay.params_;
  auto try_local_id = [&](const adnl::AdnlNodeIdShort &local_id) {
    if (std::find(params.nodes_.begin(), params.nodes_.end(), local_id) != params.nodes_.end()) {
      auto it = old_actors.find(local_id);
      if (it != old_actors.end()) {
        overlay.actors_[local_id] = std::move(it->second);
        old_actors.erase(it);
      } else {
        overlay.actors_[local_id] = td::actor::create_actor<FullNodeCustomOverlay>(
            "CustomOverlay", local_id, params, zero_state_file_hash_, config_, keyring_, adnl_, rldp_, rldp2_,
            overlays_, validator_manager_, actor_id(this));
      }
    }
  };
  try_local_id(adnl_id_);
  for (const PublicKeyHash &local_key : local_keys_) {
    auto it = current_validators_.find(local_key);
    if (it != current_validators_.end()) {
      try_local_id(it->second);
    }
  }
}

void FullNodeImpl::send_block_broadcast_to_custom_overlays(const BlockBroadcast& broadcast) {
  if (!custom_overlays_sent_broadcasts_.insert(broadcast.block_id).second) {
    return;
  }
  custom_overlays_sent_broadcasts_lru_.push(broadcast.block_id);
  if (custom_overlays_sent_broadcasts_lru_.size() > 256) {
    custom_overlays_sent_broadcasts_.erase(custom_overlays_sent_broadcasts_lru_.front());
    custom_overlays_sent_broadcasts_lru_.pop();
  }
  for (auto &private_overlay : custom_overlays_) {
    for (auto &actor : private_overlay.second.actors_) {
      auto local_id = actor.first;
      if (private_overlay.second.params_.block_senders_.count(local_id)) {
        td::actor::send_closure(actor.second, &FullNodeCustomOverlay::send_broadcast, broadcast.clone());
      }
    }
  }
}

void FullNodeImpl::send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt &block_id, CatchainSeqno cc_seqno,
                                                                     td::uint32 validator_set_hash,
                                                                     const td::BufferSlice &data) {
  // Same cache of sent broadcasts as in send_block_broadcast_to_custom_overlays
  if (!custom_overlays_sent_broadcasts_.insert(block_id).second) {
    return;
  }
  custom_overlays_sent_broadcasts_lru_.push(block_id);
  if (custom_overlays_sent_broadcasts_lru_.size() > 256) {
    custom_overlays_sent_broadcasts_.erase(custom_overlays_sent_broadcasts_lru_.front());
    custom_overlays_sent_broadcasts_lru_.pop();
  }
  for (auto &private_overlay : custom_overlays_) {
    for (auto &actor : private_overlay.second.actors_) {
      auto local_id = actor.first;
      if (private_overlay.second.params_.block_senders_.count(local_id)) {
        td::actor::send_closure(actor.second, &FullNodeCustomOverlay::send_block_candidate, block_id, cc_seqno,
                                validator_set_hash, data.clone());
      }
    }
  }
}

FullNodeImpl::FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
                           FullNodeConfig config, td::actor::ActorId<keyring::Keyring> keyring,
                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                           td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<dht::Dht> dht,
                           td::actor::ActorId<overlay::Overlays> overlays,
                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                           td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root)
    : local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , rldp2_(rldp2)
    , dht_(dht)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client)
    , db_root_(db_root)
    , config_(config) {
}

td::actor::ActorOwn<FullNode> FullNode::create(ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                                               FileHash zero_state_file_hash, FullNodeConfig config,
                                               td::actor::ActorId<keyring::Keyring> keyring,
                                               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                                               td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<dht::Dht> dht,
                                               td::actor::ActorId<overlay::Overlays> overlays,
                                               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root) {
  return td::actor::create_actor<FullNodeImpl>("fullnode", local_id, adnl_id, zero_state_file_hash, config, keyring,
                                               adnl, rldp, rldp2, dht, overlays, validator_manager, client, db_root);
}

FullNodeConfig::FullNodeConfig(const tl_object_ptr<ton_api::engine_validator_fullNodeConfig> &obj)
    : ext_messages_broadcast_disabled_(obj->ext_messages_broadcast_disabled_) {
}

tl_object_ptr<ton_api::engine_validator_fullNodeConfig> FullNodeConfig::tl() const {
  return create_tl_object<ton_api::engine_validator_fullNodeConfig>(ext_messages_broadcast_disabled_);
}
bool FullNodeConfig::operator==(const FullNodeConfig &rhs) const {
  return ext_messages_broadcast_disabled_ == rhs.ext_messages_broadcast_disabled_;
}
bool FullNodeConfig::operator!=(const FullNodeConfig &rhs) const {
  return !(*this == rhs);
}

CustomOverlayParams CustomOverlayParams::fetch(const ton_api::engine_validator_customOverlay& f) {
  CustomOverlayParams c;
  c.name_ = f.name_;
  for (const auto &node : f.nodes_) {
    c.nodes_.emplace_back(node->adnl_id_);
    if (node->msg_sender_) {
      c.msg_senders_[ton::adnl::AdnlNodeIdShort{node->adnl_id_}] = node->msg_sender_priority_;
    }
    if (node->block_sender_) {
      c.block_senders_.emplace(node->adnl_id_);
    }
  }
  return c;
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
