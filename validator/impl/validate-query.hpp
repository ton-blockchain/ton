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

#include "interfaces/validator-manager.h"
#include "vm/cells.h"
#include "vm/dict.h"
#include "block/mc-config.h"
#include "block/transaction.h"
#include "shard.hpp"
#include "signature-set.hpp"
#include <vector>
#include <string>
#include <map>

namespace ton {

namespace validator {
using td::Ref;

class ErrorCtxAdd;
class ErrorCtxSet;

struct ErrorCtx {
 protected:
  friend class ErrorCtxAdd;
  friend class ErrorCtxSet;
  std::vector<std::string> entries_;

 public:
  ErrorCtx() = default;
  ErrorCtx(std::vector<std::string> str_list) : entries_(std::move(str_list)) {
  }
  ErrorCtx(std::string str) : entries_{str} {
  }
  std::string as_string() const;
  ErrorCtxAdd add_guard(std::string str_add);
  ErrorCtxSet set_guard(std::string str);
  ErrorCtxSet set_guard(std::vector<std::string> str_list);
};

class ErrorCtxAdd {
  ErrorCtx& ctx_;

 public:
  ErrorCtxAdd(ErrorCtx& ctx, std::string ctx_elem) : ctx_(ctx) {
    ctx_.entries_.push_back(std::move(ctx_elem));
  }
  ~ErrorCtxAdd() {
    ctx_.entries_.pop_back();
  }
};

class ErrorCtxSet {
  ErrorCtx& ctx_;
  std::vector<std::string> old_ctx_;

 public:
  ErrorCtxSet(ErrorCtx& ctx, std::vector<std::string> new_ctx) : ctx_(ctx) {
    old_ctx_ = std::move(ctx_.entries_);
    ctx_.entries_ = std::move(new_ctx);
  }
  ErrorCtxSet(ErrorCtx& ctx, std::string new_ctx) : ErrorCtxSet(ctx, std::vector<std::string>{new_ctx}) {
  }
  ~ErrorCtxSet() {
    ctx_.entries_ = std::move(old_ctx_);
  }
};

inline ErrorCtxAdd ErrorCtx::add_guard(std::string str) {
  return ErrorCtxAdd(*this, std::move(str));
}

inline ErrorCtxSet ErrorCtx::set_guard(std::string str) {
  return ErrorCtxSet(*this, std::move(str));
}

inline ErrorCtxSet ErrorCtx::set_guard(std::vector<std::string> str_list) {
  return ErrorCtxSet(*this, std::move(str_list));
}

/*
 *
 * must write candidate to disk, if accepted
 * can reject block only if it is invalid (i.e. in case of 
 * internal errors must retry or crash)
 * only exception: block can be rejected, if it is known from 
 * masterchain, that it will not be part of shardchain finalized
 * state
 *
 */

class ValidateQuery : public td::actor::Actor {
  static constexpr int supported_version() {
    return 3;
  }
  static constexpr long long supported_capabilities() {
    return ton::capCreateStatsEnabled | ton::capBounceMsgBody | ton::capReportVersion | ton::capShortDequeue;
  }

 public:
  ValidateQuery(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id, std::vector<BlockIdExt> prev,
                BlockCandidate candidate, td::Ref<ValidatorSet> validator_set,
                td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                td::Promise<ValidateCandidateResult> promise, bool is_fake = false);

 private:
  int verbosity{3 * 1};
  int pending{0};
  const ShardIdFull shard_;
  const BlockIdExt id_;
  UnixTime min_ts;
  BlockIdExt min_mc_block_id;
  std::vector<BlockIdExt> prev_blocks;
  std::vector<Ref<ShardState>> prev_states;
  BlockCandidate block_candidate;
  td::Ref<ValidatorSet> validator_set_;
  td::actor::ActorId<ValidatorManager> manager;
  td::Timestamp timeout;
  td::Promise<ValidateCandidateResult> main_promise;
  bool after_merge_{false};
  bool after_split_{false};
  bool before_split_{false};
  bool want_split_{false};
  bool want_merge_{false};
  bool is_key_block_{false};
  bool update_shard_cc_{false};
  bool is_fake_{false};
  bool prev_key_block_exists_{false};
  bool debug_checks_{false};
  bool outq_cleanup_partial_{false};
  BlockSeqno prev_key_seqno_{~0u};
  int stage_{0};
  td::BitArray<64> shard_pfx_;
  int shard_pfx_len_;
  td::Bits256 created_by_;

