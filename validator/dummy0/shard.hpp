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
#pragma once

#include "interfaces/validator-manager.h"

#include <set>

namespace ton {

namespace validator {

namespace dummy0 {

class McShardHashImpl : public McShardHash {
 public:
  BlockIdExt top_block_id() const override {
    return id_;
  }
  LogicalTime start_lt() const override {
    return 0;
  }
  LogicalTime end_lt() const override {
    return 0;
  }
  UnixTime fsm_utime() const override {
    return 0;
  }
  FsmState fsm_state() const override {
    return split_ ? FsmState::fsm_split : merge_ ? FsmState::fsm_merge : FsmState::fsm_none;
  }
  bool before_split() const override {
    return split_;
  }
  bool before_merge() const override {
    return merge_;
  }
  ShardIdFull shard() const override {
    return id_.shard_full();
  }

  McShardHashImpl(BlockIdExt id, bool split, bool merge) : id_{id}, split_(split), merge_(merge) {
  }

 private:
  BlockIdExt id_;
  bool split_;
  bool merge_;
};

class ShardStateImpl : virtual public ShardState {
 public:
  virtual ~ShardStateImpl() = default;
  static td::Result<td::Ref<ShardState>> fetch(BlockIdExt block_id, td::BufferSlice data);

  bool disable_boc() const override {
    return true;
  }
  UnixTime get_unix_time() const override {
    return ts_;
  }
  LogicalTime get_logical_time() const override {
    return lt_;
  }
  ShardIdFull get_shard() const override {
    return shard_;
  }
  BlockSeqno get_seqno() const override {
    return seqno_;
  }
  BlockIdExt get_block_id() const override {
    CHECK(blocks_id_.size() == 1);
    return blocks_id_[0];
  }
  bool before_split() const override {
    return before_split_;
  }
  RootHash root_hash() const override;
  td::Ref<vm::Cell> root_cell() const override {
    UNREACHABLE();
  }
  td::Result<td::Ref<MessageQueue>> message_queue() const override {
    UNREACHABLE();
  }
  td::Status apply_block(BlockIdExt id, td::Ref<BlockData> block) override;
  td::Result<td::Ref<ShardState>> merge_with(const ShardState &with) const override;
  td::Result<std::pair<td::Ref<ShardState>, td::Ref<ShardState>>> split() const override;
  td::Status validate_deep() const override {
    return td::Status::OK();
  }
  td::Result<td::BufferSlice> serialize() const override;
  ShardStateImpl *make_copy() const override {
    return new ShardStateImpl{shard_, seqno_, ts_, before_split_, blocks_id_};
  }

  ShardStateImpl(ShardIdFull shard, BlockSeqno seqno, UnixTime ts, bool split, std::vector<BlockIdExt> block_id)
      : shard_(shard), seqno_(seqno), ts_(ts), before_split_(split), blocks_id_(block_id) {
  }
  ShardStateImpl(const tl_object_ptr<ton_api::test0_shardchain_state> &state, BlockIdExt block_id);

 private:
  ShardIdFull shard_;
  BlockSeqno seqno_;
  UnixTime ts_;
  LogicalTime lt_ = 0;

  bool before_split_;

  std::vector<BlockIdExt> blocks_id_;
};

class MasterchainStateImpl : public MasterchainState, public ShardStateImpl {
 public:
  struct ShardDescr {
    BlockIdExt top_block;
    bool before_split;
    bool before_merge;
    bool after_split;
    bool after_merge;
    ShardDescr(const tl_object_ptr<ton_api::test0_masterchain_shardInfo> &from);
    tl_object_ptr<ton_api::test0_masterchain_shardInfo> tl() const;
    td::Ref<McShardHash> mc_shard() const;
    bool operator<(const ShardDescr &with) const {
      return top_block.shard_full() < with.top_block.shard_full();
    }
  };

  td::Ref<ValidatorSet> get_validator_set(ShardIdFull shard) const override;
  td::Ref<ValidatorSet> get_next_validator_set(ShardIdFull shard) const override;
  td::Ref<ValidatorSet> get_validator_set(ShardIdFull shard, UnixTime ts) const;
  bool rotated_all_shards() const override {
    return get_seqno() == 0;
  }
  UnixTime next_validator_rotate_at() const {
    return next_validator_rotate_at_;
  }
  std::vector<td::Ref<McShardHash>> get_shards() const override;
  bool ancestor_is_valid(BlockIdExt id) const override;

  td::Status apply_block(BlockIdExt id, td::Ref<BlockData> block) override;
  td::Result<td::Ref<ShardState>> merge_with(const ShardState &with) const override {
    UNREACHABLE();
  }
  td::Result<std::pair<td::Ref<ShardState>, td::Ref<ShardState>>> split() const override {
    UNREACHABLE();
  }
  td::Status validate_deep() const override {
    return td::Status::OK();
  }
  td::Ref<McShardHash> get_shard_from_config(ShardIdFull shard) const override {
    auto v = get_shards();
    for (auto &x : v) {
      if (x->shard() == shard) {
        return x;
      }
    }
    return td::Ref<McShardHash>{};
  }
  td::Result<td::BufferSlice> serialize() const override;
  MasterchainStateImpl *make_copy() const override {
    return new MasterchainStateImpl{get_shard(),
                                    get_seqno(),
                                    get_unix_time(),
                                    cur_validator_ts_,
                                    cur_randseed_,
                                    next_randseed_,
                                    next_validator_rotate_at_,
                                    validators_,
                                    prev_blocks_,
                                    shards_,
                                    get_block_id()};
  }
  static td::Result<td::Ref<MasterchainState>> fetch(BlockIdExt block_id, td::BufferSlice data);

  MasterchainStateImpl(ShardIdFull shard, BlockSeqno seqno, UnixTime ts, UnixTime cur_validator_ts,
                       td::uint32 cur_randseed, td::uint32 next_randseed, UnixTime next_validator_rotate_at,
                       std::vector<ValidatorFullId> validators, std::vector<BlockIdExt> prev_blocks,
                       std::set<ShardDescr> shards, BlockIdExt block_id)
      : ShardStateImpl{shard, seqno, ts, false, {block_id}}
      , cur_validator_ts_(cur_validator_ts)
      , cur_randseed_(cur_randseed)
      , next_randseed_(next_randseed)
      , next_validator_rotate_at_(next_validator_rotate_at)
      , validators_(std::move(validators))
      , prev_blocks_(std::move(prev_blocks))
      , shards_(std::move(shards)) {
  }
  MasterchainStateImpl(const tl_object_ptr<ton_api::test0_shardchain_state> &state, BlockIdExt block_id);

 private:
  td::Ref<ValidatorSet> calculate_validator_set(ShardIdFull shard, td::uint32 cnt, UnixTime ts,
                                                td::uint32 randseed) const;

  UnixTime cur_validator_ts_;
  td::uint32 cur_randseed_;
  td::uint32 next_randseed_;
  UnixTime next_validator_rotate_at_;

  std::vector<ValidatorFullId> validators_;
  std::vector<BlockIdExt> prev_blocks_;
  std::set<ShardDescr> shards_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
