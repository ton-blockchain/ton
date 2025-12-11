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

#include "auto/tl/ton_api.h"
#include "td/utils/Variant.h"
#include "td/utils/overloaded.h"
#include "ton/ton-shard.h"
#include "ton/ton-types.h"

namespace ton {

namespace validator {

struct UnsplitStateType {};

struct SplitAccountStateType {
  ShardId effective_shard_id;
};

struct SplitPersistentStateType {};

using PersistentStateType = td::Variant<UnsplitStateType, SplitAccountStateType, SplitPersistentStateType>;

auto persistent_state_id_from_v1_query(auto const &query) {
  auto block = create_tl_block_id(create_block_id(query.block_));
  auto mc_block = create_tl_block_id(create_block_id(query.masterchain_block_));
  return create_tl_object<ton_api::tonNode_persistentStateIdV2>(std::move(block), std::move(mc_block), 0);
}

auto persistent_state_from_v2_query(auto const &query) {
  auto block = create_block_id(query.state_->block_);
  auto mc_block = create_block_id(query.state_->masterchain_block_);
  ShardId effective_shard = static_cast<ShardId>(query.state_->effective_shard_);

  if (effective_shard == 0 || !shard_is_ancestor(block.shard_full().shard, effective_shard)) {
    // The second condition is technically a "protocol" violation but since we don't really validate
    // stuff here regardless, let's just map it to an unsplit state.
    return std::tuple{block, mc_block, PersistentStateType{UnsplitStateType{}}};
  }

  if (effective_shard == block.shard_full().shard) {
    return std::tuple{block, mc_block, PersistentStateType{SplitPersistentStateType{}}};
  }

  CHECK(shard_is_proper_ancestor(block.shard_full().shard, effective_shard));
  return std::tuple{block, mc_block, PersistentStateType{SplitAccountStateType{effective_shard}}};
}

inline ShardId persistent_state_to_effective_shard(ShardIdFull const &shard, PersistentStateType const &type) {
  ShardId result = 0;
  type.visit(td::overloaded([](UnsplitStateType) {},
                            [&](SplitAccountStateType type) { result = type.effective_shard_id; },
                            [&](SplitPersistentStateType) { result = shard.shard; }));
  return result;
}

inline std::string persistent_state_type_to_string(ShardIdFull const &shard, PersistentStateType const &state) {
  std::string result;
  state.visit(td::overloaded([&](UnsplitStateType) { result = "unsplit"; },
                             [&](SplitAccountStateType type) {
                               int real_pfx_len = shard_prefix_length(shard.shard);
                               int effective_pfx_len = shard_prefix_length(type.effective_shard_id);
                               td::uint64 parts_count = 1 << (effective_pfx_len - real_pfx_len);
                               td::uint64 part_idx =
                                   type.effective_shard_id >> (64 - effective_pfx_len) & (parts_count - 1);
                               result =
                                   "part " + std::to_string(part_idx + 1) + " out of " + std::to_string(parts_count);
                             },
                             [&](SplitPersistentStateType) { result = "split header"; }));
  return result;
}

}  // namespace validator

}  // namespace ton