  Ref<vm::Cell> prev_state_root_;
  Ref<vm::Cell> state_root_;
  Ref<vm::Cell> state_update_;
  ton::Bits256 prev_state_hash_, state_hash_;

  ErrorCtx error_ctx_;

  td::Ref<MasterchainStateQ> mc_state_, latest_mc_state_;
  td::Ref<vm::Cell> mc_state_root_;
  BlockIdExt mc_blkid_, latest_mc_blkid_;
  ton::BlockSeqno mc_seqno_{0}, latest_mc_seqno_;

  Ref<vm::Cell> block_root_;
  std::vector<Ref<vm::Cell>> collated_roots_;
  std::map<RootHash, Ref<vm::Cell>> virt_roots_;
  std::unique_ptr<vm::Dictionary> top_shard_descr_dict_;

  Ref<vm::CellSlice> shard_hashes_;              // from McBlockExtra
  Ref<vm::CellSlice> blk_config_params_;         // from McBlockExtra
  Ref<BlockSignatureSet> prev_signatures_;       // from McBlockExtra (UNCHECKED)
  Ref<vm::Cell> recover_create_msg_, mint_msg_;  // from McBlockExtra (UNCHECKED)

  std::unique_ptr<block::ConfigInfo> config_, new_config_;
  std::unique_ptr<block::ShardConfig> old_shard_conf_;  // from reference mc state
  std::unique_ptr<block::ShardConfig> new_shard_conf_;  // from shard_hashes_ in mc blocks
  Ref<block::WorkchainInfo> wc_info_;
  std::unique_ptr<vm::AugmentedDictionary> fees_import_dict_;
  Ref<vm::Cell> old_mparams_;
  bool accept_msgs_{true};

  ton::BlockSeqno min_shard_ref_mc_seqno_{~0U};
  ton::UnixTime max_shard_utime_{0};
  ton::LogicalTime max_shard_lt_{0};

  int global_id_{0};
  ton::BlockSeqno vert_seqno_{~0U};
  bool ihr_enabled_{false};
  bool create_stats_enabled_{false};
  ton::BlockSeqno prev_key_block_seqno_;
  ton::BlockIdExt prev_key_block_;
  ton::LogicalTime prev_key_block_lt_;
  std::unique_ptr<block::BlockLimits> block_limits_;
  std::unique_ptr<block::BlockLimitStatus> block_limit_status_;

  LogicalTime start_lt_, end_lt_;
  UnixTime prev_now_{~0u}, now_{~0u};

  ton::Bits256 rand_seed_;
  std::vector<block::StoragePrices> storage_prices_;
  block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
  block::ComputePhaseConfig compute_phase_cfg_;
  block::ActionPhaseConfig action_phase_cfg_;
  td::RefInt256 masterchain_create_fee_, basechain_create_fee_;

  std::vector<block::McShardDescr> neighbors_;
  std::map<BlockSeqno, Ref<MasterchainStateQ>> aux_mc_states_;

  block::ShardState ps_, ns_;
  std::unique_ptr<vm::AugmentedDictionary> sibling_out_msg_queue_;
  std::shared_ptr<block::MsgProcessedUptoCollection> sibling_processed_upto_;

  std::map<td::Bits256, int> block_create_count_;
  unsigned block_create_total_{0};

  std::unique_ptr<vm::AugmentedDictionary> in_msg_dict_, out_msg_dict_, account_blocks_dict_;
  block::ValueFlow value_flow_;
  block::CurrencyCollection import_created_, transaction_fees_;
  td::RefInt256 import_fees_;

