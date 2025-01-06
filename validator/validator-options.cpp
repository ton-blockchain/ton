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
#include "validator-options.hpp"

#include "ton/ton-shard.h"

#include <ton/ton-tl.hpp>

namespace ton {

namespace validator {

td::Status CollatorsList::unpack(const ton_api::engine_validator_collatorsList& obj) {
  shards.clear();
  self_collate = false;
  for (const auto& shard_obj : obj.shards_) {
    ShardIdFull shard_id = create_shard_id(shard_obj->shard_id_);
    if (shard_id.is_masterchain()) {
      return td::Status::Error("masterchain shard in collators list");
    }
    if (!shard_id.is_valid_ext()) {
      return td::Status::Error(PSTRING() << "invalid shard " << shard_id.to_str());
    }
    shards.emplace_back();
    Shard& shard = shards.back();
    shard.shard_id = shard_id;
    shard.self_collate = shard_obj->self_collate_;
    if (shard.self_collate) {
      self_collate = true;
    }
    if (shard_obj->select_mode_.empty() || shard_obj->select_mode_ == "random") {
      shard.select_mode = mode_random;
    } else if (shard_obj->select_mode_ == "ordered") {
      shard.select_mode = mode_ordered;
    } else if (shard_obj->select_mode_ == "round_robin") {
      shard.select_mode = mode_round_robin;
    } else {
      return td::Status::Error(PSTRING() << "invalid select mode '" << shard_obj->select_mode_
                                         << "' (allowed: 'random', 'ordered', 'round_robin')");
    }
    for (const auto& collator : shard_obj->collators_) {
      shard.collators.push_back(adnl::AdnlNodeIdShort{collator->adnl_id_});
    }
  }
  return td::Status::OK();
}

CollatorsList CollatorsList::default_list() {
  CollatorsList list;
  list.shards.push_back(
      {.shard_id = ShardIdFull{basechainId, shardIdAll}, .select_mode = mode_random, .self_collate = true});
  list.self_collate = true;
  return list;
}

td::Ref<ValidatorManagerOptions> ValidatorManagerOptions::create(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                                                                 std::function<bool(ShardIdFull)> check_shard,
                                                                 bool allow_blockchain_init, double sync_blocks_before,
                                                                 double block_ttl, double state_ttl,
                                                                 double max_mempool_num, double archive_ttl,
                                                                 double key_proof_ttl, bool initial_sync_disabled) {
  return td::make_ref<ValidatorManagerOptionsImpl>(zero_block_id, init_block_id, std::move(check_shard),
                                                   allow_blockchain_init, sync_blocks_before, block_ttl, state_ttl,
                                                   max_mempool_num, archive_ttl, key_proof_ttl, initial_sync_disabled);
}

}  // namespace validator

}  // namespace ton
