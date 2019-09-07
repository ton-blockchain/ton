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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "shard.hpp"
#include "adnl/utils.hpp"
#include "validator-set.hpp"
#include "validator/interfaces/block.h"
#include "ton/ton-tl.hpp"

#include <map>

namespace ton {

namespace validator {

namespace dummy0 {

td::Ref<ton::validator::ValidatorSet> MasterchainStateImpl::calculate_validator_set(ShardIdFull shard, td::uint32 cnt,
                                                                                    UnixTime ts,
                                                                                    td::uint32 randseed) const {
  auto hash = ts ^ randseed;
  auto hash2 = 1000000007;

  auto idx = hash % validators_.size();

  std::vector<std::pair<ValidatorFullId, ValidatorWeight>> vec;
  std::map<NodeIdShort, size_t> m;
  while (cnt-- > 0) {
    auto &d = validators_[idx];
    auto id = d.short_id();
    auto it = m.find(id);
    if (it != m.end()) {
      vec[it->second].second++;
    } else {
      vec.emplace_back(d, 1);
      m[id] = vec.size() - 1;
    }
    idx = (idx + hash2) % validators_.size();
  }

  return td::Ref<ValidatorSetImpl>{true, ts, shard.shard, std::move(vec)};
}

td::Ref<ValidatorSet> MasterchainStateImpl::get_validator_set(ShardIdFull shard) const {
  //CHECK(shard.is_masterchain());
  return calculate_validator_set(shard, 200, cur_validator_ts_, cur_randseed_);
}

td::Ref<ValidatorSet> MasterchainStateImpl::get_next_validator_set(ShardIdFull shard) const {
  //CHECK(shard.is_masterchain());
  return calculate_validator_set(shard, 200, cur_validator_ts_ + 1, next_randseed_);
}

td::Ref<ValidatorSet> MasterchainStateImpl::get_validator_set(ShardIdFull shard, UnixTime ts) const {
  if (ts == cur_validator_ts_) {
    return get_validator_set(shard);
  } else if (ts == cur_validator_ts_ + 1) {
    return get_next_validator_set(shard);
  } else {
    return td::Ref<ValidatorSet>{};
  }
}

td::Status MasterchainStateImpl::apply_block(BlockIdExt id, td::Ref<BlockData> block) {
  TRY_STATUS(ShardStateImpl::apply_block(id, block));
  TRY_RESULT(B, fetch_tl_object<ton_api::test0_shardchain_block>(block->data(), true));

  if (B->extra_->get_id() != ton_api::test0_masterchainBlockExtra_extra::ID) {
    return td::Status::Error(ErrorCode::protoviolation, "bad block extra");
  }
  auto E = static_cast<const ton_api::test0_masterchainBlockExtra_extra *>(B->extra_.get());

  if (B->prev_.size() != 1) {
    return td::Status::Error(ErrorCode::protoviolation, "bad prev size");
  }
  auto prev = create_block_id(B->prev_[0]);
  CHECK(prev.id.seqno == prev_blocks_.size());
  prev_blocks_.push_back(prev);

  if (E->rotate_) {
    CHECK(static_cast<UnixTime>(B->ts_) >= next_validator_rotate_at_);
    next_validator_rotate_at_ = B->ts_ + 300;
    cur_validator_ts_++;
    cur_randseed_ = next_randseed_;
    next_randseed_ = E->randseed_;
  } else {
    CHECK(static_cast<UnixTime>(B->ts_) < next_validator_rotate_at_);
  }

  for (auto &shard : E->shards_) {
    ShardDescr S{shard};
    auto shard_B = S.top_block;
    if (shard_B.id.seqno == 0) {
      for (auto &X : shards_) {
        if (X.top_block.id.workchain == shard_B.id.workchain) {
          return td::Status::Error(ErrorCode::protoviolation, "bad new block: duplicate zero block");
        }
      }
      shards_.emplace(std::move(S));
    } else {
      if (S.after_split) {
        if (S.top_block.id.shard == shardIdAll) {
          return td::Status::Error(ErrorCode::protoviolation, "cannot merge fullshard");
        }
        auto L = S;
        L.top_block.id.shard = shard_parent(S.top_block.id.shard);
        if (shards_.count(L) != 1) {
          return td::Status::Error(ErrorCode::protoviolation, "unknown parent shard");
        }
        shards_.erase(L);
        shards_.emplace(std::move(S));
      } else if (S.after_merge) {
        auto L = S;
        L.top_block.id.shard = shard_child(S.top_block.id.shard, true);
        if (shards_.count(L) != 1) {
          return td::Status::Error(ErrorCode::protoviolation, "unknown child L shard");
        }
        auto R = S;
        R.top_block.id.shard = shard_child(S.top_block.id.shard, false);
        if (shards_.count(R) != 1) {
          return td::Status::Error(ErrorCode::protoviolation, "unknown child R shard");
        }
        shards_.erase(L);
        shards_.erase(R);
        shards_.emplace(std::move(S));
      } else {
        if (shards_.count(S) != 1) {
          return td::Status::Error(ErrorCode::protoviolation, "unknown shard");
        }
        shards_.erase(S);
        shards_.emplace(std::move(S));
      }
    }
  }

  return td::Status::OK();
}

td::Result<td::BufferSlice> MasterchainStateImpl::serialize() const {
  TRY_RESULT(B, ShardStateImpl::serialize());
  auto F = fetch_tl_object<ton_api::test0_shardchain_state>(std::move(B), true).move_as_ok();

  std::vector<tl_object_ptr<ton_api::PublicKey>> pool;
  for (auto &v : validators_) {
    pool.emplace_back(v.tl());
  }

  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev;
  for (auto &p : prev_blocks_) {
    prev.emplace_back(create_tl_block_id(p));
  }

  std::vector<tl_object_ptr<ton_api::test0_masterchain_shardInfo>> shards;
  for (auto &shard : shards_) {
    shards.emplace_back(shard.tl());
  }
  auto obj = create_tl_object<ton_api::test0_masterchainStateExtra_extra>(
      cur_validator_ts_, cur_randseed_, next_randseed_, next_validator_rotate_at_, std::move(prev), std::move(shards),
      std::move(pool));
  F->extra_ = std::move(obj);
  return serialize_tl_object(F, true);
}

td::Result<td::Ref<MasterchainState>> MasterchainStateImpl::fetch(BlockIdExt block_id, td::BufferSlice data) {
  TRY_RESULT(F, fetch_tl_object<ton_api::test0_shardchain_state>(std::move(data), true));

  return td::Ref<MasterchainStateImpl>{true, F, block_id};
}

td::Result<td::Ref<ShardState>> ShardStateImpl::fetch(BlockIdExt block_id, td::BufferSlice data) {
  TRY_RESULT(F, fetch_tl_object<ton_api::test0_shardchain_state>(std::move(data), true));

  if (block_id.id.workchain == masterchainId) {
    return td::Ref<MasterchainStateImpl>{true, F, block_id};
  } else {
    return td::Ref<ShardStateImpl>{true, F, block_id};
  }
}

std::vector<td::Ref<McShardHash>> MasterchainStateImpl::get_shards() const {
  std::vector<td::Ref<McShardHash>> shards;
  for (auto &shard : shards_) {
    shards.emplace_back(shard.mc_shard());
  }
  return shards;
}

MasterchainStateImpl::MasterchainStateImpl(const tl_object_ptr<ton_api::test0_shardchain_state> &state,
                                           BlockIdExt block_id)
    : ShardStateImpl{state, block_id} {
  CHECK(state->extra_->get_id() == ton_api::test0_masterchainStateExtra_extra::ID);
  auto E = static_cast<const ton_api::test0_masterchainStateExtra_extra *>(state->extra_.get());

  cur_validator_ts_ = E->validator_ts_;
  cur_randseed_ = E->validator_randseed_;
  next_randseed_ = E->next_randseed_;
  next_validator_rotate_at_ = E->next_rotate_at_;

  for (auto &v : E->pool_) {
    validators_.emplace_back(PublicKey{v});
  }

  for (auto &p : E->prev_blocks_) {
    prev_blocks_.push_back(create_block_id(p));
  }

  for (auto &shard : E->shards_) {
    shards_.emplace(shard);
  }
}

bool MasterchainStateImpl::ancestor_is_valid(BlockIdExt id) const {
  if (id.id.seqno > get_seqno()) {
    return false;
  }
  if (id.id.seqno == get_seqno()) {
    return get_block_id() == id;
  }
  return prev_blocks_[id.id.seqno] == id;
}

MasterchainStateImpl::ShardDescr::ShardDescr(const tl_object_ptr<ton_api::test0_masterchain_shardInfo> &from) {
  top_block = create_block_id(from->last_block_);
  before_merge = from->before_merge_;
  before_split = from->before_split_;
  after_merge = from->after_merge_;
  after_split = from->after_split_;
}
tl_object_ptr<ton_api::test0_masterchain_shardInfo> MasterchainStateImpl::ShardDescr::tl() const {
  return create_tl_object<ton_api::test0_masterchain_shardInfo>(create_tl_block_id(top_block), before_merge,
                                                                before_split, after_merge, after_split);
}
td::Ref<McShardHash> MasterchainStateImpl::ShardDescr::mc_shard() const {
  return td::Ref<McShardHashImpl>{true, top_block, before_split, before_merge};
}

RootHash ShardStateImpl::root_hash() const {
  auto h = sha256_uint256(serialize().move_as_ok());
  return UInt256_2_Bits256(h);
}

td::Result<td::BufferSlice> ShardStateImpl::serialize() const {
  auto obj =
      create_tl_object<ton_api::test0_shardchain_state>(shard_.workchain, shard_.shard, seqno_, ts_, before_split_,
                                                        create_tl_object<ton_api::test0_masterchainStateExtra_empty>());
  return serialize_tl_object(obj, true);
}

td::Status ShardStateImpl::apply_block(BlockIdExt id, td::Ref<BlockData> block) {
  TRY_RESULT(B, fetch_tl_object<ton_api::test0_shardchain_block>(block->data(), true));
  if (static_cast<BlockSeqno>(B->seqno_) != seqno_ + 1) {
    return td::Status::Error(ErrorCode::protoviolation, "bad seqno");
  }
  if (static_cast<UnixTime>(B->ts_) <= ts_) {
    return td::Status::Error(ErrorCode::protoviolation, "time goes back");
  }
  if (B->workchain_ != shard_.workchain) {
    return td::Status::Error(ErrorCode::protoviolation, "bad workchain");
  }
  if (static_cast<ShardId>(B->shard_) != shard_.shard) {
    return td::Status::Error(ErrorCode::protoviolation, "bad shard");
  }
  seqno_++;
  ts_ = B->ts_;
  before_split_ = B->split_;
  return td::Status::OK();
}

ShardStateImpl::ShardStateImpl(const tl_object_ptr<ton_api::test0_shardchain_state> &state, BlockIdExt block_id) {
  blocks_id_ = {block_id};
  shard_ = ShardIdFull{state->workchain_, static_cast<ShardId>(state->shard_)};
  seqno_ = state->seqno_;
  ts_ = state->ts_;

  before_split_ = state->split_;
}

td::Result<td::Ref<ShardState>> ShardStateImpl::merge_with(const ShardState &with) const {
  auto &x = dynamic_cast<const ShardStateImpl &>(with);
  CHECK(blocks_id_.size() == 1);
  CHECK(x.blocks_id_.size() == 1);

  return td::Ref<ShardStateImpl>{true,
                                 shard_parent(shard_),
                                 std::max(seqno_, x.seqno_),
                                 std::max(ts_, x.ts_),
                                 false,
                                 std::vector<BlockIdExt>{blocks_id_[0], x.blocks_id_[0]}};
}

td::Result<std::pair<td::Ref<ShardState>, td::Ref<ShardState>>> ShardStateImpl::split() const {
  if (!before_split_) {
    return td::Status::Error(ErrorCode::protoviolation, "split flag not raised");
  }
  CHECK(blocks_id_.size() == 1);

  auto L = td::Ref<ShardStateImpl>{
      true, shard_child(shard_, true), seqno_, ts_, false, std::vector<BlockIdExt>{blocks_id_[0]}};
  auto R = td::Ref<ShardStateImpl>{
      true, shard_child(shard_, false), seqno_, ts_, false, std::vector<BlockIdExt>{blocks_id_[0]}};

  return std::pair<td::Ref<ShardState>, td::Ref<ShardState>>{L, R};
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