  ton::LogicalTime proc_lt_{0}, claimed_proc_lt_{0}, min_enq_lt_{~0ULL};
  ton::Bits256 proc_hash_, claimed_proc_hash_, min_enq_hash_;
  bool inbound_queues_empty_{false};

  std::vector<std::tuple<Bits256, LogicalTime, LogicalTime>> msg_proc_lt_;

  std::vector<std::tuple<Bits256, Bits256, bool>> lib_publishers_, lib_publishers2_;

  td::PerfWarningTimer perf_timer_;

  static constexpr td::uint32 priority() {
    return 2;
  }
  WorkchainId workchain() const {
    return shard_.workchain;
  }

  void finish_query();
  void abort_query(td::Status error);
  bool reject_query(std::string error, td::BufferSlice reason = {});
  bool reject_query(std::string err_msg, td::Status error, td::BufferSlice reason = {});
  bool soft_reject_query(std::string error, td::BufferSlice reason = {});
  void alarm() override;
  void start_up() override;

  bool save_candidate();
  void written_candidate();

  bool fatal_error(td::Status error);
  bool fatal_error(int err_code, std::string err_msg);
  bool fatal_error(int err_code, std::string err_msg, td::Status error);
  bool fatal_error(std::string err_msg, int err_code = -666);

  std::string error_ctx() const {
    return error_ctx_.as_string();
  }
  ErrorCtxAdd error_ctx_add_guard(std::string str) {
    return error_ctx_.add_guard(std::move(str));
  }
  ErrorCtxSet error_ctx_set_guard(std::string str) {
    return error_ctx_.set_guard(std::move(str));
  }

  bool is_masterchain() const {
    return shard_.is_masterchain();
  }
  td::actor::ActorId<ValidateQuery> get_self() {
    return actor_id(this);
  }

