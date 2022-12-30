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
#pragma once
#include "interfaces/shard.h"
#include "vm/db/StaticBagOfCellsDb.h"
#include "block/mc-config.h"
#include "config.hpp"

namespace ton {

namespace validator {
using td::Ref;

class ShardStateQ : virtual public ShardState {
 protected:
  BlockIdExt blkid;

 private:
  RootHash rhash;
  td::BufferSlice data;
  std::vector<std::shared_ptr<vm::StaticBagOfCellsDb>> bocs_;
  Ref<vm::Cell> root;
  LogicalTime lt{0};
  UnixTime utime{0};
  bool before_split_{false};
  bool fake_split_{false};
  bool fake_merge_{false};

 protected:
  friend class Ref<ShardStateQ>;
  ShardStateQ(const ShardStateQ& other);
  ShardStateQ(ShardStateQ&& other) = default;

 public:
  td::Status init();
  ShardStateQ(const BlockIdExt& _id, td::BufferSlice _data);
  ShardStateQ(const BlockIdExt& _id, Ref<vm::Cell> _root, td::BufferSlice _data = {});
  virtual ~ShardStateQ() = default;
  static td::Result<Ref<ShardStateQ>> fetch(const BlockIdExt& _id, td::BufferSlice _data, Ref<vm::Cell> _root = {});
  bool disable_boc() const override {
    return false;
  }
  ShardIdFull get_shard() const override {
    return ShardIdFull(blkid);
  }
  BlockSeqno get_seqno() const override {
    return blkid.id.seqno;
  }
  BlockIdExt get_block_id() const override {
    return blkid;
  }
  RootHash root_hash() const override {
    return rhash;
  }
  Ref<vm::Cell> root_cell() const override {
    return root;
  }
  bool before_split() const override {
    return before_split_;
  }
  UnixTime get_unix_time() const override {
    return utime;
  }
  LogicalTime get_logical_time() const override {
    return lt;
  }
  td::Status validate_deep() const override;
  ShardStateQ* make_copy() const override;
  td::Result<Ref<MessageQueue>> message_queue() const override;
  td::Status apply_block(BlockIdExt id, Ref<BlockData> block) override;
  td::Result<Ref<ShardState>> merge_with(const ShardState& with) const override;
  td::Result<std::pair<Ref<ShardState>, Ref<ShardState>>> split() const override;
  td::Result<td::BufferSlice> serialize() const override;
  td::Status serialize_to_file(td::FileFd& fd) const override;
};

#if TD_MSVC
#pragma warning(push)
#pragma warning(disable : 4250)  // MasterchainState is an interface, so there is no problem here
#endif
class MasterchainStateQ : public MasterchainState, public ShardStateQ {
 public:
  MasterchainStateQ(const BlockIdExt& _id, td::BufferSlice _data);
  MasterchainStateQ(const BlockIdExt& _id, Ref<vm::Cell> _root, td::BufferSlice _data = {});
  virtual ~MasterchainStateQ() = default;
  td::Status apply_block(BlockIdExt id, Ref<BlockData> block) override;
  Ref<ValidatorSet> get_validator_set(ShardIdFull shard) const override;
  Ref<ValidatorSet> get_next_validator_set(ShardIdFull shard) const override;
  Ref<ValidatorSet> get_total_validator_set(int next) const override;  // next = -1 -> prev, next = 0 -> cur
  Ref<ValidatorSet> get_validator_set(ShardIdFull shard, UnixTime ts, CatchainSeqno cc_seqno) const;
  bool rotated_all_shards() const override;
  std::vector<Ref<McShardHash>> get_shards() const override;
  td::Ref<McShardHash> get_shard_from_config(ShardIdFull shard) const override;
  bool ancestor_is_valid(BlockIdExt id) const override {
    return check_old_mc_block_id(id);
  }
  bool workchain_is_active(WorkchainId workchain_id) const override {
    return has_workchain(workchain_id);
  }
  bool has_workchain(WorkchainId workchain) const {
    return config_ && config_->has_workchain(workchain);
  }
  td::uint32 min_split_depth(WorkchainId workchain_id) const override;
  td::uint32 soft_min_split_depth(WorkchainId workchain_id) const override;
  BlockSeqno min_ref_masterchain_seqno() const override;
  td::Status prepare() override;
  ZeroStateIdExt get_zerostate_id() const {
    return zerostate_id_;
  }
  ValidatorSessionConfig get_consensus_config() const override {
    return config_->get_consensus_config();
  }
  block::SizeLimitsConfig::ExtMsgLimits get_ext_msg_limits() const override {
    auto R = config_->get_size_limits_config();
    return R.is_error() ? block::SizeLimitsConfig::ExtMsgLimits() : R.ok_ref().ext_msg_limits;
  }
  BlockIdExt last_key_block_id() const override;
  BlockIdExt next_key_block_id(BlockSeqno seqno) const override;
  BlockIdExt prev_key_block_id(BlockSeqno seqno) const override;
  MasterchainStateQ* make_copy() const override;

  static td::Result<Ref<MasterchainStateQ>> fetch(const BlockIdExt& _id, td::BufferSlice _data,
                                                  Ref<vm::Cell> _root = {});

  bool get_old_mc_block_id(ton::BlockSeqno seqno, ton::BlockIdExt& blkid,
                           ton::LogicalTime* end_lt = nullptr) const override;
  bool check_old_mc_block_id(const ton::BlockIdExt& blkid, bool strict = false) const override;
  std::shared_ptr<block::ConfigInfo> get_config() const {
    return config_;
  }
  td::Result<td::Ref<ConfigHolder>> get_config_holder() const override {
    if (!config_) {
      return td::Status::Error(ErrorCode::notready, "config not found");
    } else {
      return td::make_ref<ConfigHolderQ>(config_);
    }
  }

 private:
  ZeroStateIdExt zerostate_id_;
  std::shared_ptr<block::ConfigInfo> config_;
  std::shared_ptr<block::ValidatorSet> cur_validators_, next_validators_;
  MasterchainStateQ(const MasterchainStateQ& other) = default;
  td::Status mc_init();
  td::Status mc_reinit();
  Ref<ValidatorSet> compute_validator_set(ShardIdFull shard, const block::ValidatorSet& vset, UnixTime time,
                                          CatchainSeqno cc_seqno) const;
};
#if TD_MSVC
#pragma warning(pop)
#endif

}  // namespace validator
}  // namespace ton
