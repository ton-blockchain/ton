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
#include "ton/ton-types.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "interfaces/block-handle.h"
#include "interfaces/validator-manager.h"
#include "interfaces/shard.h"
#include "block.hpp"
#include "shard.hpp"
#include "proof.hpp"
#include "block/block-auto.h"


namespace ton {

namespace validator {
using td::Ref;

class LiteQuery : public td::actor::Actor {
  td::BufferSlice query_;
  td::actor::ActorId<ton::validator::ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::BufferSlice> promise_;

  td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> acc_state_promise_;

  int pending_{0};
  int mode_{0};
  WorkchainId acc_workchain_;
  StdSmcAddress acc_addr_;
  LogicalTime trans_lt_;
  Bits256 trans_hash_;
  BlockIdExt base_blk_id_, base_blk_id_alt_, blk_id_;
  Ref<MasterchainStateQ> mc_state_, mc_state0_;
  Ref<ShardStateQ> state_;
  Ref<BlockQ> mc_block_, block_;
  Ref<ProofQ> mc_proof_, mc_proof_alt_;
  Ref<ProofLinkQ> proof_link_;
  td::BufferSlice buffer_;
  std::function<void()> continuation_;
  bool cont_set_{false};
  td::BufferSlice shard_proof_;
  std::vector<Ref<vm::Cell>> roots_;
  std::vector<Ref<td::CntObject>> aux_objs_;
  std::vector<ton::BlockIdExt> blk_ids_;
  std::unique_ptr<block::BlockProofChain> chain_;
  Ref<vm::Stack> stack_;

 public:
  enum {
    default_timeout_msec = 4500,      // 4.5 seconds
    max_transaction_count = 16,       // fetch at most 16 transactions in one query
    client_method_gas_limit = 300000  // gas limit for liteServer.runSmcMethod
  };
  enum {
    ls_version = 0x101,
    ls_capabilities = 7
  };  // version 1.1; +1 = build block proof chains, +2 = masterchainInfoExt, +4 = runSmcMethod
  LiteQuery(td::BufferSlice data, td::actor::ActorId<ton::validator::ValidatorManager> manager,
            td::Promise<td::BufferSlice> promise);
  LiteQuery(WorkchainId wc, StdSmcAddress  acc_addr, td::actor::ActorId<ton::validator::ValidatorManager> manager,
            td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise);
  static void run_query(td::BufferSlice data, td::actor::ActorId<ton::validator::ValidatorManager> manager,
                        td::Promise<td::BufferSlice> promise);

  static void fetch_account_state(WorkchainId wc, StdSmcAddress  acc_addr, td::actor::ActorId<ton::validator::ValidatorManager> manager,
                                  td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise);

 private:
  bool fatal_error(td::Status error);
  bool fatal_error(std::string err_msg, int err_code = -400);
  bool fatal_error(int err_code, std::string err_msg = "");
  void abort_query(td::Status reason);
  void abort_query_ext(td::Status reason, std::string err_msg);
  bool finish_query(td::BufferSlice result);
  void alarm() override;
  void start_up() override;
  void perform_getTime();
  void perform_getVersion();
  void perform_getMasterchainInfo(int mode);
  void continue_getMasterchainInfo(Ref<MasterchainState> mc_state, BlockIdExt blkid, int mode);
  void gotMasterchainInfoForAccountState(Ref<MasterchainState> mc_state, BlockIdExt blkid, int mode);
  void perform_getBlock(BlockIdExt blkid);
  void continue_getBlock(BlockIdExt blkid, Ref<BlockData> block);
  void perform_getBlockHeader(BlockIdExt blkid, int mode);
  void continue_getBlockHeader(BlockIdExt blkid, int mode, Ref<BlockData> block);
  void perform_getState(BlockIdExt blkid);
  void continue_getState(BlockIdExt blkid, Ref<ShardState> state);
  void continue_getZeroState(BlockIdExt blkid, td::BufferSlice state);
  void perform_sendMessage(td::BufferSlice ext_msg);
  void perform_getAccountState(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, int mode);
  void continue_getAccountState_0(Ref<MasterchainState> mc_state, BlockIdExt blkid);
  void continue_getAccountState();
  void finish_getAccountState(td::BufferSlice shard_proof);
  void perform_fetchAccountState();
  void perform_runSmcMethod(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, int mode, td::int64 method_id,
                            td::BufferSlice params);
  void finish_runSmcMethod(td::BufferSlice shard_proof, td::BufferSlice state_proof, Ref<vm::Cell> acc_root,
                           UnixTime gen_utime, LogicalTime gen_lt);
  void perform_getLibraries(std::vector<td::Bits256> library_list);
  void continue_getLibraries(Ref<MasterchainState> mc_state, BlockIdExt blkid, std::vector<td::Bits256> library_list);
  void perform_getOneTransaction(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, LogicalTime lt);
  void continue_getOneTransaction();
  void perform_getTransactions(WorkchainId workchain, StdSmcAddress addr, LogicalTime lt, Bits256 hash, unsigned count);
  void continue_getTransactions(unsigned remaining, bool exact);
  void continue_getTransactions_2(BlockIdExt blkid, Ref<BlockData> block, unsigned remaining);
  void abort_getTransactions(td::Status error, ton::BlockIdExt blkid);
  void finish_getTransactions();
  void perform_getShardInfo(BlockIdExt blkid, ShardIdFull shard, bool exact);
  void perform_getAllShardsInfo(BlockIdExt blkid);
  void continue_getShardInfo(ShardIdFull shard, bool exact);
  void continue_getAllShardsInfo();
  void perform_getConfigParams(BlockIdExt blkid, int mode, std::vector<int> param_list = {});
  void continue_getConfigParams(int mode, std::vector<int> param_list);
  void perform_lookupBlock(BlockId blkid, int mode, LogicalTime lt, UnixTime utime);
  void perform_listBlockTransactions(BlockIdExt blkid, int mode, int count, Bits256 account, LogicalTime lt);
  void finish_listBlockTransactions(int mode, int count);
  void perform_getBlockProof(BlockIdExt from, BlockIdExt to, int mode);
  void continue_getBlockProof(BlockIdExt from, BlockIdExt to, int mode, BlockIdExt baseblk,
                              Ref<MasterchainStateQ> state);
  void perform_getValidatorStats(BlockIdExt blkid, int mode, int count, Bits256 start_after, UnixTime min_utime);
  void continue_getValidatorStats(int mode, int limit, Bits256 start_after, UnixTime min_utime);
  bool construct_proof_chain(BlockIdExt id);
  bool construct_proof_link_forward(ton::BlockIdExt cur, ton::BlockIdExt next);
  bool construct_proof_link_forward_cont(ton::BlockIdExt cur, ton::BlockIdExt next);
  bool construct_proof_link_back(ton::BlockIdExt cur, ton::BlockIdExt next);
  bool construct_proof_link_back_cont(ton::BlockIdExt cur, ton::BlockIdExt next);
  bool adjust_last_proof_link(ton::BlockIdExt cur, Ref<vm::Cell> block_root);
  bool finish_proof_chain(ton::BlockIdExt id);