  void after_get_latest_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res);
  void after_get_mc_state(td::Result<Ref<ShardState>> res);
  void got_mc_handle(td::Result<BlockHandle> res);
  void after_get_shard_state(int idx, td::Result<Ref<ShardState>> res);
  bool process_mc_state(Ref<MasterchainState> mc_state);
  bool try_unpack_mc_state();
  bool fetch_config_params();
  bool check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len = true);
  bool check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev);
  bool check_this_shard_mc_info();
  bool init_parse();
  bool unpack_block_candidate();
  bool extract_collated_data_from(Ref<vm::Cell> croot, int idx);
  bool extract_collated_data();
  bool try_validate();
  bool compute_prev_state();
  bool compute_next_state();
  bool unpack_merge_prev_state();
  bool unpack_prev_state();
  bool unpack_next_state();
  bool unpack_one_prev_state(block::ShardState& ss, BlockIdExt blkid, Ref<vm::Cell> prev_state_root);
  bool split_prev_state(block::ShardState& ss);
  bool request_neighbor_queues();
  void got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res);

  bool register_mc_state(Ref<MasterchainStateQ> other_mc_state);
  bool request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state);
  Ref<MasterchainStateQ> get_aux_mc_state(BlockSeqno seqno) const;
  void after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res);

  bool check_one_shard(const block::McShardHash& info, const block::McShardHash* sibling,
                       const block::WorkchainInfo* wc_info, const block::CatchainValidatorsConfig& ccvc);
  bool check_shard_layout();
  bool register_shard_block_creators(std::vector<td::Bits256> creator_list);
  bool check_cur_validator_set();
  bool check_mc_validator_info(bool update_mc_cc);
  bool check_utime_lt();

  bool fix_one_processed_upto(block::MsgProcessedUpto& proc, ton::ShardIdFull owner, bool allow_cur = false);
  bool fix_processed_upto(block::MsgProcessedUptoCollection& upto, bool allow_cur = false);
  bool fix_all_processed_upto();
  bool add_trivial_neighbor_after_merge();
  bool add_trivial_neighbor();
  bool unpack_block_data();
  bool unpack_precheck_value_flow(Ref<vm::Cell> value_flow_root);
  bool compute_minted_amount(block::CurrencyCollection& to_mint);
  bool precheck_one_account_update(td::ConstBitPtr acc_id, Ref<vm::CellSlice> old_value, Ref<vm::CellSlice> new_value);
  bool precheck_account_updates();
  bool precheck_one_transaction(td::ConstBitPtr acc_id, ton::LogicalTime trans_lt, Ref<vm::CellSlice> trans_csr,
                                ton::Bits256& prev_trans_hash, ton::LogicalTime& prev_trans_lt,
                                unsigned& prev_trans_lt_len, ton::Bits256& acc_state_hash);
  bool precheck_one_account_block(td::ConstBitPtr acc_id, Ref<vm::CellSlice> acc_blk);
  bool precheck_account_transactions();
  Ref<vm::Cell> lookup_transaction(const ton::StdSmcAddress& addr, ton::LogicalTime lt) const;
  bool is_valid_transaction_ref(Ref<vm::Cell> trans_ref) const;
  bool precheck_one_message_queue_update(td::ConstBitPtr out_msg_id, Ref<vm::CellSlice> old_value,
                                         Ref<vm::CellSlice> new_value);
  bool precheck_message_queue_update();
  bool update_max_processed_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash);
  bool update_min_enqueued_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash);
  bool check_imported_message(Ref<vm::Cell> msg_env);
  bool is_special_in_msg(const vm::CellSlice& in_msg) const;
  bool check_in_msg(td::ConstBitPtr key, Ref<vm::CellSlice> in_msg);
  bool check_in_msg_descr();
  bool check_out_msg(td::ConstBitPtr key, Ref<vm::CellSlice> out_msg);
  bool check_out_msg_descr();
  bool check_processed_upto();
  bool check_neighbor_outbound_message(Ref<vm::CellSlice> enq_msg, ton::LogicalTime lt, td::ConstBitPtr key,
                                       const block::McShardDescr& src_nb, bool& unprocessed);
  bool check_in_queue();
  bool check_delivered_dequeued();
  std::unique_ptr<block::Account> make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                    Ref<vm::CellSlice> extra);
  std::unique_ptr<block::Account> unpack_account(td::ConstBitPtr addr);
  bool check_one_transaction(block::Account& account, LogicalTime lt, Ref<vm::Cell> trans_root, bool is_first,
                             bool is_last);
  bool check_account_transactions(const StdSmcAddress& acc_addr, Ref<vm::CellSlice> acc_tr);
  bool check_transactions();
  bool scan_account_libraries(Ref<vm::Cell> orig_libs, Ref<vm::Cell> final_libs, const td::Bits256& addr);
  bool check_all_ticktock_processed();
  bool check_message_processing_order();
  bool check_special_message(Ref<vm::Cell> in_msg_root, const block::CurrencyCollection& amount,
                             Ref<vm::Cell> addr_cell);
  bool check_special_messages();
  bool check_one_library_update(td::ConstBitPtr key, Ref<vm::CellSlice> old_value, Ref<vm::CellSlice> new_value);
  bool check_shard_libraries();
  bool check_new_state();
  bool check_config_update(Ref<vm::CellSlice> old_conf_params, Ref<vm::CellSlice> new_conf_params);
  bool check_one_prev_dict_update(ton::BlockSeqno seqno, Ref<vm::CellSlice> old_val_extra,
                                  Ref<vm::CellSlice> new_val_extra);
  bool check_mc_state_extra();
  td::Status check_counter_update(const block::DiscountedCounter& oc, const block::DiscountedCounter& nc,
                                  unsigned expected_incr);
  bool check_one_block_creator_update(td::ConstBitPtr key, Ref<vm::CellSlice> old_val, Ref<vm::CellSlice> new_val);
  bool check_block_create_stats();
  bool check_one_shard_fee(ShardIdFull shard, const block::CurrencyCollection& fees,
                           const block::CurrencyCollection& create);
  bool check_mc_block_extra();
};

}  // namespace validator

}  // namespace ton
