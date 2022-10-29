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
#include "shard.hpp"
#include "top-shard-descr.hpp"
#include "common/refcnt.hpp"
#include "vm/cells.h"
#include "vm/dict.h"
#include "block/mc-config.h"
#include "block/block.h"
#include "block/transaction.h"
#include "block/block-db.h"
#include "block/output-queue-merger.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include <map>
#include <queue>

namespace ton {

namespace validator {
using td::Ref;

class Collator final : public td::actor::Actor {
  static constexpr int supported_version() {
    return 3;
  }
  static constexpr long long supported_capabilities() {
    return ton::capCreateStatsEnabled | ton::capBounceMsgBody | ton::capReportVersion | ton::capShortDequeue;
  }
  using LtCellRef = block::LtCellRef;
  using NewOutMsg = block::NewOutMsg;
  const ShardIdFull shard_;
  ton::BlockId new_id;
  bool busy_{false};
  bool before_split_{false};
  bool after_split_{false};
  bool after_merge_{false};
  bool want_split_{false};
  bool want_merge_{false};
  bool right_child_{false};
  bool preinit_complete{false};
  bool is_key_block_{false};
  bool block_full_{false};
  bool outq_cleanup_partial_{false};
  bool inbound_queues_empty_{false};
  bool libraries_changed_{false};
  bool prev_key_block_exists_{false};
  bool is_hardfork_{false};
  UnixTime min_ts;
  BlockIdExt min_mc_block_id;
  std::vector<BlockIdExt> prev_blocks;
  std::vector<Ref<ShardState>> prev_states;
  std::vector<Ref<BlockData>> prev_block_data;
  Ed25519_PublicKey created_by_;
  Ref<ValidatorSet> validator_set_;
  td::actor::ActorId<ValidatorManager> manager;
  td::Timestamp timeout;
  td::Timestamp soft_timeout_, medium_timeout_;
  td::Promise<BlockCandidate> main_promise;
  ton::BlockSeqno last_block_seqno{0};
  ton::BlockSeqno prev_mc_block_seqno{0};
  ton::BlockSeqno new_block_seqno{0};
  ton::BlockSeqno prev_key_block_seqno_{0};
  int step{0};
  int pending{0};
  static constexpr int max_ihr_msg_size = 65535;   // 64k
  static constexpr int max_ext_msg_size = 65535;   // 64k
  static constexpr int max_blk_sign_size = 65535;  // 64k
  static constexpr bool shard_splitting_enabled = true;

 public:
  Collator(ShardIdFull shard, bool is_hardfork, td::uint32 min_ts, BlockIdExt min_masterchain_block_id,
           std::vector<BlockIdExt> prev, Ref<ValidatorSet> validator_set, Ed25519_PublicKey collator_id,
           td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout, td::Promise<BlockCandidate> promise);
  ~Collator() override = default;
  bool is_busy() const {
    return busy_;
  }
  ShardId get_shard() const {
    return shard_.shard;
  }
  WorkchainId workchain() const {
    return shard_.workchain;
  }
  static constexpr td::uint32 priority() {
    return 2;
  }

  static td::Result<std::unique_ptr<block::ConfigInfo>>
                     impl_fetch_config_params(std::unique_ptr<block::ConfigInfo> config,
                                              Ref<vm::Cell>* old_mparams,
                                              std::vector<block::StoragePrices>* storage_prices,
                                              block::StoragePhaseConfig* storage_phase_cfg,
                                              td::BitArray<256>* rand_seed,
                                              block::ComputePhaseConfig* compute_phase_cfg,
                                              block::ActionPhaseConfig* action_phase_cfg,
                                              td::RefInt256* masterchain_create_fee,
                                              td::RefInt256* basechain_create_fee,
                                              WorkchainId wc);

  static td::Result<std::unique_ptr<block::Transaction>>
                        impl_create_ordinary_transaction(Ref<vm::Cell> msg_root,
                                                         block::Account* acc,
                                                         UnixTime utime, LogicalTime lt,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg,
                                                         bool external, LogicalTime after_lt);

