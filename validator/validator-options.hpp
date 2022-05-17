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

#include "validator/validator.h"

namespace ton {

namespace validator {

struct ValidatorManagerOptionsImpl : public ValidatorManagerOptions {
 public:
  BlockIdExt zero_block_id() const override {
    return zero_block_id_;
  }
  BlockIdExt init_block_id() const override {
    return init_block_id_;
  }
  bool need_monitor(ShardIdFull shard) const override {
    return check_shard_(shard, 0, ShardCheckMode::m_monitor);
  }
  bool need_validate(ShardIdFull shard, CatchainSeqno cc_seqno) const override {
    return check_shard_(shard, cc_seqno, ShardCheckMode::m_validate);
  }
  bool allow_blockchain_init() const override {
    return allow_blockchain_init_;
  }
  double sync_blocks_before() const override {
    return sync_blocks_before_;
  }
  double block_ttl() const override {
    return block_ttl_;
  }
  double state_ttl() const override {
    return state_ttl_;
  }
  double max_mempool_num() const override {
    return max_mempool_num_;
  }
  double archive_ttl() const override {
    return archive_ttl_;
  }
  double key_proof_ttl() const override {
    return key_proof_ttl_;
  }
  bool initial_sync_disabled() const override {
    return initial_sync_disabled_;
  }
  bool is_hardfork(BlockIdExt block_id) const override {
    if (!block_id.is_valid()) {
      return false;
    }
    for (size_t i = 0; i < hardforks_.size(); i++) {
      if (block_id == hardforks_[i]) {
        return (i == hardforks_.size() - 1) || block_id.seqno() < hardforks_[i + 1].seqno();
      }
    }
    return false;
  }
  td::uint32 get_vertical_seqno(BlockSeqno seqno) const override {
    size_t best = 0;
    for (size_t i = 0; i < hardforks_.size(); i++) {
      if (seqno >= hardforks_[i].seqno()) {
        best = i + 1;
      }
    }
    return static_cast<td::uint32>(best);
  }
  td::uint32 get_maximal_vertical_seqno() const override {
    return td::narrow_cast<td::uint32>(hardforks_.size());
  }
  td::uint32 get_last_fork_masterchain_seqno() const override {
    return hardforks_.size() ? hardforks_.rbegin()->seqno() : 0;
  }
  std::vector<BlockIdExt> get_hardforks() const override {
    return hardforks_;
  }
  bool check_unsafe_resync_allowed(CatchainSeqno seqno) const override {
    return unsafe_catchains_.count(seqno) > 0;
  }
  td::uint32 check_unsafe_catchain_rotate(BlockSeqno seqno, CatchainSeqno cc_seqno) const override {
    auto it = unsafe_catchain_rotates_.find(cc_seqno);
    if (it == unsafe_catchain_rotates_.end()) {
      return 0;
    } else {
      return it->second.first <= seqno ? it->second.second : 0;
    }
  }
  bool need_db_truncate() const override {
    return truncate_ > 0;
  }
  BlockSeqno get_truncate_seqno() const override {
    return truncate_;
  }
  BlockSeqno sync_upto() const override {
    return sync_upto_;
  }

  void set_zero_block_id(BlockIdExt block_id) override {
    zero_block_id_ = block_id;
  }
  void set_init_block_id(BlockIdExt block_id) override {
    init_block_id_ = block_id;
  }
  void set_shard_check_function(std::function<bool(ShardIdFull, CatchainSeqno, ShardCheckMode)> check_shard) override {
    check_shard_ = std::move(check_shard);
  }
  void set_allow_blockchain_init(bool value) override {
    allow_blockchain_init_ = value;
  }
  void set_sync_blocks_before(double value) override {
    sync_blocks_before_ = value;
  }
  void set_block_ttl(double value) override {
    block_ttl_ = value;
  }
  void set_state_ttl(double value) override {
    state_ttl_ = value;
  }
  void set_max_mempool_num(double value) override {
    max_mempool_num_ = value;
  }
  void set_archive_ttl(double value) override {
    archive_ttl_ = value;
  }
  void set_key_proof_ttl(double value) override {
    key_proof_ttl_ = value;
  }
  void set_initial_sync_disabled(bool value) override {
    initial_sync_disabled_ = value;
  }
  void set_hardforks(std::vector<BlockIdExt> vec) override {
    hardforks_ = std::move(vec);
  }
  void add_unsafe_resync_catchain(CatchainSeqno seqno) override {
    unsafe_catchains_.insert(seqno);
  }
  void add_unsafe_catchain_rotate(BlockSeqno seqno, CatchainSeqno cc_seqno, td::uint32 value) override {
    VLOG(INFO) << "Add unsafe catchain rotation: Master block seqno " << seqno<<" Catchain seqno " << cc_seqno << " New value "<< value;
    unsafe_catchain_rotates_[cc_seqno] = std::make_pair(seqno, value);
  }
  void truncate_db(BlockSeqno seqno) override {
    truncate_ = seqno;
  }
  void set_sync_upto(BlockSeqno seqno) override {
    sync_upto_ = seqno;
  }

  ValidatorManagerOptionsImpl *make_copy() const override {
    return new ValidatorManagerOptionsImpl(*this);
  }

  ValidatorManagerOptionsImpl(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                              std::function<bool(ShardIdFull, CatchainSeqno, ShardCheckMode)> check_shard,
                              bool allow_blockchain_init, double sync_blocks_before,
                              double block_ttl, double state_ttl, double max_mempool_num,
                              double archive_ttl, double key_proof_ttl,
                              bool initial_sync_disabled)
      : zero_block_id_(zero_block_id)
      , init_block_id_(init_block_id)
      , check_shard_(std::move(check_shard))
      , allow_blockchain_init_(allow_blockchain_init)
      , sync_blocks_before_(sync_blocks_before)
      , block_ttl_(block_ttl)
      , state_ttl_(state_ttl)
      , max_mempool_num_(max_mempool_num)
      , archive_ttl_(archive_ttl)
      , key_proof_ttl_(key_proof_ttl)
      , initial_sync_disabled_(initial_sync_disabled) {
  }

 private:
  BlockIdExt zero_block_id_;
  BlockIdExt init_block_id_;
  std::function<bool(ShardIdFull, CatchainSeqno, ShardCheckMode)> check_shard_;
  bool allow_blockchain_init_;
  double sync_blocks_before_;
  double block_ttl_;
  double state_ttl_;
  double max_mempool_num_;
  double archive_ttl_;
  double key_proof_ttl_;
  bool initial_sync_disabled_;
  std::vector<BlockIdExt> hardforks_;
  std::set<CatchainSeqno> unsafe_catchains_;
  std::map<CatchainSeqno, std::pair<BlockSeqno, td::uint32>> unsafe_catchain_rotates_;
  BlockSeqno truncate_{0};
  BlockSeqno sync_upto_{0};
};

}  // namespace validator

}  // namespace ton