  void load_prevKeyBlock(ton::BlockIdExt blkid, td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>>);
  void continue_loadPrevKeyBlock(ton::BlockIdExt blkid, td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res,
                                 td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>>);
  void finish_loadPrevKeyBlock(ton::BlockIdExt blkid, td::Result<Ref<BlockData>> res,
                               td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>> promise);

  bool request_block_data(BlockIdExt blkid);
  bool request_block_state(BlockIdExt blkid);
  bool request_block_data_state(BlockIdExt blkid);
  bool request_proof_link(BlockIdExt blkid);
  bool request_mc_block_data(BlockIdExt blkid);
  bool request_mc_block_state(BlockIdExt blkid);
  bool request_mc_block_data_state(BlockIdExt blkid);
  bool request_mc_proof(BlockIdExt blkid, int mode = 0);
  bool request_zero_state(BlockIdExt blkid);
  void got_block_state(BlockIdExt blkid, Ref<ShardState> state);
  void got_mc_block_state(BlockIdExt blkid, Ref<ShardState> state);
  void got_block_data(BlockIdExt blkid, Ref<BlockData> data);
  void got_mc_block_data(BlockIdExt blkid, Ref<BlockData> data);
  void got_mc_block_proof(BlockIdExt blkid, int mode, Ref<Proof> proof);
  void got_block_proof_link(BlockIdExt blkid, Ref<ProofLink> proof_link);
  void got_zero_state(BlockIdExt blkid, td::BufferSlice zerostate);
  void dec_pending() {
    if (!--pending_) {
      check_pending();
    }
  }
  void check_pending();
  bool set_continuation(std::function<void()>&& cont);
  bool make_mc_state_root_proof(Ref<vm::Cell>& proof);
  bool make_state_root_proof(Ref<vm::Cell>& proof);
  bool make_state_root_proof(Ref<vm::Cell>& proof, Ref<ShardStateQ> state, Ref<BlockData> block,
                             const BlockIdExt& blkid);
  bool make_state_root_proof(Ref<vm::Cell>& proof, Ref<vm::Cell> state_root, Ref<vm::Cell> block_root,
                             const BlockIdExt& blkid);
  bool make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, ShardIdFull shard,
                             ShardIdFull& true_shard, Ref<vm::Cell>& leaf, bool& found, bool exact = true);
  bool make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, ShardIdFull shard, bool exact = true);
  bool make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, AccountIdPrefixFull prefix);
  bool make_shard_info_proof(Ref<vm::Cell>& proof, BlockIdExt& blkid, AccountIdPrefixFull prefix);
  bool make_ancestor_block_proof(Ref<vm::Cell>& proof, Ref<vm::Cell> state_root, const BlockIdExt& old_blkid);
};

}  // namespace validator
}  // namespace ton