 private:
  void start_up() override;
  void alarm() override;
  int verbosity{3 * 0};
  int verify{1};
  ton::LogicalTime start_lt, max_lt;
  ton::UnixTime now_;
  ton::UnixTime prev_now_;
  ton::UnixTime now_upper_limit_{~0U};
  unsigned out_msg_queue_ops_{}, in_descr_cnt_{}, out_descr_cnt_{};
  Ref<MasterchainStateQ> mc_state_;
  Ref<BlockData> prev_mc_block;
  BlockIdExt mc_block_id_;
  Ref<vm::Cell> mc_state_root;
  Ref<vm::Cell> mc_block_root;
  td::BitArray<256> rand_seed_;
  std::unique_ptr<block::ConfigInfo> config_;
  std::unique_ptr<block::ShardConfig> shard_conf_;
  std::map<BlockSeqno, Ref<MasterchainStateQ>> aux_mc_states_;
  std::vector<block::McShardDescr> neighbors_;
  std::unique_ptr<block::OutputQueueMerger> nb_out_msgs_;
  std::vector<ton::StdSmcAddress> special_smcs;
  std::vector<std::pair<ton::StdSmcAddress, int>> ticktock_smcs;
  Ref<vm::Cell> prev_block_root;
  Ref<vm::Cell> prev_state_root_, prev_state_root_pure_;
  Ref<vm::Cell> state_root;                              // (new) shardchain state
  Ref<vm::Cell> state_update;                            // Merkle update from prev_state_root to state_root
  std::shared_ptr<vm::CellUsageTree> state_usage_tree_;  // used to construct Merkle update
  Ref<vm::CellSlice> new_config_params_;
  Ref<vm::Cell> old_mparams_;
  ton::LogicalTime prev_state_lt_;
  ton::LogicalTime shards_max_end_lt_{0};
  ton::UnixTime prev_state_utime_;
  int global_id_{0};
  ton::BlockSeqno min_ref_mc_seqno_{~0U};
  ton::BlockSeqno vert_seqno_{~0U}, prev_vert_seqno_{~0U};
  ton::BlockIdExt prev_key_block_;
  ton::LogicalTime prev_key_block_lt_;
  bool accept_msgs_{true};
  bool shard_conf_adjusted_{false};
  bool ihr_enabled_{false};
  bool create_stats_enabled_{false};
  bool report_version_{false};
  bool skip_topmsgdescr_{false};
  bool skip_extmsg_{false};
  bool short_dequeue_records_{false};
  td::uint64 overload_history_{0}, underload_history_{0};
  td::uint64 block_size_estimate_{};
  Ref<block::WorkchainInfo> wc_info_;
  std::vector<Ref<ShardTopBlockDescription>> shard_block_descr_;
  std::vector<Ref<ShardTopBlockDescrQ>> used_shard_block_descr_;
  std::unique_ptr<vm::Dictionary> shard_libraries_;
  Ref<vm::Cell> mc_state_extra_;
  std::unique_ptr<vm::AugmentedDictionary> account_dict;
  std::map<ton::StdSmcAddress, std::unique_ptr<block::Account>> accounts;
  std::vector<block::StoragePrices> storage_prices_;
  block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
  block::ComputePhaseConfig compute_phase_cfg_;
  block::ActionPhaseConfig action_phase_cfg_;
  td::RefInt256 masterchain_create_fee_, basechain_create_fee_;
  std::unique_ptr<block::BlockLimits> block_limits_;
  std::unique_ptr<block::BlockLimitStatus> block_limit_status_;
  ton::LogicalTime min_new_msg_lt{std::numeric_limits<td::uint64>::max()};
  block::CurrencyCollection total_balance_, old_total_balance_, total_validator_fees_;
  block::CurrencyCollection global_balance_, old_global_balance_, import_created_{0};
  Ref<vm::Cell> recover_create_msg_, mint_msg_;
  Ref<vm::Cell> new_block;
  block::ValueFlow value_flow_{block::ValueFlow::SetZero()};
  std::unique_ptr<vm::AugmentedDictionary> fees_import_dict_;
  std::map<ton::Bits256, int> ext_msg_map;
  std::vector<std::pair<Ref<vm::Cell>, ExtMessage::Hash>> ext_msg_list_;
  std::priority_queue<NewOutMsg, std::vector<NewOutMsg>, std::greater<NewOutMsg>> new_msgs;
  std::pair<ton::LogicalTime, ton::Bits256> last_proc_int_msg_, first_unproc_int_msg_;
  std::unique_ptr<vm::AugmentedDictionary> in_msg_dict, out_msg_dict, out_msg_queue_, sibling_out_msg_queue_;
  std::unique_ptr<vm::Dictionary> ihr_pending;
  std::shared_ptr<block::MsgProcessedUptoCollection> processed_upto_, sibling_processed_upto_;
  std::unique_ptr<vm::Dictionary> block_create_stats_;
  std::map<td::Bits256, int> block_create_count_;
  unsigned block_create_total_{0};
  std::vector<ExtMessage::Hash> bad_ext_msgs_, delay_ext_msgs_;
  Ref<vm::Cell> shard_account_blocks_;  // ShardAccountBlocks
  std::vector<Ref<vm::Cell>> collated_roots_;
  std::unique_ptr<ton::BlockCandidate> block_candidate;

