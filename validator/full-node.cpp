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

namespace ton {

namespace validator {

namespace fullnode {

void FullNodeImpl::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }

  local_keys_.insert(key);

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
      shards_.emplace(shard, FullNodeShard::create(shard, local_id_, adnl_id_, zero_state_file_hash_, keyring_, adnl_,
                                                   rldp_, overlays_, validator_manager_, client_));
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
  td::actor::send_closure(shard, &FullNodeShard::send_external_message, std::move(data));
}

void FullNodeImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  auto shard = get_shard(ShardIdFull{masterchainId, shardIdAll});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::send_shard_block_info, block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::send_broadcast(BlockBroadcast broadcast) {
  auto shard = get_shard(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT broadcast to unknown shard";
    return;
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

void FullNodeImpl::got_key_block_proof(td::Ref<ProofLink> proof) {
  auto R = proof->get_key_block_config();
  R.ensure();
  auto config = R.move_as_ok();

  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
  for (td::int32 i = -1; i <= 1; i++) {
    auto r = config->get_total_validator_set(i < 0 ? i : 1 - i);
    if (r.not_null()) {
      auto vec = r->export_vector();
      for (auto &el : vec) {
        auto key = ValidatorFullId{el.key}.compute_short_id();
        keys.push_back(key);
        if (local_keys_.count(key)) {
          l = key;
        }
      }
    }
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

void FullNodeImpl::got_zero_block_state(td::Ref<ShardState> state) {
  auto m = td::Ref<MasterchainState>{std::move(state)};

  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
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
      }
    }
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
  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get zero state: " << R.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &FullNodeImpl::got_zero_block_state, R.move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle,
                            std::move(P));
  } else {
    CHECK(handle->is_key_block());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get key block proof: " << R.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_proof, R.move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_proof_link_from_db, handle,
                            std::move(P));
  }
}

void FullNodeImpl::start_up() {
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
    void send_broadcast(BlockBroadcast broadcast) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_broadcast, std::move(broadcast));
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

FullNodeImpl::FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
                           td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                           td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<dht::Dht> dht,
                           td::actor::ActorId<overlay::Overlays> overlays,
                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                           td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root)
    : local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , dht_(dht)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client)
    , db_root_(db_root) {
  add_shard(ShardIdFull{masterchainId});
}

td::actor::ActorOwn<FullNode> FullNode::create(ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                                               FileHash zero_state_file_hash,
                                               td::actor::ActorId<keyring::Keyring> keyring,
                                               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                                               td::actor::ActorId<dht::Dht> dht,
                                               td::actor::ActorId<overlay::Overlays> overlays,
                                               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root) {
  return td::actor::create_actor<FullNodeImpl>("fullnode", local_id, adnl_id, zero_state_file_hash, keyring, adnl, rldp,
                                               dht, overlays, validator_manager, client, db_root);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
