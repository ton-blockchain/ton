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
#include <ton/ton-tl.hpp>

#include "ton/ton-io.hpp"
#include "ton/ton-shard.h"

#include "validator-options.hpp"

namespace ton {

namespace validator {

td::Status CollatorsList::unpack(const ton_api::engine_validator_collatorsList& obj) {
  collators.clear();
  register_collators.clear();
  for (const auto& collator : obj.collators_) {
    collators.push_back(adnl::AdnlNodeIdShort{collator->adnl_id_});
  }
  for (const auto& collator : obj.register_collators_) {
    register_collators.push_back(adnl::AdnlNodeIdShort{collator->adnl_id_});
  }
  disable_self_collate = obj.disable_self_collate_;
  return td::Status::OK();
}

CollatorsList CollatorsList::default_list() {
  return CollatorsList{};
}

td::Status ShardBlockVerifierConfig::unpack(const ton_api::engine_validator_shardBlockVerifierConfig& obj) {
  shards.clear();
  for (const auto& shard_obj : obj.shards_) {
    Shard shard;
    shard.shard_id = create_shard_id(shard_obj->shard_id_);
    if (shard.shard_id.is_masterchain() || !shard.shard_id.is_valid_ext()) {
      return td::Status::Error(PSTRING() << "invalid shard " << shard.shard_id);
    }
    std::set<adnl::AdnlNodeIdShort> trusted_nodes;
    for (const td::Bits256& id : shard_obj->trusted_nodes_) {
      adnl::AdnlNodeIdShort node_id{id};
      if (!trusted_nodes.insert(node_id).second) {
        return td::Status::Error(PSTRING() << "duplicate node " << node_id);
      }
      shard.trusted_nodes.push_back(node_id);
    }
    if (shard_obj->required_confirms_ < 0 || shard_obj->required_confirms_ > (int)shard.trusted_nodes.size()) {
      return td::Status::Error(PSTRING() << "invalid required_confirms " << shard_obj->required_confirms_
                                         << " for shard " << shard.shard_id << " (nodes: " << shard.trusted_nodes.size()
                                         << ")");
    }
    shard.required_confirms = shard_obj->required_confirms_;
    shards.push_back(std::move(shard));
  }
  return td::Status::OK();
}

td::Result<td::Ref<ExtMessagePoolOptions>> ExtMessagePoolOptions::unpack(
    const ton_api::engine_validator_extMessagePoolConfig& f) {
  ExtMessagePoolOptions options;
  if (f.max_mempool_messages_ >= 0) {
    options.max_mempool_messages = f.max_mempool_messages_;
  }
  if (f.num_checkers_ >= 0) {
    if (f.num_checkers_ > 128) {
      return td::Status::Error("too many checkers (max 128)");
    }
    options.num_checkers = f.num_checkers_;
  }
  if (f.max_admission_waiters_ >= 0) {
    options.max_admission_waiters = f.max_admission_waiters_;
  }
  if (f.max_ext_msg_per_addr_ >= 0) {
    if (f.max_ext_msg_per_addr_ == 0) {
      return td::Status::Error("max_ext_msg_per_addr is zero");
    }
    options.max_ext_msg_per_addr = f.max_ext_msg_per_addr_;
  }
  if (f.max_ext_msg_per_addr_time_window_ >= 0.0) {
    if (f.max_ext_msg_per_addr_time_window_ == 0.0) {
      return td::Status::Error("max_ext_msg_per_addr_time_window is zero");
    }
    options.max_ext_msg_per_addr_time_window = f.max_ext_msg_per_addr_time_window_;
  }
  if (f.local_ls_message_priority_ >= 0) {
    options.local_ls_message_priority = f.local_ls_message_priority_;
  }
  return td::Ref<ExtMessagePoolOptions>{true, std::move(options)};
}

td::Ref<ValidatorManagerOptions> ValidatorManagerOptions::create(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                                                                 bool allow_blockchain_init, double sync_blocks_before,
                                                                 double block_ttl, double state_ttl, double archive_ttl,
                                                                 double key_proof_ttl, bool initial_sync_disabled) {
  return td::make_ref<ValidatorManagerOptionsImpl>(zero_block_id, init_block_id, allow_blockchain_init,
                                                   sync_blocks_before, block_ttl, state_ttl, archive_ttl, key_proof_ttl,
                                                   initial_sync_disabled);
}

}  // namespace validator

}  // namespace ton