  td::PerfWarningTimer perf_timer_;
  //
  block::Account* lookup_account(td::ConstBitPtr addr) const;
  std::unique_ptr<block::Account> make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                    Ref<vm::CellSlice> extra, bool force_create = false);
  td::Result<block::Account*> make_account(td::ConstBitPtr addr, bool force_create = false);
  td::actor::ActorId<Collator> get_self() {
    return actor_id(this);
  }
  bool init_utime();
  bool init_lt();
  bool fetch_config_params();
  bool fatal_error(td::Status error);
  bool fatal_error(int err_code, std::string err_msg);
  bool fatal_error(std::string err_msg, int err_code = -666);
  void check_pending();
  void after_get_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res);
  void after_get_shard_state(int idx, td::Result<Ref<ShardState>> res);
  void after_get_block_data(int idx, td::Result<Ref<BlockData>> res);
  void after_get_shard_blocks(td::Result<std::vector<Ref<ShardTopBlockDescription>>> res);
  bool preprocess_prev_mc_state();
  bool register_mc_state(Ref<MasterchainStateQ> other_mc_state);
  bool request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state);
  Ref<MasterchainStateQ> get_aux_mc_state(BlockSeqno seqno) const;
  void after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res);
  bool fix_one_processed_upto(block::MsgProcessedUpto& proc, const ton::ShardIdFull& owner);
  bool fix_processed_upto(block::MsgProcessedUptoCollection& upto);
  void got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res);
  bool adjust_shard_config();
  bool store_shard_fees(ShardIdFull shard, const block::CurrencyCollection& fees,
                        const block::CurrencyCollection& created);
  bool store_shard_fees(Ref<block::McShardHash> descr);
  bool import_new_shard_top_blocks();
  bool register_shard_block_creators(std::vector<td::Bits256> creator_list);
  bool init_block_limits();
  bool compute_minted_amount(block::CurrencyCollection& to_mint);
  bool init_value_create();
  bool try_collate();
  bool do_preinit();
  bool do_collate();
  bool create_special_transactions();
  bool create_special_transaction(block::CurrencyCollection amount, Ref<vm::Cell> dest_addr_cell,
                                  Ref<vm::Cell>& in_msg);
  bool create_ticktock_transactions(int mask);
  bool create_ticktock_transaction(const ton::StdSmcAddress& smc_addr, ton::LogicalTime req_start_lt, int mask);
  Ref<vm::Cell> create_ordinary_transaction(Ref<vm::Cell> msg_root);
  bool check_cur_validator_set();
  bool unpack_last_mc_state();
  bool unpack_last_state();
  bool unpack_merge_last_state();
  bool unpack_one_last_state(block::ShardState& ss, BlockIdExt blkid, Ref<vm::Cell> prev_state_root);
  bool split_last_state(block::ShardState& ss);
  bool import_shard_state_data(block::ShardState& ss);
  bool add_trivial_neighbor();
  bool add_trivial_neighbor_after_merge();
  bool out_msg_queue_cleanup();
  bool dequeue_message(Ref<vm::Cell> msg_envelope, ton::LogicalTime delivered_lt);
  bool check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len = true);
  bool check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev);
  bool check_this_shard_mc_info();
  bool request_neighbor_msg_queues();
  void update_max_lt(ton::LogicalTime lt);
  bool is_masterchain() const {
    return shard_.is_masterchain();
  }
  bool is_our_address(Ref<vm::CellSlice> addr_ref) const;
  bool is_our_address(ton::AccountIdPrefixFull addr_prefix) const;
  bool is_our_address(const ton::StdSmcAddress& addr) const;
  void after_get_external_messages(td::Result<std::vector<Ref<ExtMessage>>> res);
  td::Result<bool> register_external_message_cell(Ref<vm::Cell> ext_msg, const ExtMessage::Hash& ext_hash);
  // td::Result<bool> register_external_message(td::Slice ext_msg_boc);
  td::Result<bool> register_ihr_message_cell(Ref<vm::Cell> ihr_msg);
  td::Result<bool> register_ihr_message(td::Slice ihr_msg_boc);
  td::Result<bool> register_shard_signatures_cell(Ref<vm::Cell> shard_blk_signatures);
  td::Result<bool> register_shard_signatures(td::Slice shard_blk_signatures_boc);
  void register_new_msg(block::NewOutMsg msg);
  void register_new_msgs(block::Transaction& trans);
  bool process_new_messages(bool enqueue_only = false);
  int process_one_new_message(block::NewOutMsg msg, bool enqueue_only = false, Ref<vm::Cell>* is_special = nullptr);
  bool process_inbound_internal_messages();
  bool process_inbound_message(Ref<vm::CellSlice> msg, ton::LogicalTime lt, td::ConstBitPtr key,
                               const block::McShardDescr& src_nb);
  bool process_inbound_external_messages();
  int process_external_message(Ref<vm::Cell> msg);
  bool enqueue_message(block::NewOutMsg msg, td::RefInt256 fwd_fees_remaining, ton::LogicalTime enqueued_lt);
  bool enqueue_transit_message(Ref<vm::Cell> msg, Ref<vm::Cell> old_msg_env, ton::AccountIdPrefixFull prev_prefix,
                               ton::AccountIdPrefixFull cur_prefix, ton::AccountIdPrefixFull dest_prefix,
                               td::RefInt256 fwd_fee_remaining, ton::LogicalTime enqueued_lt);
  bool delete_out_msg_queue_msg(td::ConstBitPtr key);
  bool insert_in_msg(Ref<vm::Cell> in_msg);
  bool insert_out_msg(Ref<vm::Cell> out_msg);
  bool insert_out_msg(Ref<vm::Cell> out_msg, td::ConstBitPtr msg_hash);
  bool register_out_msg_queue_op(bool force = false);
  bool update_min_mc_seqno(ton::BlockSeqno some_mc_seqno);
  bool combine_account_transactions();
  bool update_public_libraries();
  bool update_account_public_libraries(Ref<vm::Cell> orig_libs, Ref<vm::Cell> final_libs, const td::Bits256& addr);
  bool add_public_library(td::ConstBitPtr key, td::ConstBitPtr addr, Ref<vm::Cell> library);
  bool remove_public_library(td::ConstBitPtr key, td::ConstBitPtr addr);
  bool check_block_overload();
  bool update_block_creator_count(td::ConstBitPtr key, unsigned shard_incr, unsigned mc_incr);
  int creator_count_outdated(td::ConstBitPtr key, vm::CellSlice& cs);
  bool update_block_creator_stats();
  bool create_mc_state_extra();
  bool create_shard_state();
  td::Result<Ref<vm::Cell>> get_config_data_from_smc(const ton::StdSmcAddress& cfg_addr);
  bool try_fetch_new_config(const ton::StdSmcAddress& cfg_addr, Ref<vm::Cell>& new_config);
  bool update_processed_upto();
  bool compute_out_msg_queue_info(Ref<vm::Cell>& out_msg_queue_info);
  bool compute_total_balance();
  bool store_master_ref(vm::CellBuilder& cb);
  bool store_prev_blk_ref(vm::CellBuilder& cb, bool after_merge);
  bool store_zero_state_ref(vm::CellBuilder& cb);
  bool store_version(vm::CellBuilder& cb) const;
  bool create_block_info(Ref<vm::Cell>& block_info);
  bool check_value_flow();
  bool create_block_extra(Ref<vm::Cell>& block_extra);
  bool update_shard_config(const block::WorkchainSet& wc_set, const block::CatchainValidatorsConfig& ccvc,
                           bool update_cc);
  bool create_mc_block_extra(Ref<vm::Cell>& mc_block_extra);
  bool create_block();
  Ref<vm::Cell> collate_shard_block_descr_set();
  bool create_collated_data();
  bool create_block_candidate();
  void return_block_candidate(td::Result<td::Unit> saved);
  bool update_last_proc_int_msg(const std::pair<ton::LogicalTime, ton::Bits256>& new_lt_hash);
};

}  // namespace validator

}  // namespace ton
