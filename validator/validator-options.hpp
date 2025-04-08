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
  bool need_monitor(ShardIdFull shard, const td::Ref<MasterchainState>& state) const override {
    td::uint32 min_split = state->monitor_min_split_depth(shard.workchain);
    return check_shard_((td::uint32)shard.pfx_len() <= min_split ? shard : shard_prefix(shard, min_split));
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
  std::string get_session_logs_file() const override {
    return session_logs_file_;
  }
  td::uint32 get_celldb_compress_depth() const override {
    return celldb_compress_depth_;
  }
  size_t get_max_open_archive_files() const override {
    return max_open_archive_files_;
  }
  double get_archive_preload_period() const override {
    return archive_preload_period_;
  }
  bool get_disable_rocksdb_stats() const override {
    return disable_rocksdb_stats_;
  }
  bool nonfinal_ls_queries_enabled() const override {
    return nonfinal_ls_queries_enabled_;
  }
  td::optional<td::uint64> get_celldb_cache_size() const override {
    return celldb_cache_size_;
  }
  bool get_celldb_direct_io() const override {
    return celldb_direct_io_;
  }
  bool get_celldb_preload_all() const override {
    return celldb_preload_all_;
  }
  bool get_celldb_in_memory() const override {
    return celldb_in_memory_;
  }
  bool get_celldb_v2() const override {
    return celldb_v2_;
  }
  bool get_celldb_disable_bloom_filter() const override {
    return celldb_disable_bloom_filter_;
  }
  td::optional<double> get_catchain_max_block_delay() const override {
    return catchain_max_block_delay_;
  }
  td::optional<double> get_catchain_max_block_delay_slow() const override {
    return catchain_max_block_delay_slow_;
  }
  bool get_state_serializer_enabled() const override {
    return state_serializer_enabled_;
  }
  td::Ref<CollatorOptions> get_collator_options() const override {
    return collator_options_;
  }
  bool get_fast_state_serializer_enabled() const override {
    return fast_state_serializer_enabled_;
  }
  double get_catchain_broadcast_speed_multiplier() const override {
    return catchain_broadcast_speed_multipliers_;
  }

  void set_zero_block_id(BlockIdExt block_id) override {
    zero_block_id_ = block_id;
  }
  void set_init_block_id(BlockIdExt block_id) override {
    init_block_id_ = block_id;
  }
  void set_shard_check_function(std::function<bool(ShardIdFull)> check_shard) override {
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
  void set_session_logs_file(std::string f) override {
    session_logs_file_ = std::move(f);
  }
  void set_celldb_compress_depth(td::uint32 value) override {
    celldb_compress_depth_ = value;
  }
  void set_max_open_archive_files(size_t value) override {
    max_open_archive_files_ = value;
  }
  void set_archive_preload_period(double value) override {
    archive_preload_period_ = value;
  }
  void set_disable_rocksdb_stats(bool value) override {
    disable_rocksdb_stats_ = value;
  }
  void set_nonfinal_ls_queries_enabled(bool value) override {
    nonfinal_ls_queries_enabled_ = value;
  }
  void set_celldb_cache_size(td::uint64 value) override {
    celldb_cache_size_ = value;
  }
  void set_celldb_direct_io(bool value) override {
    celldb_direct_io_ = value;
  }
  void set_celldb_preload_all(bool value) override {
    celldb_preload_all_ = value;
  }
  void set_celldb_in_memory(bool value) override {
    celldb_in_memory_ = value;
  }
  void set_celldb_v2(bool value) override {
    celldb_v2_ = value;
  }
  void set_celldb_disable_bloom_filter(bool value) override {
    celldb_disable_bloom_filter_ = value;
  }
  void set_catchain_max_block_delay(double value) override {
    catchain_max_block_delay_ = value;
  }
  void set_catchain_max_block_delay_slow(double value) override {
    catchain_max_block_delay_slow_ = value;
  }
  void set_state_serializer_enabled(bool value) override {
    state_serializer_enabled_ = value;
  }
  void set_collator_options(td::Ref<CollatorOptions> value) override {
    collator_options_ = std::move(value);
  }
  void set_fast_state_serializer_enabled(bool value) override {
    fast_state_serializer_enabled_ = value;
  }
  void set_catchain_broadcast_speed_multiplier(double value) override {
    catchain_broadcast_speed_multipliers_ = value;
  }

  ValidatorManagerOptionsImpl *make_copy() const override {
    return new ValidatorManagerOptionsImpl(*this);
  }

  ValidatorManagerOptionsImpl(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                              std::function<bool(ShardIdFull)> check_shard, bool allow_blockchain_init,
                              double sync_blocks_before, double block_ttl, double state_ttl, double max_mempool_num,
                              double archive_ttl, double key_proof_ttl, bool initial_sync_disabled)
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
  std::function<bool(ShardIdFull)> check_shard_;
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
  std::string session_logs_file_;
  td::uint32 celldb_compress_depth_{0};
  size_t max_open_archive_files_ = 0;
  double archive_preload_period_ = 0.0;
  bool disable_rocksdb_stats_;
  bool nonfinal_ls_queries_enabled_ = false;
  td::optional<td::uint64> celldb_cache_size_;
  bool celldb_direct_io_ = false;
  bool celldb_preload_all_ = false;
  bool celldb_in_memory_ = false;
  bool celldb_v2_ = false;
  bool celldb_disable_bloom_filter_ = false;
  td::optional<double> catchain_max_block_delay_, catchain_max_block_delay_slow_;
  bool state_serializer_enabled_ = true;
  td::Ref<CollatorOptions> collator_options_{true};
  bool fast_state_serializer_enabled_ = false;
  double catchain_broadcast_speed_multipliers_;
};

}  // namespace validator

}  // namespace ton
