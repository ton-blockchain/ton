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
#include "liteserver.hpp"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/overloaded.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "adnl/utils.hpp"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"
#include "td/utils/Random.h"
#include "vm/boc.h"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/check-proof.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/memo.h"
#include "shard.hpp"
#include "validator-set.hpp"
#include "signature-set.hpp"
#include "fabric.h"
#include <ctime>
#include "td/actor/MultiPromise.h"
#include "collator-impl.h"

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

td::int32 get_tl_tag(td::Slice slice) {
  return slice.size() >= 4 ? td::as<td::int32>(slice.data()) : -1;
}

void LiteQuery::run_query(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                          td::actor::ActorId<LiteServerCache> cache,
                          td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<LiteQuery>("litequery", std::move(data), std::move(manager), std::move(cache),
                                     std::move(promise))
      .release();
}

void LiteQuery::fetch_account_state(WorkchainId wc, StdSmcAddress  acc_addr, td::actor::ActorId<ton::validator::ValidatorManager> manager,
                                 td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise) {
  td::actor::create_actor<LiteQuery>("litequery", wc, acc_addr, std::move(manager), std::move(promise)).release();
}

LiteQuery::LiteQuery(td::BufferSlice data, td::actor::ActorId<ValidatorManager> manager,
                     td::actor::ActorId<LiteServerCache> cache, td::Promise<td::BufferSlice> promise)
    : query_(std::move(data)), manager_(std::move(manager)), cache_(std::move(cache)), promise_(std::move(promise)) {
  timeout_ = td::Timestamp::in(default_timeout_msec * 0.001);
}

LiteQuery::LiteQuery(WorkchainId wc, StdSmcAddress  acc_addr, td::actor::ActorId<ValidatorManager> manager,
                     td::Promise<std::tuple<td::Ref<vm::CellSlice>,UnixTime,LogicalTime,std::unique_ptr<block::ConfigInfo>>> promise)
    : manager_(std::move(manager)), acc_state_promise_(std::move(promise)), acc_workchain_(wc), acc_addr_(acc_addr) {
  timeout_ = td::Timestamp::in(default_timeout_msec * 0.001);
}

void LiteQuery::abort_query(td::Status reason) {
  LOG(INFO) << "aborted liteserver query: " << reason.to_string();
  if (acc_state_promise_) {
    acc_state_promise_.set_error(std::move(reason));
  } else if (promise_) {
    td::actor::send_closure(manager_, &ValidatorManager::add_lite_query_stats, query_obj_ ? query_obj_->get_id() : 0,
                            false);
    promise_.set_error(std::move(reason));
  }
  stop();
}

bool LiteQuery::fatal_error(td::Status error) {
  abort_query(std::move(error));
  return false;
}

bool LiteQuery::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, std::move(err_msg)));
}

bool LiteQuery::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

void LiteQuery::alarm() {
  fatal_error(-503, "timeout");
}

bool LiteQuery::finish_query(td::BufferSlice result, bool skip_cache_update) {
  if (use_cache_ && !skip_cache_update) {
    td::actor::send_closure(cache_, &LiteServerCache::update, cache_key_, result.clone());
  }
  if (promise_) {
    td::actor::send_closure(manager_, &ValidatorManager::add_lite_query_stats, query_obj_ ? query_obj_->get_id() : 0,
                            true);
    promise_.set_result(std::move(result));
    stop();
    return true;
  } else {
    stop();
    return false;
  }
}

void LiteQuery::start_up() {
  alarm_timestamp() = timeout_;

  if (acc_state_promise_) {
    td::actor::send_closure_later(actor_id(this), &LiteQuery::perform_fetchAccountState);
    return;
  }

  auto F = fetch_tl_object<ton::lite_api::Function>(query_, true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  query_obj_ = F.move_as_ok();

  if (!cache_.empty() && query_obj_->get_id() == lite_api::liteServer_sendMessage::ID) {
    // Dropping duplicate "sendMessage"
    cache_key_ = td::sha256_bits256(query_);
    td::actor::send_closure(cache_, &LiteServerCache::process_send_message, cache_key_,
                            [SelfId = actor_id(this)](td::Result<td::Unit> R) {
                              if (R.is_ok()) {
                                td::actor::send_closure(SelfId, &LiteQuery::perform);
                              } else {
                                td::actor::send_closure(SelfId, &LiteQuery::abort_query,
                                                        R.move_as_error_prefix("cannot send external message : "));
                              }
                            });
    return;
  }
  use_cache_ = use_cache();
  if (use_cache_) {
    cache_key_ = td::sha256_bits256(query_);
    td::actor::send_closure(
        cache_, &LiteServerCache::lookup, cache_key_, [SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &LiteQuery::perform);
          } else {
            td::actor::send_closure(SelfId, &LiteQuery::finish_query, R.move_as_ok(), true);
          }
        });
  } else {
    perform();
  }
}

bool LiteQuery::use_cache()  {
  if (cache_.empty()) {
    return false;
  }
  bool use = false;
  lite_api::downcast_call(
      *query_obj_,
      td::overloaded(
          [&](lite_api::liteServer_runSmcMethod& q) {
            // wc=-1, seqno=-1 means "use latest mc block"
            use = q.id_->workchain_ != masterchainId || q.id_->seqno_ != -1;
          },
          [&](auto& obj) { use = false; }));
  return use;
}

void LiteQuery::perform() {
  lite_api::downcast_call(
      *query_obj_,
      td::overloaded(
          [&](lite_api::liteServer_getTime& q) { this->perform_getTime(); },
          [&](lite_api::liteServer_getVersion& q) { this->perform_getVersion(); },
          [&](lite_api::liteServer_getMasterchainInfo& q) { this->perform_getMasterchainInfo(-1); },
          [&](lite_api::liteServer_getMasterchainInfoExt& q) {
            this->perform_getMasterchainInfo(q.mode_ & 0x7fffffff);
          },
          [&](lite_api::liteServer_getBlock& q) { this->perform_getBlock(ton::create_block_id(q.id_)); },
          [&](lite_api::liteServer_getBlockHeader& q) {
            this->perform_getBlockHeader(ton::create_block_id(q.id_), q.mode_);
          },
          [&](lite_api::liteServer_getState& q) { this->perform_getState(ton::create_block_id(q.id_)); },
          [&](lite_api::liteServer_getAccountState& q) {
            this->perform_getAccountState(ton::create_block_id(q.id_), static_cast<WorkchainId>(q.account_->workchain_),
                                          q.account_->id_, 0);
          },
          [&](lite_api::liteServer_getAccountStatePrunned& q) {
            this->perform_getAccountState(ton::create_block_id(q.id_), static_cast<WorkchainId>(q.account_->workchain_),
                                          q.account_->id_, 0x40000000);
          },
          [&](lite_api::liteServer_getOneTransaction& q) {
            this->perform_getOneTransaction(ton::create_block_id(q.id_),
                                            static_cast<WorkchainId>(q.account_->workchain_), q.account_->id_,
                                            static_cast<LogicalTime>(q.lt_));
          },
          [&](lite_api::liteServer_getTransactions& q) {
            this->perform_getTransactions(static_cast<WorkchainId>(q.account_->workchain_), q.account_->id_,
                                          static_cast<LogicalTime>(q.lt_), q.hash_, static_cast<unsigned>(q.count_));
          },
          [&](lite_api::liteServer_sendMessage& q) { this->perform_sendMessage(std::move(q.body_)); },
          [&](lite_api::liteServer_getShardInfo& q) {
            this->perform_getShardInfo(ton::create_block_id(q.id_),
                                       ShardIdFull{q.workchain_, static_cast<ShardId>(q.shard_)}, q.exact_);
          },
          [&](lite_api::liteServer_getAllShardsInfo& q) {
            this->perform_getAllShardsInfo(ton::create_block_id(q.id_));
          },
          [&](lite_api::liteServer_lookupBlock& q) {
            this->perform_lookupBlock(ton::create_block_id_simple(q.id_), q.mode_, q.lt_, q.utime_);
          },
          [&](lite_api::liteServer_lookupBlockWithProof& q) {
            this->perform_lookupBlockWithProof(ton::create_block_id_simple(q.id_), ton::create_block_id(q.mc_block_id_), q.mode_, q.lt_, q.utime_);
          },
          [&](lite_api::liteServer_listBlockTransactions& q) {
            this->perform_listBlockTransactions(ton::create_block_id(q.id_), q.mode_, q.count_,
                                                (q.mode_ & 128) ? q.after_->account_ : td::Bits256::zero(),
                                                static_cast<LogicalTime>((q.mode_ & 128) ? (q.after_->lt_) : 0));
          },
          [&](lite_api::liteServer_listBlockTransactionsExt& q) {
            this->perform_listBlockTransactionsExt(ton::create_block_id(q.id_), q.mode_, q.count_,
                                                (q.mode_ & 128) ? q.after_->account_ : td::Bits256::zero(),
                                                static_cast<LogicalTime>((q.mode_ & 128) ? (q.after_->lt_) : 0));
          },
          [&](lite_api::liteServer_getConfigParams& q) {
            this->perform_getConfigParams(ton::create_block_id(q.id_), (q.mode_ & 0xffff) | 0x10000, q.param_list_);
          },
          [&](lite_api::liteServer_getConfigAll& q) {
            this->perform_getConfigParams(ton::create_block_id(q.id_), (q.mode_ & 0xffff) | 0x20000);
          },
          [&](lite_api::liteServer_getBlockProof& q) {
            this->perform_getBlockProof(ton::create_block_id(q.known_block_),
                                        q.mode_ & 1 ? ton::create_block_id(q.target_block_) : ton::BlockIdExt{},
                                        q.mode_);
          },
          [&](lite_api::liteServer_getValidatorStats& q) {
            this->perform_getValidatorStats(ton::create_block_id(q.id_), q.mode_, q.limit_,
                                            q.mode_ & 1 ? q.start_after_ : td::Bits256::zero(),
                                            q.mode_ & 4 ? q.modified_after_ : 0);
          },
          [&](lite_api::liteServer_runSmcMethod& q) {
            this->perform_runSmcMethod(ton::create_block_id(q.id_), static_cast<WorkchainId>(q.account_->workchain_),
                                       q.account_->id_, q.mode_, q.method_id_, std::move(q.params_));
          },
          [&](lite_api::liteServer_getLibraries& q) {
            this->perform_getLibraries(q.library_list_);
          },
          [&](lite_api::liteServer_getLibrariesWithProof& q) {
            this->perform_getLibrariesWithProof(ton::create_block_id(q.id_), q.mode_, q.library_list_);
          },
          [&](lite_api::liteServer_getShardBlockProof& q) {
            this->perform_getShardBlockProof(create_block_id(q.id_));
          },
          [&](lite_api::liteServer_nonfinal_getCandidate& q) {
            this->perform_nonfinal_getCandidate(q.id_->creator_, create_block_id(q.id_->block_id_),
                                                q.id_->collated_data_hash_);
          },
          [&](lite_api::liteServer_nonfinal_getValidatorGroups& q) {
            this->perform_nonfinal_getValidatorGroups(q.mode_, ShardIdFull{q.wc_, (ShardId)q.shard_});
          },
          [&](lite_api::liteServer_getOutMsgQueueSizes& q) {
            this->perform_getOutMsgQueueSizes(q.mode_ & 1 ? ShardIdFull(q.wc_, q.shard_) : td::optional<ShardIdFull>());
          },
          [&](lite_api::liteServer_getBlockOutMsgQueueSize& q) {
            this->perform_getBlockOutMsgQueueSize(q.mode_, create_block_id(q.id_));
          },
          [&](lite_api::liteServer_getDispatchQueueInfo& q) {
            this->perform_getDispatchQueueInfo(q.mode_, create_block_id(q.id_), q.after_addr_, q.max_accounts_);
          },
          [&](lite_api::liteServer_getDispatchQueueMessages& q) {
            this->perform_getDispatchQueueMessages(q.mode_, create_block_id(q.id_), q.addr_,
                                                   std::max<td::int64>(q.after_lt_, 0), q.max_messages_);
          },
          [&](auto& obj) { this->abort_query(td::Status::Error(ErrorCode::protoviolation, "unknown query")); }));
}

void LiteQuery::perform_getTime() {
  LOG(INFO) << "started a getTime() liteserver query";
  td::int32 now = static_cast<td::int32>(std::time(nullptr));
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_currentTime>(now);
  finish_query(std::move(b));
}

void LiteQuery::perform_getVersion() {
  LOG(INFO) << "started a getVersion() liteserver query";
  td::int32 now = static_cast<td::int32>(std::time(nullptr));
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_version>(0, ls_version, ls_capabilities, now);
  finish_query(std::move(b));
}

void LiteQuery::perform_getMasterchainInfo(int mode) {
  LOG(INFO) << "started a getMasterchainInfo(" << mode << ") liteserver query";
  if (mode > 0) {
    fatal_error("unsupported getMasterchainInfo mode");
    return;
  }
  td::actor::send_closure_later(
      manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
      [Self = actor_id(this), return_state = bool(acc_state_promise_), mode](td::Result<std::pair<Ref<ton::validator::MasterchainState>, BlockIdExt>> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          auto pair = res.move_as_ok();
          auto func = return_state ? &LiteQuery::gotMasterchainInfoForAccountState : &LiteQuery::continue_getMasterchainInfo;
          td::actor::send_closure_later(Self, func, std::move(pair.first),
                                        pair.second, mode);
        }
      });
}

void LiteQuery::gotMasterchainInfoForAccountState(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid,
                                            int mode) {
  perform_getAccountState(blkid, acc_workchain_, acc_addr_, 0x80000000);
}

void LiteQuery::continue_getMasterchainInfo(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid,
                                            int mode) {
  LOG(INFO) << "obtained data for getMasterchainInfo() : last block = " << blkid.to_str();
  auto mc_state_q = Ref<ton::validator::MasterchainStateQ>(std::move(mc_state));
  if (mc_state_q.is_null()) {
    fatal_error("cannot obtain a valid masterchain state");
    return;
  }
  auto zerostate_id = mc_state_q->get_zerostate_id();
  auto zs_tl = create_tl_object<lite_api::tonNode_zeroStateIdExt>(zerostate_id.workchain, zerostate_id.root_hash,
                                                                  zerostate_id.file_hash);
  td::int32 now = static_cast<td::int32>(std::time(nullptr));
  auto b = (mode == -1) ? ton::create_serialize_tl_object<ton::lite_api::liteServer_masterchainInfo>(
                              ton::create_tl_lite_block_id(blkid), mc_state_q->root_hash(), std::move(zs_tl))
                        : ton::create_serialize_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(
                              mode, ls_version, ls_capabilities, ton::create_tl_lite_block_id(blkid),
                              mc_state_q->get_unix_time(), now, mc_state_q->root_hash(), std::move(zs_tl));
  finish_query(std::move(b));
}

void LiteQuery::perform_getBlock(BlockIdExt blkid) {
  LOG(INFO) << "started a getBlock(" << blkid.to_str() << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_for_litequery, blkid,
                          [Self = actor_id(this), blkid](td::Result<Ref<ton::validator::BlockData>> res) {
                            if (res.is_error()) {
                              td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                            } else {
                              td::actor::send_closure_later(Self, &LiteQuery::continue_getBlock, blkid,
                                                            res.move_as_ok());
                            }
                          });
}

void LiteQuery::continue_getBlock(BlockIdExt blkid, Ref<ton::validator::BlockData> block) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ")";
  CHECK(block.not_null());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockData>(ton::create_tl_lite_block_id(blkid),
                                                                                block->data());

  finish_query(std::move(b));
}

void LiteQuery::perform_getBlockHeader(BlockIdExt blkid, int mode) {
  LOG(INFO) << "started a getBlockHeader(" << blkid.to_str() << ", " << mode << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_for_litequery, blkid,
                          [Self = actor_id(this), blkid, mode](td::Result<Ref<ton::validator::BlockData>> res) {
                            if (res.is_error()) {
                              td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                            } else {
                              td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockHeader, blkid, mode,
                                                            res.move_as_ok());
                            }
                          });
}

static bool visit(Ref<vm::Cell> cell);

static bool visit(const vm::CellSlice& cs) {
  auto cnt = cs.size_refs();
  bool res = true;
  for (unsigned i = 0; i < cnt; i++) {
    res &= visit(cs.prefetch_ref(i));
  }
  return res;
}

static bool visit(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return true;
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  return visit(cs);
}

static bool visit(Ref<vm::CellSlice> cs_ref) {
  return cs_ref.is_null() || visit(*cs_ref);
}

void LiteQuery::continue_getBlockHeader(BlockIdExt blkid, int mode, Ref<ton::validator::BlockData> block) {
  LOG(INFO) << "obtained data for getBlockHeader(" << blkid.to_str() << ", " << mode << ")";
  CHECK(block.not_null());
  CHECK(block->block_id() == blkid);
  auto block_root = block->root_cell();
  if (block_root.is_null()) {
    fatal_error("block has no valid root cell");
    return;
  }
  // create block header proof
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == blkid.root_hash);
  vm::MerkleProofBuilder mpb{block_root};
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(mpb.root(), blk) && tlb::unpack_cell(blk.info, info))) {
    fatal_error("cannot unpack block header");
    return;
  }
  if (mode & 1) {
    // with state_update
    vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
    if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
          && upd_cs.size_ext() == 0x20228)) {
      fatal_error("invalid Merkle update in block");
      return;
    }
  }
  visit(info.master_ref);
  visit(info.prev_ref);
  visit(info.prev_vert_ref);
  if (mode & 2) {
    // with value flow
    visit(blk.value_flow);
  }
  if (mode & 16) {
    // with extra
    block::gen::BlockExtra::Record extra;
    if (!tlb::unpack_cell(blk.extra, extra)) {
      fatal_error("cannot unpack BlockExtra in block");
      return;
    }
    if (blkid.is_masterchain()) {
      auto mc_extra_root = extra.custom->prefetch_ref();
      block::gen::McBlockExtra::Record mc_extra;
      if (!(mc_extra_root.not_null() && tlb::unpack_cell(std::move(mc_extra_root), mc_extra))) {
        fatal_error("cannot unpack McBlockExtra in block");
        return;
      }
      if (mode & 32) {
        // with ShardHashes
        visit(mc_extra.shard_hashes);
      }
      if (mode & 64) {
        // with PrevBlkSignatures
        visit(mc_extra.r1.prev_blk_signatures);
      }
    }
  }
  auto proof_data = mpb.extract_proof_boc();
  if (proof_data.is_error()) {
    fatal_error(proof_data.move_as_error());
    return;
  }
  // send answer
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockHeader>(ton::create_tl_lite_block_id(blkid),
                                                                                  mode, proof_data.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getState(BlockIdExt blkid) {
  LOG(INFO) << "started a getState(" << blkid.to_str() << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  if (blkid.id.seqno > 1000) {
    fatal_error("cannot request total state: possibly too large");
    return;
  }
  if (blkid.id.seqno) {
    td::actor::send_closure(manager_, &ValidatorManager::get_block_state_for_litequery, blkid,
                            [Self = actor_id(this), blkid](td::Result<Ref<ShardState>> res) {
                              if (res.is_error()) {
                                td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                              } else {
                                td::actor::send_closure_later(Self, &LiteQuery::continue_getState, blkid,
                                                              res.move_as_ok());
                              }
                            });
  } else {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_zero_state, blkid,
                                  [Self = actor_id(this), blkid](td::Result<td::BufferSlice> res) {
                                    if (res.is_error()) {
                                      td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                    } else {
                                      td::actor::send_closure_later(Self, &LiteQuery::continue_getZeroState, blkid,
                                                                    res.move_as_ok());
                                    }
                                  });
  }
}

void LiteQuery::continue_getState(BlockIdExt blkid, Ref<ton::validator::ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ")";
  CHECK(state.not_null());
  auto res = state->serialize();
  if (res.is_error()) {
    abort_query(res.move_as_error());
    return;
  }
  auto data = res.move_as_ok();
  FileHash file_hash;
  td::sha256(data, file_hash.as_slice());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockState>(
      ton::create_tl_lite_block_id(blkid), state->root_hash(), file_hash, std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::continue_getZeroState(BlockIdExt blkid, td::BufferSlice state) {
  LOG(INFO) << "obtained data for getZeroState(" << blkid.to_str() << ")";
  CHECK(!state.empty());
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockState>(
      ton::create_tl_lite_block_id(blkid), blkid.root_hash, blkid.file_hash, std::move(state));
  finish_query(std::move(b));
}

void LiteQuery::perform_sendMessage(td::BufferSlice data) {
  LOG(INFO) << "started a sendMessage(<" << data.size() << " bytes>) liteserver query";
  auto copy = data.clone();
  td::actor::send_closure_later(
      manager_, &ValidatorManager::check_external_message, std::move(copy),
      [Self = actor_id(this), data = std::move(data), manager = manager_, cache = cache_,
       cache_key = cache_key_](td::Result<td::Ref<ExtMessage>> res) mutable {
        if (res.is_error()) {
          // Don't cache errors
          td::actor::send_closure(cache, &LiteServerCache::drop_send_message_from_cache, cache_key);
          td::actor::send_closure(Self, &LiteQuery::abort_query,
                                  res.move_as_error_prefix("cannot apply external message to current state : "s));
        } else {
          LOG(INFO) << "sending an external message to validator manager";
          td::actor::send_closure_later(manager, &ValidatorManager::send_external_message, res.move_as_ok());
          auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_sendMsgStatus>(1);
          td::actor::send_closure(Self, &LiteQuery::finish_query, std::move(b), false);
        }
      });
}

void LiteQuery::get_block_handle_checked(BlockIdExt blkid, td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle_for_litequery, blkid, std::move(promise));
}

bool LiteQuery::request_mc_block_data(BlockIdExt blkid) {
  if (!blkid.is_masterchain() || !blkid.is_valid_full()) {
    return fatal_error("reference block must belong to the masterchain");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  base_blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_data_for_litequery, blkid,
      [Self = actor_id(this), blkid](td::Result<Ref<BlockData>> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query,
                                  res.move_as_error_prefix("cannot load block "s + blkid.to_str() + " : "));
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_data, blkid, res.move_as_ok());
        }
      });
  return true;
}

bool LiteQuery::request_mc_proof(BlockIdExt blkid, int mode) {
  if (!blkid.is_masterchain() || !blkid.is_valid_full()) {
    return fatal_error("reference block must belong to the masterchain");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  if (mode) {
    base_blk_id_alt_ = blkid;
  } else {
    base_blk_id_ = blkid;
  }
  ++pending_;
  td::actor::send_closure(
      manager_, &ValidatorManager::get_key_block_proof, blkid,
      [Self = actor_id(this), manager = manager_, blkid, mode](td::Result<td::BufferSlice> R) {
        if (R.is_ok()) {
          auto proof = create_proof(blkid, R.move_as_ok());
          proof.ensure();
          td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_proof, blkid, mode, proof.move_as_ok());
          return;
        }
        td::actor::send_closure_later(
            manager, &ValidatorManager::get_block_proof_from_db_short, blkid,
            [Self, blkid, mode](td::Result<Ref<Proof>> res) {
              if (res.is_error()) {
                td::actor::send_closure(Self, &LiteQuery::abort_query,
                                        res.move_as_error_prefix("cannot load proof for "s + blkid.to_str() + " : "));
              } else {
                td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_proof, blkid, mode, res.move_as_ok());
              }
            });
      });
  return true;
}

bool LiteQuery::request_mc_block_state(BlockIdExt blkid) {
  if (!blkid.is_masterchain() || !blkid.is_valid_full()) {
    return fatal_error("reference block must belong to the masterchain");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  base_blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_state_for_litequery, blkid,
      [Self = actor_id(this), blkid](td::Result<Ref<ShardState>> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query,
                                  res.move_as_error_prefix("cannot load state for "s + blkid.to_str() + " : "));
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::got_mc_block_state, blkid, res.move_as_ok());
        }
      });
  return true;
}

bool LiteQuery::request_mc_block_data_state(BlockIdExt blkid) {
  return request_mc_block_data(blkid) && request_mc_block_state(blkid);
}

bool LiteQuery::request_block_data_state(BlockIdExt blkid) {
  LOG(INFO) << "requesting state for block (" << blkid.to_str() << ")";
  return request_block_data(blkid) && request_block_state(blkid);
}

bool LiteQuery::request_block_state(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure(manager_, &ValidatorManager::get_block_state_for_litequery, blkid,
                          [Self = actor_id(this), blkid](td::Result<Ref<ShardState>> res) {
                            if (res.is_error()) {
                              td::actor::send_closure(
                                  Self, &LiteQuery::abort_query,
                                  res.move_as_error_prefix("cannot load state for "s + blkid.to_str() + " : "));
                            } else {
                              td::actor::send_closure_later(Self, &LiteQuery::got_block_state, blkid, res.move_as_ok());
                            }
                          });
  return true;
}

bool LiteQuery::request_block_data(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_for_litequery, blkid,
                          [Self = actor_id(this), blkid](td::Result<Ref<BlockData>> res) {
                            if (res.is_error()) {
                              td::actor::send_closure(
                                  Self, &LiteQuery::abort_query,
                                  res.move_as_error_prefix("cannot load block "s + blkid.to_str() + " : "));
                            } else {
                              td::actor::send_closure_later(Self, &LiteQuery::got_block_data, blkid, res.move_as_ok());
                            }
                          });
  return true;
}

bool LiteQuery::request_proof_link(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  if (blkid.is_masterchain()) {
    td::actor::send_closure(
        manager_, &ValidatorManager::get_key_block_proof_link, blkid,
        [Self = actor_id(this), manager = manager_, blkid](td::Result<td::BufferSlice> R) {
          if (R.is_ok()) {
            auto proof = create_proof(blkid, R.move_as_ok());
            proof.ensure();
            td::actor::send_closure_later(Self, &LiteQuery::got_block_proof_link, blkid, proof.move_as_ok());
            return;
          }
          td::actor::send_closure_later(
              manager, &ValidatorManager::get_block_proof_link_from_db_short, blkid,
              [Self, blkid](td::Result<Ref<ProofLink>> res) {
                if (res.is_error()) {
                  td::actor::send_closure(
                      Self, &LiteQuery::abort_query,
                      res.move_as_error_prefix("cannot load proof link for "s + blkid.to_str() + " : "));
                } else {
                  td::actor::send_closure_later(Self, &LiteQuery::got_block_proof_link, blkid, res.move_as_ok());
                }
              });
        });
  } else {
    get_block_handle_checked(blkid, [=, manager = manager_, Self = actor_id(this)](td::Result<ConstBlockHandle> R) {
      if (R.is_error()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        return;
      }
      td::actor::send_closure_later(
          manager, &ValidatorManager::get_block_proof_link_from_db, R.move_as_ok(),
          [=](td::Result<Ref<ProofLink>> res) {
            if (res.is_error()) {
              td::actor::send_closure(
                  Self, &LiteQuery::abort_query,
                  res.move_as_error_prefix("cannot load proof link for "s + blkid.to_str() + " : "));
            } else {
              td::actor::send_closure_later(Self, &LiteQuery::got_block_proof_link, blkid, res.move_as_ok());
            }
          });
    });
  }
  return true;
}

bool LiteQuery::request_zero_state(BlockIdExt blkid) {
  if (!blkid.is_valid_full()) {
    return fatal_error("invalid block id requested");
  }
  if (blkid.seqno()) {
    return fatal_error("invalid zerostate requested");
  }
  if (!cont_set_) {
    return fatal_error("continuation not set");
  }
  blk_id_ = blkid;
  ++pending_;
  get_block_handle_checked(blkid, [=, manager = manager_, Self = actor_id(this)](td::Result<ConstBlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
      return;
    }
    td::actor::send_closure_later(
        manager, &ValidatorManager::get_zero_state, blkid,
        [=](td::Result<td::BufferSlice> res) {
          if (res.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query,
                                    res.move_as_error_prefix("cannot load zerostate of "s + blkid.to_str() + " : "));
          } else {
            td::actor::send_closure_later(Self, &LiteQuery::got_zero_state, blkid, res.move_as_ok());
          }
        });
  });
  return true;
}

void LiteQuery::perform_getAccountState(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, int mode) {
  LOG(INFO) << "started a getAccountState(" << blkid.to_str() << ", " << workchain << ", " << addr.to_hex() << ", "
            << mode << ") liteserver query";
  if (blkid.id.workchain != masterchainId && blkid.id.workchain != workchain) {
    fatal_error("reference block for a getAccountState() must belong to the masterchain");
    return;
  }
  //if (workchain != masterchainId && workchain != basechainId) {
  //  fatal_error("cannot get account states from specified workchain");
  //  return;
  //}
  if (!blkid.is_valid()) {
    fatal_error("reference block id for a getAccountState() is invalid");
    return;
  }
  if (workchain == blkid.id.workchain &&
      !ton::shard_contains(blkid.shard_full(), extract_addr_prefix(workchain, addr))) {
    fatal_error("requested account id is not contained in the shard of the reference block");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  mode_ = mode;
  if (blkid.id.workchain != masterchainId) {
    base_blk_id_ = blkid;
    set_continuation([&]() -> void { finish_getAccountState({}); });
    request_block_data_state(blkid);
  } else if (blkid.id.seqno != ~0U) {
    set_continuation([&]() -> void { continue_getAccountState(); });
    request_mc_block_data_state(blkid);
  } else {
    LOG(INFO) << "sending a get_last_liteserver_state_block query to manager";
    td::actor::send_closure_later(
        manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
        [Self = actor_id(this)](td::Result<std::pair<Ref<ton::validator::MasterchainState>, BlockIdExt>> res) -> void {
          if (res.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          } else {
            auto pair = res.move_as_ok();
            td::actor::send_closure_later(Self, &LiteQuery::continue_getAccountState_0, std::move(pair.first),
                                          pair.second);
          }
        });
  }
}

void LiteQuery::continue_getAccountState_0(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid) {
  LOG(INFO) << "obtained last masterchain block = " << blkid.to_str();
  base_blk_id_ = blkid;
  CHECK(mc_state.not_null());
  mc_state_ = Ref<MasterchainStateQ>(std::move(mc_state));
  CHECK(mc_state_.not_null());
  set_continuation([&]() -> void { continue_getAccountState(); });
  request_mc_block_data(blkid);
}

void LiteQuery::perform_fetchAccountState() {
  perform_getMasterchainInfo(-1);
}

void LiteQuery::perform_runSmcMethod(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, int mode,
                                     td::int64 method_id, td::BufferSlice params) {
  LOG(INFO) << "started a runSmcMethod(" << blkid.to_str() << ", " << workchain << ", " << addr.to_hex() << ", "
            << method_id << ", " << mode << ") liteserver query with " << params.size() << " parameter bytes";
  if (params.size() >= 65536) {
    fatal_error("more than 64k parameter bytes passed");
    return;
  }
  if (mode & ~0x3f) {
    fatal_error("unsupported mode in runSmcMethod");
    return;
  }
  stack_.clear();
  try {
    if (params.size()) {
      auto res = vm::std_boc_deserialize(std::move(params));
      if (res.is_error()) {
        fatal_error("cannot deserialize parameter list boc: "s + res.move_as_error().to_string());
        return;
      }
      vm::FakeVmStateLimits fstate(1000);  // limit recursive (de)serialization calls
      vm::VmStateInterface::Guard guard(&fstate);
      auto cs = vm::load_cell_slice(res.move_as_ok());
      if (!(vm::Stack::deserialize_to(cs, stack_, 2 /* no continuations */) && cs.empty_ext())) {
        fatal_error("parameter list boc cannot be deserialized as a VmStack");
        return;
      }
    } else {
      stack_ = td::make_ref<vm::Stack>();
    }
    stack_.write().push_smallint(method_id);
  } catch (vm::VmError& vme) {
    fatal_error("error deserializing parameter list: "s + vme.get_msg());
    return;
  } catch (vm::VmVirtError& vme) {
    fatal_error("virtualization error while deserializing parameter list: "s + vme.get_msg());
    return;
  }
  perform_getAccountState(blkid, workchain, addr, mode | 0x10000);
}

void LiteQuery::perform_getLibraries(std::vector<td::Bits256> library_list) {
  LOG(INFO) << "started a getLibraries(<list of " << library_list.size() << " parameters>) liteserver query";
  if (library_list.size() > 16) {
    LOG(INFO) << "too many libraries requested, returning only first 16";
    library_list.resize(16);
  }
  sort( library_list.begin(), library_list.end() );
  library_list.erase( unique( library_list.begin(), library_list.end() ), library_list.end() );
  td::actor::send_closure_later(
      manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
      [Self = actor_id(this), library_list](td::Result<std::pair<Ref<ton::validator::MasterchainState>, BlockIdExt>> res) -> void {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          auto pair = res.move_as_ok();
          td::actor::send_closure_later(Self, &LiteQuery::continue_getLibraries, std::move(pair.first),
                                        pair.second, library_list);
        }
      });
}

void LiteQuery::continue_getLibraries(Ref<ton::validator::MasterchainState> mc_state, BlockIdExt blkid, std::vector<td::Bits256> library_list) {
  LOG(INFO) << "obtained last masterchain block = " << blkid.to_str();
  base_blk_id_ = blkid;
  CHECK(mc_state.not_null());
  mc_state_ = Ref<MasterchainStateQ>(std::move(mc_state));
  CHECK(mc_state_.not_null());

  auto rconfig = block::ConfigInfo::extract_config(mc_state_->root_cell(), block::ConfigInfo::needLibraries);
  if (rconfig.is_error()) {
    fatal_error("cannot extract library list block configuration from masterchain state");
    return;
  }
  auto config = rconfig.move_as_ok();

  if (false) {
    std::ostringstream os;
    vm::load_cell_slice(config->get_libraries_root()).print_rec(os);
    LOG(INFO) << "\n" << os.str();

    auto lib_dict = std::make_unique<vm::Dictionary>(config->get_libraries_root(), 256);
    for (auto k: *lib_dict) {
      std::ostringstream oss;
      k.second->print_rec(oss);
      LOG(INFO) << "library " << k.first.to_hex(256) << ": \n" << oss.str();
    }
  }

  std::vector<ton::tl_object_ptr<ton::lite_api::liteServer_libraryEntry>> a;
  for (const auto& hash : library_list) {
    LOG(INFO) << "looking for library " << hash.to_hex();
    auto libres = config->lookup_library(hash);
    if (libres.is_null()) {
      LOG(INFO) << "library lookup result is null";
      continue;
    }
    auto data = vm::std_boc_serialize(libres);
    if (data.is_error()) {
      LOG(WARNING) << "library serialization failed: " << data.move_as_error().to_string();
      continue;
    }
    a.push_back(ton::create_tl_object<ton::lite_api::liteServer_libraryEntry>(hash, data.move_as_ok()));
  }
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_libraryResult>(std::move(a));
  finish_query(std::move(b));
}

void LiteQuery::perform_getLibrariesWithProof(BlockIdExt blkid, int mode, std::vector<td::Bits256> library_list) {
  LOG(INFO) << "started a getLibrariesWithProof(<list of " << library_list.size() << " parameters>) liteserver query";
  if (library_list.size() > 16) {
    LOG(INFO) << "too many libraries requested, returning only first 16";
    library_list.resize(16);
  }
  sort( library_list.begin(), library_list.end() );
  library_list.erase( unique( library_list.begin(), library_list.end() ), library_list.end() );

  set_continuation([this, library_list, mode]() -> void { continue_getLibrariesWithProof(library_list, mode); });
  request_mc_block_data_state(blkid);
}

void LiteQuery::continue_getLibrariesWithProof(std::vector<td::Bits256> library_list, int mode) {
  LOG(INFO) << "obtained masterchain block = " << base_blk_id_.to_str();
  CHECK(mc_state_.not_null());

  Ref<vm::Cell> state_proof, data_proof;
  if (!make_mc_state_root_proof(state_proof)) {
    return;
  }

  vm::MerkleProofBuilder pb{mc_state_->root_cell()};
  block::gen::ShardStateUnsplit::Record state;
  if (!tlb::unpack_cell(pb.root(), state)) {
    fatal_error("cannot unpack header of shardchain state "s + base_blk_id_.to_str());
  }
  auto libraries_dict = vm::Dictionary(state.r1.libraries->prefetch_ref(), 256);

  std::vector<ton::tl_object_ptr<ton::lite_api::liteServer_libraryEntry>> result;
  std::vector<td::Bits256> result_hashes;
  for (const auto& hash : library_list) {
    LOG(INFO) << "looking for library " << hash.to_hex();

    auto csr = libraries_dict.lookup(hash.bits(), 256);
    if (csr.is_null() || csr->prefetch_ulong(2) != 0 || !csr->have_refs()) {  // shared_lib_descr$00 lib:^Cell
      continue;
    }
    block::gen::LibDescr::Record libdescr;
    if (!tlb::csr_unpack(csr, libdescr)) {
      fatal_error("cannot unpack LibDescr record "s + hash.to_hex());
      return;
    }
    if (mode & 1) {
      // include first 16 publishers in the proof
      auto publishers_dict = vm::Dictionary{vm::DictNonEmpty(), libdescr.publishers, 256};
      auto iter = publishers_dict.begin();
      constexpr int max_publishers = 15; // set to 15 because publishers_dict.begin() counts as the first visit
      for (int i = 0; i < max_publishers && iter != publishers_dict.end(); ++i, ++iter) {}
    }

    result_hashes.push_back(hash);
  }

  auto data_proof_boc = pb.extract_proof_boc();
  if (data_proof_boc.is_error()) {
    fatal_error(data_proof_boc.move_as_error());
    return;
  }
  auto state_proof_boc = vm::std_boc_serialize(std::move(state_proof));
  if (state_proof_boc.is_error()) {
    fatal_error(state_proof_boc.move_as_error());
    return;
  }

  for (const auto& hash : result_hashes) {
    auto csr = libraries_dict.lookup(hash.bits(), 256);
    block::gen::LibDescr::Record libdescr;
    if (!tlb::csr_unpack(csr, libdescr)) {
      fatal_error("cannot unpack LibDescr record "s + hash.to_hex());
      return;
    }
    if (!libdescr.lib->get_hash().bits().equals(hash.bits(), 256)) {
      LOG(ERROR) << "public library hash mismatch: expected " << hash.to_hex() << " , found "
                << libdescr.lib->get_hash().to_hex();
      continue;
    }
    td::BufferSlice libdata;
    if (!(mode & 2)) {
      auto data = vm::std_boc_serialize(libdescr.lib);
      if (data.is_error()) {
        LOG(WARNING) << "library serialization failed: " << data.move_as_error().to_string();
        continue;
      }
      libdata = data.move_as_ok();
    }
    result.push_back(ton::create_tl_object<ton::lite_api::liteServer_libraryEntry>(hash, std::move(libdata)));
  }

  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_libraryResultWithProof>(ton::create_tl_lite_block_id(base_blk_id_), mode, std::move(result), 
                    state_proof_boc.move_as_ok(), data_proof_boc.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getOneTransaction(BlockIdExt blkid, WorkchainId workchain, StdSmcAddress addr, LogicalTime lt) {
  LOG(INFO) << "started a getOneTransaction(" << blkid.to_str() << ", " << workchain << ", " << addr.to_hex() << ","
            << lt << ") liteserver query";
  if (!blkid.is_valid_full()) {
    fatal_error("block id in getOneTransaction() is invalid");
    return;
  }
  if (!ton::shard_contains(blkid.shard_full(), extract_addr_prefix(workchain, addr))) {
    fatal_error("requested account id is not contained in the shard of the specified block");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  trans_lt_ = lt;
  set_continuation([&]() -> void { continue_getOneTransaction(); });
  request_block_data(blkid);
}

void LiteQuery::got_block_state(BlockIdExt blkid, Ref<ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(state.not_null());
  state_ = Ref<ShardStateQ>(std::move(state));
  CHECK(state_.not_null());
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::got_mc_block_state(BlockIdExt blkid, Ref<ShardState> state) {
  LOG(INFO) << "obtained data for getState(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(state.not_null());
  mc_state_ = Ref<MasterchainStateQ>(std::move(state));
  CHECK(mc_state_.not_null());
  CHECK(blkid == base_blk_id_);
  dec_pending();
}

void LiteQuery::got_block_data(BlockIdExt blkid, Ref<BlockData> data) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(data.not_null());
  block_ = Ref<BlockQ>(std::move(data));
  CHECK(block_.not_null());
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::got_mc_block_data(BlockIdExt blkid, Ref<BlockData> data) {
  LOG(INFO) << "obtained data for getBlock(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(data.not_null());
  mc_block_ = Ref<BlockQ>(std::move(data));
  CHECK(mc_block_.not_null());
  CHECK(blkid == base_blk_id_);
  dec_pending();
}

void LiteQuery::got_mc_block_proof(BlockIdExt blkid, int mode, Ref<Proof> proof) {
  LOG(INFO) << "obtained data for getBlockProof(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(proof.not_null());
  if (mode) {
    mc_proof_alt_ = Ref<ProofQ>(std::move(proof));
    CHECK(mc_proof_alt_.not_null());
    CHECK(blkid == base_blk_id_alt_);
  } else {
    mc_proof_ = Ref<ProofQ>(std::move(proof));
    CHECK(mc_proof_.not_null());
    CHECK(blkid == base_blk_id_);
  }
  dec_pending();
}

void LiteQuery::got_block_proof_link(BlockIdExt blkid, Ref<ProofLink> proof_link) {
  LOG(INFO) << "obtained data for getBlockProofLink(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(proof_link.not_null());
  proof_link_ = Ref<ProofLinkQ>(std::move(proof_link));
  CHECK(proof_link_.not_null());
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::got_zero_state(BlockIdExt blkid, td::BufferSlice zerostate) {
  LOG(INFO) << "obtained data for getZeroState(" << blkid.to_str() << ") needed by a liteserver query";
  CHECK(!zerostate.empty());
  buffer_ = std::move(zerostate);
  CHECK(blkid == blk_id_);
  dec_pending();
}

void LiteQuery::check_pending() {
  CHECK(pending_ >= 0);
  if (!pending_) {
    if (!cont_set_) {
      fatal_error("no continuation set for completion of data loading process");
    } else {
      cont_set_ = false;
      std::move(continuation_)();
    }
  }
}

bool LiteQuery::set_continuation(std::function<void()>&& cont) {
  if (cont_set_) {
    return fatal_error("continuation already set");
  } else {
    continuation_ = std::move(cont);
    return cont_set_ = true;
  }
}

bool LiteQuery::make_mc_state_root_proof(Ref<vm::Cell>& proof) {
  return make_state_root_proof(proof, mc_state_, mc_block_, base_blk_id_);
}

bool LiteQuery::make_state_root_proof(Ref<vm::Cell>& proof) {
  return make_state_root_proof(proof, state_, block_, blk_id_);
}

bool LiteQuery::make_state_root_proof(Ref<vm::Cell>& proof, Ref<ShardStateQ> state, Ref<BlockData> block,
                                      const BlockIdExt& blkid) {
  CHECK(block.not_null() && state.not_null());
  return make_state_root_proof(proof, state->root_cell(), block->root_cell(), blkid);
}

bool LiteQuery::make_state_root_proof(Ref<vm::Cell>& proof, Ref<vm::Cell> state_root, Ref<vm::Cell> block_root,
                                      const BlockIdExt& blkid) {
  CHECK(block_root.not_null() && state_root.not_null());
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == blkid.root_hash);
  vm::MerkleProofBuilder pb{std::move(block_root)};
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(pb.root(), blk) && tlb::unpack_cell(blk.info, info) &&
        block::gen::BlkPrevInfo(info.after_merge).validate_ref(info.prev_ref))) {
    return fatal_error("cannot unpack block header");
  }
  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  auto upd_hash = upd_cs.prefetch_ref(1)->get_hash(0);
  auto state_hash = state_root->get_hash();
  if (upd_hash != state_hash) {
    return fatal_error("cannot construct Merkle proof for given masterchain state because of hash mismatch");
  }
  if (!pb.extract_proof_to(proof)) {
    return fatal_error("unknown error creating Merkle proof");
  }
  return true;
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, ShardIdFull shard,
                                      ShardIdFull& true_shard, Ref<vm::Cell>& leaf, bool& found, bool exact) {
  vm::MerkleProofBuilder pb{mc_state_->root_cell()};
  block::gen::ShardStateUnsplit::Record sstate;
  if (!(tlb::unpack_cell(pb.root(), sstate))) {
    return fatal_error("cannot unpack state header");
  }
  auto shards_dict = block::ShardConfig::extract_shard_hashes_dict(pb.root());
  if (!shards_dict) {
    return fatal_error("cannot extract ShardHashes from last mc state");
  }
  vm::CellSlice cs;
  found = block::ShardConfig::get_shard_hash_raw_from(*shards_dict, cs, shard, true_shard, exact, &leaf);
  if (found) {
    info = block::McShardHash::unpack(cs, true_shard);
    if (info.is_null()) {
      return fatal_error("cannot unpack a leaf entry from ShardHashes");
    }
  } else {
    info.clear();
  }
  if (!pb.extract_proof_to(proof)) {
    return fatal_error("unknown error creating Merkle proof");
  }
  return true;
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, ShardIdFull shard,
                                      bool exact) {
  Ref<vm::Cell> leaf;
  ShardIdFull true_shard;
  bool found;
  return make_shard_info_proof(proof, info, shard, true_shard, leaf, found, exact);
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, Ref<block::McShardHash>& info, AccountIdPrefixFull prefix) {
  return make_shard_info_proof(proof, info, prefix.as_leaf_shard(), false);
}

bool LiteQuery::make_shard_info_proof(Ref<vm::Cell>& proof, BlockIdExt& blkid, AccountIdPrefixFull prefix) {
  Ref<block::McShardHash> info;
  if (!make_shard_info_proof(proof, info, prefix)) {
    return false;
  }
  if (info.not_null()) {
    blkid = info->top_block_id();
  } else {
    blkid.invalidate();
  }
  return true;
}

bool LiteQuery::make_ancestor_block_proof(Ref<vm::Cell>& proof, Ref<vm::Cell> state_root, const BlockIdExt& old_blkid) {
  vm::MerkleProofBuilder mpb{std::move(state_root)};
  auto rconfig = block::ConfigInfo::extract_config(mpb.root(), block::ConfigInfo::needPrevBlocks);
  if (rconfig.is_error()) {
    return fatal_error(
        "cannot extract previous block configuration from masterchain state while constructing Merkle proof for "s +
        old_blkid.to_str());
  }
  if (!rconfig.move_as_ok()->check_old_mc_block_id(old_blkid, true)) {
    return fatal_error("cannot check that "s + old_blkid.to_str() +
                       " is indeed a previous masterchain block while constructing Merkle proof");
  }
  if (!mpb.extract_proof_to(proof)) {
    return fatal_error("error while constructing Merkle proof for old masterchain block "s + old_blkid.to_str());
  }
  return true;
}

void LiteQuery::continue_getAccountState() {
  LOG(INFO) << "continue getAccountState() query";
  if (acc_workchain_ == masterchainId) {
    blk_id_ = base_blk_id_;
    block_ = mc_block_;
    state_ = mc_state_;
    finish_getAccountState({});
    return;
  }
  Ref<vm::Cell> proof3, proof4;
  ton::BlockIdExt blkid;
  if (!(make_mc_state_root_proof(proof3) &&
        make_shard_info_proof(proof4, blkid, extract_addr_prefix(acc_workchain_, acc_addr_)))) {
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof3), std::move(proof4)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  if (!blkid.is_valid()) {
    // no shard with requested address found
    LOG(INFO) << "getAccountState(" << acc_workchain_ << ":" << acc_addr_.to_hex()
              << ") query completed (unknown workchain/shard)";
    auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_accountState>(
        ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blkid), proof.move_as_ok(),
        td::BufferSlice{}, td::BufferSlice{});
    finish_query(std::move(b));
  } else {
    shard_proof_ = proof.move_as_ok();
    set_continuation([this]() -> void { finish_getAccountState(std::move(shard_proof_)); });
    request_block_data_state(blkid);
  }
}

void LiteQuery::finish_getAccountState(td::BufferSlice shard_proof) {
  LOG(INFO) << "completing getAccountState() query";
  Ref<vm::Cell> proof1, proof2;
  if (!make_state_root_proof(proof1)) {
    return;
  }
  vm::MerkleProofBuilder pb{state_->root_cell()};
  block::gen::ShardStateUnsplit::Record sstate;
  if (!tlb::unpack_cell(pb.root(), sstate)) {
    fatal_error("cannot unpack state header");
    return;
  }
  vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(sstate.accounts), 256, block::tlb::aug_ShardAccounts};
  auto acc_csr = accounts_dict.lookup(acc_addr_);
  if (mode_ & 0x80000000) {
    auto config = block::ConfigInfo::extract_config(mc_state_->root_cell(), 0xFFFF);
    if (config.is_error()) {
      fatal_error(config.move_as_error());
      return;
    }
    auto rconfig = config.move_as_ok();
    rconfig->set_block_id_ext(mc_state_->get_block_id());
    acc_state_promise_.set_value(std::make_tuple(
                                  std::move(acc_csr), sstate.gen_utime, sstate.gen_lt, std::move(rconfig)
                                 ));
    return;
  }

  Ref<vm::Cell> acc_root;
  if (acc_csr.not_null()) {
    acc_root = acc_csr->prefetch_ref();
  }
  if (!pb.extract_proof_to(proof2)) {
    fatal_error("unknown error creating Merkle proof");
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
  pb.clear();
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  if (mode_ & 0x10000) {
    if (!mc_state_.is_null()) {
      finish_runSmcMethod(std::move(shard_proof), proof.move_as_ok(), std::move(acc_root), sstate.gen_utime,
                          sstate.gen_lt);
      return;
    }
    shard_proof_ = std::move(shard_proof);
    proof_ = proof.move_as_ok();
    set_continuation(
        [&, base_blk_id = base_blk_id_, acc_root, utime = sstate.gen_utime, lt = sstate.gen_lt]() mutable -> void {
          base_blk_id_ = base_blk_id;  // It gets overridden by request_mc_block_data_state
          finish_runSmcMethod(std::move(shard_proof_), std::move(proof_), std::move(acc_root), utime, lt);
        });
    td::optional<BlockIdExt> master_ref = state_->get_master_ref();
    if (!master_ref) {
      fatal_error("masterchain ref block is not available");
      return;
    }
    request_mc_block_data_state(master_ref.value());
    return;
  }
  td::BufferSlice data;
  if (acc_root.not_null()) {
    if (mode_ & 0x40000000) {
      vm::MerkleProofBuilder mpb{acc_root};
      // This does not include code, data and libs into proof, but it includes extra currencies
      if (!block::gen::t_Account.validate_ref(mpb.root())) {
        fatal_error("failed to validate Account");
        return;
      }
      if (!mpb.extract_proof_to(acc_root)) {
        fatal_error("unknown error creating Merkle proof");
        return;
      }
    }
    auto res = vm::std_boc_serialize(std::move(acc_root));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  }
  LOG(INFO) << "getAccountState(" << acc_workchain_ << ":" << acc_addr_.to_hex() << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_accountState>(
      ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blk_id_), std::move(shard_proof),
      proof.move_as_ok(), std::move(data));
  finish_query(std::move(b));
}

// same as in lite-client/lite-client-common.cpp
static td::Ref<vm::Tuple> prepare_vm_c7(ton::UnixTime now, ton::LogicalTime lt, td::Ref<vm::CellSlice> my_addr,
                                        const block::CurrencyCollection& balance,
                                        const block::ConfigInfo* config = nullptr, td::Ref<vm::Cell> my_code = {},
                                        td::RefInt256 due_payment = td::zero_refint()) {
  td::BitArray<256> rand_seed;
  td::RefInt256 rand_seed_int{true};
  td::Random::secure_bytes(rand_seed.as_slice());
  if (!rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false)) {
    return {};
  }
  std::vector<vm::StackEntry> tuple = {
      td::make_refint(0x076ef1ea),                         // [ magic:0x076ef1ea
      td::make_refint(0),                                  //   actions:Integer
      td::make_refint(0),                                  //   msgs_sent:Integer
      td::make_refint(now),                                //   unixtime:Integer
      td::make_refint(lt),                                 //   block_lt:Integer
      td::make_refint(lt),                                 //   trans_lt:Integer
      std::move(rand_seed_int),                            //   rand_seed:Integer
      balance.as_vm_tuple(),                               //   balance_remaining:[Integer (Maybe Cell)]
      my_addr,                                             //   myself:MsgAddressInt
      config ? config->get_root_cell() : vm::StackEntry()  //   global_config:(Maybe Cell) ] = SmartContractInfo;
  };
  if (config && config->get_global_version() >= 4) {
    tuple.push_back(vm::StackEntry::maybe(my_code));                   // code:Cell
    tuple.push_back(block::CurrencyCollection::zero().as_vm_tuple());  // in_msg_value:[Integer (Maybe Cell)]
    tuple.push_back(td::zero_refint());                                // storage_fees:Integer

    // [ wc:Integer shard:Integer seqno:Integer root_hash:Integer file_hash:Integer] = BlockId;
    // [ last_mc_blocks:[BlockId...]
    //   prev_key_block:BlockId ] : PrevBlocksInfo
    auto info = config->get_prev_blocks_info();
    tuple.push_back(info.is_ok() ? info.move_as_ok() : vm::StackEntry());
  }
  if (config && config->get_global_version() >= 6) {
    tuple.push_back(vm::StackEntry::maybe(config->get_unpacked_config_tuple(now)));  // unpacked_config_tuple:[...]
    tuple.push_back(due_payment);                                                    // due_payment:Integer
    // precompiled_gas_usage:(Maybe Integer)
    td::optional<block::PrecompiledContractsConfig::Contract> precompiled;
    if (my_code.not_null()) {
      precompiled = config->get_precompiled_contracts_config().get_contract(my_code->get_hash().bits());
    }
    tuple.push_back(precompiled ? td::make_refint(precompiled.value().gas_usage) : vm::StackEntry());
  }
  if (config && config->get_global_version() >= 11) {
    tuple.push_back(block::transaction::Transaction::prepare_in_msg_params_tuple(nullptr, {}, {}));
  }
  auto tuple_ref = td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(tuple));
  LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple_ref).to_string();
  return vm::make_tuple_ref(std::move(tuple_ref));
}

void LiteQuery::finish_runSmcMethod(td::BufferSlice shard_proof, td::BufferSlice state_proof, Ref<vm::Cell> acc_root,
                                    UnixTime gen_utime, LogicalTime gen_lt) {
  LOG(INFO) << "completing runSmcMethod() query";
  int mode = mode_ & 0xffff;
  if (acc_root.is_null()) {
    // no such account
    LOG(INFO) << "runSmcMethod(" << acc_workchain_ << ":" << acc_addr_.to_hex()
              << ") query completed: account state is empty";
    auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_runMethodResult>(
        mode, ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blk_id_), std::move(shard_proof),
        std::move(state_proof), td::BufferSlice(), td::BufferSlice(), td::BufferSlice(), -0x100, td::BufferSlice());
    finish_query(std::move(b));
    return;
  }
  vm::MerkleProofBuilder pb{std::move(acc_root)};
  block::gen::Account::Record_account acc;
  block::gen::StorageInfo::Record storage_info;
  block::gen::AccountStorage::Record store;
  block::CurrencyCollection balance;
  block::gen::StateInit::Record state_init;
  if (!(tlb::unpack_cell(pb.root(), acc) && tlb::csr_unpack(std::move(acc.storage), store) &&
        balance.validate_unpack(store.balance) && store.state->prefetch_ulong(1) == 1 &&
        store.state.write().advance(1) && tlb::csr_unpack(std::move(store.state), state_init) &&
        tlb::csr_unpack(std::move(acc.storage_stat), storage_info))) {
    LOG(INFO) << "error unpacking account state, or account is frozen or uninitialized";
    td::Result<td::BufferSlice> proof_boc;
    if (mode & 2) {
      proof_boc = pb.extract_proof_boc();
      if (proof_boc.is_error()) {
        fatal_error(proof_boc.move_as_error());
        return;
      }
    } else {
      proof_boc = td::BufferSlice();
    }
    auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_runMethodResult>(
        mode, ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blk_id_), std::move(shard_proof),
        std::move(state_proof), proof_boc.move_as_ok(), td::BufferSlice(), td::BufferSlice(), -0x100,
        td::BufferSlice());
    finish_query(std::move(b));
    return;
  }
  auto code = state_init.code->prefetch_ref();
  auto data = state_init.data->prefetch_ref();
  auto acc_libs = state_init.library->prefetch_ref();
  long long gas_limit = client_method_gas_limit;
  td::RefInt256 due_payment;
  if (storage_info.due_payment.write().fetch_long(1)) {
    due_payment = block::tlb::t_Grams.as_integer(storage_info.due_payment);
  } else {
    due_payment = td::zero_refint();
  }
  LOG(DEBUG) << "creating VM with gas limit " << gas_limit;
  // **** INIT VM ****
  auto r_config = block::ConfigInfo::extract_config(
      mc_state_->root_cell(),
      block::ConfigInfo::needLibraries | block::ConfigInfo::needCapabilities | block::ConfigInfo::needPrevBlocks);
  if (r_config.is_error()) {
    fatal_error(r_config.move_as_error());
    return;
  }
  auto config = r_config.move_as_ok();
  std::vector<td::Ref<vm::Cell>> libraries;
  if (config->get_libraries_root().not_null()) {
    libraries.push_back(config->get_libraries_root());
  }
  if (acc_libs.not_null()) {
    libraries.push_back(acc_libs);
  }
  vm::GasLimits gas{gas_limit, gas_limit};
  vm::VmState vm{code,
                 config->get_global_version(),
                 std::move(stack_),
                 gas,
                 1,
                 std::move(data),
                 vm::VmLog::Null(),
                 std::move(libraries)};
  auto c7 = prepare_vm_c7(gen_utime, gen_lt, td::make_ref<vm::CellSlice>(acc.addr->clone()), balance, config.get(),
                          std::move(code), due_payment);
  vm.set_c7(c7);  // tuple with SmartContractInfo
  // vm.incr_stack_trace(1);    // enable stack dump after each step
  LOG(INFO) << "starting VM to run GET-method of smart contract " << acc_workchain_ << ":" << acc_addr_.to_hex();
  // **** RUN VM ****
  int exit_code = ~vm.run();
  LOG(DEBUG) << "VM terminated with exit code " << exit_code;
  stack_ = vm.get_stack_ref();
  LOG(INFO) << "runSmcMethod(" << acc_workchain_ << ":" << acc_addr_.to_hex() << ") query completed: exit code is "
            << exit_code;
  vm::FakeVmStateLimits fstate(1000);  // limit recursive (de)serialization calls
  vm::VmStateInterface::Guard guard(&fstate);
  Ref<vm::Cell> cell;
  td::BufferSlice c7_info, result;
  if (mode & 8) {
    // serialize c7
    if (!(mode & 32)) {
      c7 = prepare_vm_c7(gen_utime, gen_lt, td::make_ref<vm::CellSlice>(acc.addr->clone()), balance);
    }
    vm::CellBuilder cb;
    if (!(vm::StackEntry{std::move(c7)}.serialize(cb) && cb.finalize_to(cell))) {
      fatal_error("cannot serialize c7");
      return;
    }
    auto res = vm::std_boc_serialize(std::move(cell));
    if (res.is_error()) {
      fatal_error("cannot serialize c7 : "s + res.move_as_error().to_string());
      return;
    }
    c7_info = res.move_as_ok();
  }
  // pre-serialize stack always (to visit all data cells referred from the result)
  vm::CellBuilder cb;
  if (!(stack_->serialize(cb) && cb.finalize_to(cell))) {
    fatal_error("cannot serialize resulting stack");
    return;
  }
  if (mode & 4) {
    // serialize stack if required
    auto res = vm::std_boc_serialize(std::move(cell));
    if (res.is_error()) {
      fatal_error("cannot serialize resulting stack : "s + res.move_as_error().to_string());
      return;
    }
    result = res.move_as_ok();
  }
  td::Result<td::BufferSlice> proof_boc;
  if (mode & 2) {
    proof_boc = pb.extract_proof_boc();
    if (proof_boc.is_error()) {
      fatal_error(proof_boc.move_as_error());
      return;
    }
  } else {
    proof_boc = td::BufferSlice();
  }
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_runMethodResult>(
      mode, ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(blk_id_), std::move(shard_proof),
      std::move(state_proof), proof_boc.move_as_ok(), std::move(c7_info), td::BufferSlice(), exit_code,
      std::move(result));
  finish_query(std::move(b));
}

void LiteQuery::continue_getOneTransaction() {
  LOG(INFO) << "completing getOneTransaction() query";
  CHECK(block_.not_null());
  vm::MerkleProofBuilder pb{block_->root_cell()};
  auto trans_res = block::get_block_transaction(pb.root(), acc_workchain_, acc_addr_, trans_lt_);
  if (trans_res.is_error()) {
    fatal_error(trans_res.move_as_error());
    return;
  }
  auto trans_root = trans_res.move_as_ok();
  auto proof_boc = pb.extract_proof_boc();
  if (proof_boc.is_error()) {
    fatal_error(proof_boc.move_as_error());
    return;
  }
  td::BufferSlice data;
  if (trans_root.not_null()) {
    auto res = vm::std_boc_serialize(std::move(trans_root));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  }
  LOG(INFO) << "getOneTransaction(" << acc_workchain_ << ":" << acc_addr_.to_hex() << "," << trans_lt_
            << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_transactionInfo>(
      ton::create_tl_lite_block_id(blk_id_), proof_boc.move_as_ok(), std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::perform_getTransactions(WorkchainId workchain, StdSmcAddress addr, LogicalTime lt, Bits256 hash,
                                        unsigned count) {
  LOG(INFO) << "started a getTransactions(" << workchain << ", " << addr.to_hex() << ", " << lt << ", " << hash.to_hex()
            << ", " << count << ") liteserver query";
  count = std::min(count, (unsigned)max_transaction_count);
  /*
  if (count > max_transaction_count) {
    fatal_error("cannot fetch more than 10 preceding transactions at one time");
    return;
  }
  */
  if (workchain == ton::workchainInvalid) {
    fatal_error("invalid workchain specified");
    return;
  }
  acc_workchain_ = workchain;
  acc_addr_ = addr;
  trans_lt_ = lt;
  trans_hash_ = hash;
  continue_getTransactions(count, false);
}

void LiteQuery::continue_getTransactions(unsigned remaining, bool exact) {
  LOG(INFO) << "continue getTransactions() : " << remaining << " remaining";
  bool redo = true;
  while (remaining && redo && trans_lt_ && block_.not_null()) {
    redo = false;
    if (!ton::shard_contains(block_->block_id().shard_full(), ton::extract_addr_prefix(acc_workchain_, acc_addr_))) {
      fatal_error("obtained a block that cannot contain specified account");
      return;
    }
    auto res = block::get_block_transaction_try(block_->root_cell(), acc_workchain_, acc_addr_, trans_lt_);
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    auto root = res.move_as_ok();
    if (root.not_null()) {
      // transaction found
      if (trans_hash_ != root->get_hash().bits()) {
        if (!roots_.empty()) {
          LOG(ERROR) << "transaction hash mismatch: prev_trans_lt/hash invalid for " << acc_workchain_ << ":"
                     << acc_addr_.to_hex() << " lt=" << trans_lt_ << " hash=" << trans_hash_.to_hex()
                     << " found hash=" << root->get_hash().bits().to_hex(256);
        }
        fatal_error("transaction hash mismatch");
        return;
      }
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(root, trans)) {
        fatal_error("cannot unpack transaction");
        return;
      }
      if (trans.prev_trans_lt >= trans_lt_) {
        fatal_error("previous transaction time is not less than the current one");
        return;
      }
      roots_.push_back(std::move(root));
      aux_objs_.push_back(block_);
      blk_ids_.push_back(block_->block_id());
      LOG(DEBUG) << "going to previous transaction with lt=" << trans.prev_trans_lt << " from current lt=" << trans_lt_;
      trans_lt_ = trans.prev_trans_lt;
      trans_hash_ = trans.prev_trans_hash;
      redo = (trans_lt_ > 0);
      exact = false;
      --remaining;
      continue;
    } else if (exact) {
      LOG(DEBUG) << "could not find transaction " << trans_lt_ << " of " << acc_workchain_ << ':' << acc_addr_.to_hex()
                 << " in block " << block_->block_id().to_str();
      if (roots_.empty()) {
        fatal_error("cannot locate transaction in block with specified logical time");
        return;
      }
      finish_getTransactions();
      return;
    }
  }
  if (!remaining || !trans_lt_) {
    finish_getTransactions();
    return;
  }
  ++pending_;
  LOG(DEBUG) << "sending get_block_by_lt_from_db() query to manager for " << acc_workchain_ << ":" << acc_addr_.to_hex()
             << " " << trans_lt_;
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_by_lt_for_litequery, ton::extract_addr_prefix(acc_workchain_, acc_addr_),
      trans_lt_, [Self = actor_id(this), remaining, manager = manager_](td::Result<ConstBlockHandle> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_getTransactions, res.move_as_error(), ton::BlockIdExt{});
        } else {
          auto handle = res.move_as_ok();
          LOG(DEBUG) << "requesting data for block " << handle->id().to_str();
          td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db, handle,
                                        [Self, blkid = handle->id(), remaining](td::Result<Ref<BlockData>> res) {
                                          if (res.is_error()) {
                                            td::actor::send_closure(Self, &LiteQuery::abort_getTransactions,
                                                                    res.move_as_error(), blkid);
                                          } else {
                                            td::actor::send_closure_later(Self, &LiteQuery::continue_getTransactions_2,
                                                                          blkid, res.move_as_ok(), remaining);
                                          }
                                        });
        }
      });
}

void LiteQuery::continue_getTransactions_2(BlockIdExt blkid, Ref<BlockData> block, unsigned remaining) {
  LOG(INFO) << "getTransactions() : loaded block " << blkid.to_str();
  --pending_;
  CHECK(!pending_);
  CHECK(block.not_null());
  block_ = Ref<BlockQ>(std::move(block));
  blk_id_ = blkid;
  continue_getTransactions(remaining, true);
}

void LiteQuery::abort_getTransactions(td::Status error, ton::BlockIdExt blkid) {
  LOG(INFO) << "getTransactions() : got error " << error.message() << " from manager";
  if (roots_.empty()) {
    if (blkid.is_valid()) {
      fatal_error(PSTRING() << "cannot load block " << blkid.to_str()
                            << " with specified transaction: " << error.message());
    } else {
      fatal_error(PSTRING() << "cannot compute block with specified transaction: " << error.message());
    }
  } else {
    pending_ = 0;
    finish_getTransactions();
  }
}

void LiteQuery::finish_getTransactions() {
  LOG(INFO) << "completing getTransactions() liteserver query";
  auto res = vm::std_boc_serialize_multi(std::move(roots_));
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  std::vector<ton::tl_object_ptr<ton::lite_api::tonNode_blockIdExt>> a;
  for (const auto& id : blk_ids_) {
    a.push_back(ton::create_tl_lite_block_id(id));
  }
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_transactionList>(std::move(a), res.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getShardInfo(BlockIdExt blkid, ShardIdFull shard, bool exact) {
  LOG(INFO) << "started a getShardInfo(" << blkid.to_str() << ", " << shard.to_str() << ", " << exact
            << ") liteserver query";
  if (!shard.is_valid()) {
    fatal_error("requested shard is invalid");
    return;
  }
  set_continuation([this, shard, exact]() -> void { continue_getShardInfo(shard, exact); });
  request_mc_block_data_state(blkid);
}

void LiteQuery::load_prevKeyBlock(ton::BlockIdExt blkid, td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>> promise) {
  td::actor::send_closure_later(manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
                                [Self = actor_id(this), blkid, promise = std::move(promise)](
                                    td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) mutable {
                                  td::actor::send_closure_later(Self, &LiteQuery::continue_loadPrevKeyBlock, blkid,
                                                                std::move(res), std::move(promise));
                                });
}

void LiteQuery::continue_loadPrevKeyBlock(ton::BlockIdExt blkid,
                                          td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res,
                                          td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>> promise) {
  TRY_RESULT_PROMISE(promise, pair, std::move(res));
  base_blk_id_ = pair.second;
  if (!base_blk_id_.is_masterchain_ext()) {
    promise.set_error(
        td::Status::Error(PSTRING() << "the most recent masterchain block " << base_blk_id_.to_str() << " is invalid"));
    return;
  }
  auto state = Ref<MasterchainStateQ>(std::move(pair.first));
  if (state.is_null()) {
    promise.set_error(
        td::Status::Error(PSLICE() << "obtained no valid masterchain state for block " << base_blk_id_.to_str()));
    return;
  }
  if (blkid.seqno() > base_blk_id_.seqno()) {
    promise.set_error(td::Status::Error(PSLICE()
                                        << "client knows block " << blkid.to_str()
                                        << " newer than the reference masterchain block " << base_blk_id_.to_str()));
    return;
  }
  mc_state0_ = Ref<MasterchainStateQ>(state);
  if (base_blk_id_ != state->get_block_id()) {
    promise.set_error(td::Status::Error(PSLICE() << "the state for " << base_blk_id_.to_str()
                                                 << " is in fact a state for different block "
                                                 << state->get_block_id().to_str()));
    return;
  }
  if (!state->check_old_mc_block_id(blkid)) {
    promise.set_error(td::Status::Error(PSLICE() << "requested masterchain block " << blkid.to_str()
                                                 << " is unknown from the perspective of reference block "
                                                 << base_blk_id_.to_str()));
    return;
  }
  LOG(INFO) << "continuing load_prevKeyBlock(" << blkid.to_str() << ") query with a state for "
            << base_blk_id_.to_str();
  auto key_blk_id = state->prev_key_block_id(blkid.seqno());
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_data_from_db_short, key_blk_id,
      [Self = actor_id(this), key_blk_id, promise = std::move(promise)](td::Result<Ref<BlockData>> res) mutable {
        td::actor::send_closure_later(Self, &LiteQuery::finish_loadPrevKeyBlock, key_blk_id, std::move(res),
                                      std::move(promise));
      });
}

void LiteQuery::finish_loadPrevKeyBlock(ton::BlockIdExt blkid, td::Result<Ref<BlockData>> res,
                                        td::Promise<std::pair<BlockIdExt, Ref<BlockQ>>> promise) {
  TRY_RESULT_PROMISE_PREFIX(promise, data, std::move(res), PSLICE() << "cannot load block " << blkid.to_str() << " : ");
  Ref<BlockQ> data0{std::move(data)};
  if (data0.is_null()) {
    promise.set_error(td::Status::Error("no block data for key block "s + blkid.to_str()));
    return;
  }
  promise.set_result(std::make_pair(blkid, std::move(data0)));
}

void LiteQuery::perform_getConfigParams(BlockIdExt blkid, int mode, std::vector<int> param_list) {
  LOG(INFO) << "started a getConfigParams(" << blkid.to_str() << ", " << mode << ", <list of " << param_list.size()
            << " parameters>) liteserver query";
  if (!blkid.is_masterchain_ext()) {
    fatal_error("configuration parameters can be loaded with respect to a masterchain block only");
    return;
  }
  if (!(mode & 0x8000)) {
    // ordinary case: get configuration from masterchain state
    set_continuation([this, mode, param_list = std::move(param_list)]() mutable {
      continue_getConfigParams(mode, std::move(param_list));
    });
    request_mc_block_data_state(blkid);
  } else {
    // get configuration from previous key block
    load_prevKeyBlock(blkid, [this, mode, param_list = std::move(param_list)](
                                 td::Result<std::pair<BlockIdExt, Ref<BlockQ>>> res) mutable {
      if (res.is_error()) {
        this->abort_query(res.move_as_error());
      } else {
        this->base_blk_id_ = res.ok().first;
        this->mc_block_ = res.move_as_ok().second;
        this->continue_getConfigParams(mode, std::move(param_list));
      }
    });
  }
}

void LiteQuery::continue_getConfigParams(int mode, std::vector<int> param_list) {
  LOG(INFO) << "completing getConfigParams(" << base_blk_id_.to_str() << ", " << mode << ", <list of "
            << param_list.size() << " parameters>) liteserver query";
  bool keyblk = (mode & 0x8000);
  Ref<vm::Cell> proof1, block;
  if (keyblk) {
    block = mc_block_->root_cell();
  } else if (!make_mc_state_root_proof(proof1)) {
    return;
  }

  vm::MerkleProofBuilder mpb{keyblk ? block : mc_state_->root_cell()};
  if (keyblk) {
    auto res = block::check_block_header_proof(mpb.root(), base_blk_id_);
    if (res.is_error()) {
      fatal_error(res.move_as_error_prefix("invalid key block header:"));
      return;
    }
  }

  std::unique_ptr<block::Config> cfg;
  if (keyblk || !(mode & block::ConfigInfo::needPrevBlocks)) {
    auto res = keyblk ? block::Config::extract_from_key_block(mpb.root(), mode)
                      : block::Config::extract_from_state(mpb.root(), mode);
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    cfg = res.move_as_ok();
  } else {
    if (mode & block::ConfigInfo::needPrevBlocks) {
      mode |= block::ConfigInfo::needCapabilities;
    }
    auto res = block::ConfigInfo::extract_config(mpb.root(), mode);
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    cfg = res.move_as_ok();
  }
  if (!cfg) {
    fatal_error("cannot extract configuration from last mc state");
    return;
  }
  try {
    if (mode & 0x20000) {
      visit(cfg->get_root_cell());
    } else if (mode & 0x10000) {
      for (int i : param_list) {
        visit(cfg->get_config_param(i));
      }
    }
    if (!keyblk && mode & block::ConfigInfo::needPrevBlocks) {
      ((block::ConfigInfo*)cfg.get())->get_prev_blocks_info();
    }
  } catch (vm::VmError& err) {
    fatal_error("error while traversing required configuration parameters: "s + err.get_msg());
    return;
  }
  auto res1 = !keyblk ? vm::std_boc_serialize(std::move(proof1)) : td::BufferSlice();
  if (res1.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res1.move_as_error().to_string());
    return;
  }
  auto res2 = mpb.extract_proof_boc();
  if (res2.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res2.move_as_error().to_string());
    return;
  }
  LOG(INFO) << "getConfigParams() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_configInfo>(
      mode & 0xffff, ton::create_tl_lite_block_id(base_blk_id_), res1.move_as_ok(), res2.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getAllShardsInfo(BlockIdExt blkid) {
  LOG(INFO) << "started a getAllShardsInfo(" << blkid.to_str() << ") liteserver query";
  set_continuation([&]() -> void { continue_getAllShardsInfo(); });
  request_mc_block_data(blkid);
}

void LiteQuery::continue_getShardInfo(ShardIdFull shard, bool exact) {
  LOG(INFO) << "completing getShardInfo(" << shard.to_str() << ") query";
  Ref<vm::Cell> proof1, proof2, leaf;
  if (!make_mc_state_root_proof(proof1)) {
    return;
  }
  vm::CellSlice cs;
  ShardIdFull true_shard;
  bool found;
  Ref<block::McShardHash> shard_info;
  if (!make_shard_info_proof(proof2, shard_info, shard, true_shard, leaf, found, exact)) {
    return;
  }
  auto proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
  if (proof.is_error()) {
    fatal_error(proof.move_as_error());
    return;
  }
  BlockIdExt true_id;
  td::BufferSlice data;
  if (found) {
    if (shard_info.is_null()) {
      fatal_error("cannot unpack a leaf entry from ShardHashes");
      return;
    }
    true_id = shard_info->top_block_id();
    auto res = vm::std_boc_serialize(std::move(leaf));
    if (res.is_error()) {
      fatal_error(res.move_as_error());
      return;
    }
    data = res.move_as_ok();
  } else {
    true_id.invalidate_clear();
  }
  LOG(INFO) << "getShardInfo() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_shardInfo>(
      ton::create_tl_lite_block_id(base_blk_id_), ton::create_tl_lite_block_id(true_id), proof.move_as_ok(),
      std::move(data));
  finish_query(std::move(b));
}

void LiteQuery::continue_getAllShardsInfo() {
  LOG(INFO) << "completing getAllShardsInfo() query";
  vm::MerkleProofBuilder mpb{mc_block_->root_cell()};
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;
  block::gen::McBlockExtra::Record mc_extra;
  if (!tlb::unpack_cell(mpb.root(), blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
      !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
    fatal_error("cannot unpack header of block "s + mc_block_->block_id().to_str());
    return;
  }
  vm::Dictionary shards_dict(std::move(mc_extra.shard_hashes), 32);
  Ref<vm::Cell> proof;
  if (!mpb.extract_proof_to(proof)) {
    fatal_error("cannot construct Merkle proof for all shards dictionary");
    return;
  }
  auto proof_boc = vm::std_boc_serialize(std::move(proof));
  if (proof_boc.is_error()) {
    fatal_error(proof_boc.move_as_error());
    return;
  }
  vm::CellBuilder cb;
  Ref<vm::Cell> cell;
  if (!(shards_dict.append_dict_to_bool(cb) && cb.finalize_to(cell))) {
    fatal_error("cannot store ShardHashes from last mc block into a new cell");
    return;
  }
  auto data = vm::std_boc_serialize(std::move(cell));
  if (data.is_error()) {
    fatal_error(data.move_as_error());
    return;
  }
  LOG(INFO) << "getAllShardInfo() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_allShardsInfo>(
      ton::create_tl_lite_block_id(base_blk_id_), proof_boc.move_as_ok(), data.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_lookupBlockWithProof(BlockId blkid, BlockIdExt mc_blkid, int mode, LogicalTime lt, UnixTime utime) {
  if (!((1 << (mode & 7)) & 0x16)) {
    fatal_error("exactly one of mode.0, mode.1 and mode.2 bits must be set");
    return;
  }
  if (!mc_blkid.is_masterchain_ext()) {
    fatal_error("masterchain block id must be specified");
    return;
  }
  if (!(mode & 1)) {
    blkid.seqno = 0;
  }
  if (!(mode & 2)) {
    lt = 0;
  }
  if (!(mode & 4)) {
    utime = 0;
  }
  mode_ = mode;
  base_blk_id_ = mc_blkid;
  LOG(INFO) << "started a lookupBlockWithProof(" << blkid.to_str() << ", " << mc_blkid.to_str() << ", " << mode << ", "
            << lt << ", " << utime << ") liteserver query";

  ton::AccountIdPrefixFull pfx{blkid.workchain, blkid.shard};
  auto P = td::PromiseCreator::lambda(
    [Self = actor_id(this), mc_blkid, manager = manager_, pfx](td::Result<ConstBlockHandle> res) {
      if (res.is_error()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        return;
      }
      auto handle = res.move_as_ok();
      if (!handle->inited_masterchain_ref_block()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, td::Status::Error("block doesn't have masterchain ref"));
        return;
      }
      if (handle->masterchain_ref_block() > mc_blkid.seqno()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, td::Status::Error("specified mc block is older than block's masterchain ref"));
        return;
      }
      LOG(DEBUG) << "requesting data for block " << handle->id().to_str();
      td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db, handle,
                                    [Self, mc_ref_blkid = handle->masterchain_ref_block(), pfx](td::Result<Ref<BlockData>> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::continue_lookupBlockWithProof_getHeaderProof, res.move_as_ok(), pfx, mc_ref_blkid);
        }
      });
  });

  if (mode & 2) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_lt_for_litequery, pfx, lt, std::move(P));
  } else if (mode & 4) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_unix_time_for_litequery, pfx, utime,
                                  std::move(P));
  } else {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_seqno_for_litequery, pfx, blkid.seqno,
                                  std::move(P));
  }
}

void LiteQuery::continue_lookupBlockWithProof_getHeaderProof(Ref<ton::validator::BlockData> block, AccountIdPrefixFull req_prefix, BlockSeqno masterchain_ref_seqno) {
  blk_id_ = block->block_id();
  LOG(INFO) << "obtained data for getBlockHeader(" << blk_id_.to_str() << ", " << mode_ << ")";
  CHECK(block.not_null());
  auto block_root = block->root_cell();
  if (block_root.is_null()) {
    fatal_error("block has no valid root cell");
    return;
  }

  vm::MerkleProofBuilder mpb{block_root};
  std::vector<BlockIdExt> prev;
  BlockIdExt mc_blkid;
  bool after_split;
  td::Status S = block::unpack_block_prev_blk_try(mpb.root(), blk_id_, prev, mc_blkid, after_split);
  if (S.is_error()) {
    fatal_error(std::move(S));
    return;
  }
  auto proof_data = mpb.extract_proof_boc();
  if (proof_data.is_error()) {
    fatal_error(proof_data.move_as_error());
    return;
  }
  lookup_header_proof_ = proof_data.move_as_ok();

  bool include_prev = mode_ & 6;
  if (include_prev) {
    BlockIdExt prev_blkid;
    for (auto& p : prev) {
      if (ton::shard_contains(p.shard_full(), req_prefix)) {
        prev_blkid = p;
      }
    }
    CHECK(prev_blkid.is_valid());
    get_block_handle_checked(prev_blkid, [Self = actor_id(this), masterchain_ref_seqno, manager = manager_](td::Result<ConstBlockHandle> R) mutable {
      if (R.is_error()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        return;
      }
      td::actor::send_closure(manager, &ValidatorManager::get_block_data_from_db, R.move_as_ok(), 
                              [Self, masterchain_ref_seqno](td::Result<Ref<BlockData>> res) mutable {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          return;
        }
        td::actor::send_closure(Self, &LiteQuery::continue_lookupBlockWithProof_gotPrevBlockData, res.move_as_ok(), masterchain_ref_seqno);
      });
    });
  } else {
    continue_lookupBlockWithProof_gotPrevBlockData(Ref<BlockData>(), masterchain_ref_seqno);
  }
}

void LiteQuery::continue_lookupBlockWithProof_gotPrevBlockData(Ref<BlockData> prev_block, BlockSeqno masterchain_ref_seqno) {
  if (prev_block.not_null()) {
    CHECK(prev_block.not_null());
    if (prev_block->root_cell().is_null()) {
      fatal_error("block has no valid root cell");
      return;
    }
    vm::MerkleProofBuilder mpb{prev_block->root_cell()};
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(mpb.root(), blk) && tlb::unpack_cell(blk.info, info))) {
      fatal_error(td::Status::Error("cannot unpack prev block header"));
      return;
    }
    auto proof_data = mpb.extract_proof_boc();
    if (proof_data.is_error()) {
      fatal_error(proof_data.move_as_error());
      return;
    }
    lookup_prev_header_proof_ = proof_data.move_as_ok();
  }

  if (!blk_id_.is_masterchain()) {
    ton::AccountIdPrefixFull pfx{ton::masterchainId, ton::shardIdAll};
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_seqno_from_db, pfx, masterchain_ref_seqno, 
        [manager = manager_, Self = actor_id(this)](td::Result<ConstBlockHandle> R) mutable {
      if (R.is_error()) {
        td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        return;
      }
      td::actor::send_closure(manager, &ValidatorManager::get_block_data_from_db, R.move_as_ok(), 
                              [Self](td::Result<Ref<BlockData>> res) mutable {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          return;
        }
        td::actor::send_closure(Self, &LiteQuery::continue_lookupBlockWithProof_buildProofLinks, res.move_as_ok(), std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>>());
      });
    });
  } else {
    base_blk_id_alt_ = blk_id_;
    td::actor::send_closure(actor_id(this), &LiteQuery::continue_lookupBlockWithProof_getClientMcBlockDataState, std::vector<std::pair<BlockIdExt, Ref<vm::Cell>>>());
  }
}

void LiteQuery::continue_lookupBlockWithProof_buildProofLinks(td::Ref<BlockData> cur_block, 
                                        std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>> result) {
  BlockIdExt cur_id = cur_block->block_id();
  BlockIdExt prev_id;
  vm::MerkleProofBuilder mpb{cur_block->root_cell()};
  if (cur_id.is_masterchain()) {
    base_blk_id_alt_ = cur_id;
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    block::gen::McBlockExtra::Record mc_extra;
    if (!tlb::unpack_cell(mpb.root(), blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
        !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
      fatal_error("cannot unpack header of block "s + cur_id.to_str());
      return;
    }
    block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());
    ShardIdFull shard_id = blk_id_.shard_full();
    shard_id.shard = (shard_id.shard & ~(1 << (63 - shard_id.pfx_len()))) | 1;
    Ref<block::McShardHash> shard_hash = shards.get_shard_hash(shard_id, false);
    if (shard_hash.is_null()) {
      fatal_error("shard not found");
      return;
    }
    prev_id = shard_hash->top_block_id();
  } else {
    std::vector<BlockIdExt> prev;
    BlockIdExt mc_blkid;
    bool after_split;
    td::Status S = block::unpack_block_prev_blk_try(mpb.root(), cur_id, prev, mc_blkid, after_split);
    if (S.is_error()) {
      fatal_error(std::move(S));
      return;
    }
    bool found = false;
    for (const BlockIdExt& id : prev) {
      if (shard_intersects(id.shard_full(), blk_id_.shard_full())) {
        found = true;
        prev_id = id;
        break;
      }
    }
    if (!found) {
      fatal_error("failed to find block chain");
      return;
    }
  }
  auto proof = mpb.extract_proof();
  if (proof.is_error()) {
    fatal_error(proof.move_as_error_prefix("cannot serialize Merkle proof : "));
    return;
  }
  result.emplace_back(prev_id, proof.move_as_ok());

  if (prev_id == blk_id_) {
    CHECK(base_blk_id_alt_.is_masterchain());
    if (base_blk_id_alt_ != base_blk_id_) {
      continue_lookupBlockWithProof_getClientMcBlockDataState(std::move(result));
    } else {
      continue_lookupBlockWithProof_getMcBlockPrev(std::move(result));
    }
    return;
  }
  if (result.size() == 8) {
    // Chains of shardblocks between masterchain blocks can't be longer than 8 (see collator.cpp:991)
    fatal_error("proof chain is too long");
    return;
  }

  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_data_from_db_short, prev_id,
      [Self = actor_id(this), result = std::move(result)](td::Result<Ref<BlockData>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::continue_lookupBlockWithProof_buildProofLinks, R.move_as_ok(),
                                        std::move(result));
        }
  });
}

void LiteQuery::continue_lookupBlockWithProof_getClientMcBlockDataState(std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>> links) {
  set_continuation([this, links = std::move(links)]() -> void {
    continue_lookupBlockWithProof_getMcBlockPrev(std::move(links));
  });
  request_mc_block_data_state(base_blk_id_);
}

void LiteQuery::continue_lookupBlockWithProof_getMcBlockPrev(std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>> links) {
  td::BufferSlice mc_state_proof_buf, client_mc_blk_proof_buf;
  
  if (base_blk_id_alt_ != base_blk_id_) {
    vm::MerkleProofBuilder mpb{mc_state_->root_cell()};
    auto prev_blocks_dict = block::get_prev_blocks_dict(mpb.root());
    if (!prev_blocks_dict) {
      fatal_error(td::Status::Error("cannot extract prev_blocks from mc state"));
      return;
    }
    if (!block::check_old_mc_block_id(*prev_blocks_dict, base_blk_id_alt_)) {
      fatal_error(td::Status::Error("client mc blkid is not in prev_blocks"));
      return;
    }
    auto client_mc_blk_proof = mpb.extract_proof_boc();
    if (client_mc_blk_proof.is_error()) {
      fatal_error(client_mc_blk_proof.move_as_error());
      return;
    }
    client_mc_blk_proof_buf = client_mc_blk_proof.move_as_ok();

    Ref<vm::Cell> mc_state_proof;
    if (!make_mc_state_root_proof(mc_state_proof)) {
      fatal_error(td::Status::Error("cannot create Merkle proof for mc state"));
      return;
    }
    auto mc_state_proof_boc = vm::std_boc_serialize(std::move(mc_state_proof));
    if (mc_state_proof_boc.is_error()) {
      fatal_error(mc_state_proof_boc.move_as_error());
      return;
    }
    mc_state_proof_buf = mc_state_proof_boc.move_as_ok();
  }

  std::vector<tl_object_ptr<lite_api::liteServer_shardBlockLink>> links_res;
  for (auto& p : links) {
    auto prev_block_proof = vm::std_boc_serialize(std::move(p.second));
    if (prev_block_proof.is_error()) {
      fatal_error(prev_block_proof.move_as_error());
      return;
    }
    links_res.push_back(
        create_tl_object<lite_api::liteServer_shardBlockLink>(create_tl_lite_block_id(p.first), prev_block_proof.move_as_ok()));
  }

  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_lookupBlockResult>(ton::create_tl_lite_block_id(blk_id_),
                                                mode_, ton::create_tl_lite_block_id(base_blk_id_alt_), std::move(mc_state_proof_buf), std::move(client_mc_blk_proof_buf), 
                                                std::move(links_res), std::move(lookup_header_proof_), std::move(lookup_prev_header_proof_));
  finish_query(std::move(b));
}


void LiteQuery::perform_lookupBlock(BlockId blkid, int mode, LogicalTime lt, UnixTime utime) {
  if (!((1 << (mode & 7)) & 0x16)) {
    fatal_error("exactly one of mode.0, mode.1 and mode.2 bits must be set");
    return;
  }
  if (!(mode & 2)) {
    lt = 0;
  }
  if (!(mode & 4)) {
    utime = 0;
  }
  LOG(INFO) << "performing a lookupBlock(" << blkid.to_str() << ", " << mode << ", " << lt << ", " << utime
            << ") query";
  auto P = td::PromiseCreator::lambda(
      [Self = actor_id(this), manager = manager_, mode = (mode >> 4)](td::Result<ConstBlockHandle> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          auto handle = res.move_as_ok();
          LOG(DEBUG) << "requesting data for block " << handle->id().to_str();
          td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db, handle,
                                        [Self, blkid = handle->id(), mode](td::Result<Ref<BlockData>> res) {
                                          if (res.is_error()) {
                                            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                          } else {
                                            td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockHeader,
                                                                          blkid, mode, res.move_as_ok());
                                          }
                                        });
        }
      });

  ton::AccountIdPrefixFull pfx{blkid.workchain, blkid.shard};
  if (mode & 2) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_lt_for_litequery, pfx, lt,
                                  std::move(P));
  } else if (mode & 4) {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_unix_time_for_litequery, pfx, utime,
                                  std::move(P));
  } else {
    td::actor::send_closure_later(manager_, &ValidatorManager::get_block_by_seqno_for_litequery, pfx,
                                  blkid.seqno, std::move(P));
  }
}

void LiteQuery::perform_listBlockTransactions(BlockIdExt blkid, int mode, int count, Bits256 account, LogicalTime lt) {
  LOG(INFO) << "started a listBlockTransactions(" << blkid.to_str() << ", " << mode << ", " << count << ", "
            << account.to_hex() << ", " << lt << ") liteserver query";
  base_blk_id_ = blkid;
  acc_addr_ = account;
  trans_lt_ = lt;
  set_continuation([this, mode, count]() -> void { finish_listBlockTransactions(mode, count); });
  request_block_data(blkid);
}

static td::Result<tl_object_ptr<lite_api::liteServer_transactionMetadata>> get_in_msg_metadata(
    const Ref<vm::Cell>& in_msg_descr_root, const Ref<vm::Cell>& trans_root) {
  vm::AugmentedDictionary in_msg_descr{vm::load_cell_slice_ref(in_msg_descr_root), 256, block::tlb::aug_InMsgDescr};
  block::gen::Transaction::Record transaction;
  if (!block::tlb::unpack_cell(trans_root, transaction)) {
    return td::Status::Error("invalid Transaction in block");
  }
  Ref<vm::Cell> msg = transaction.r1.in_msg->prefetch_ref();
  if (msg.is_null()) {
    return nullptr;
  }
  td::Bits256 in_msg_hash = msg->get_hash().bits();
  Ref<vm::CellSlice> in_msg = in_msg_descr.lookup(in_msg_hash);
  if (in_msg.is_null()) {
    return td::Status::Error(PSTRING() << "no InMsg in InMsgDescr for message with hash " << in_msg_hash.to_hex());
  }
  int tag = block::gen::t_InMsg.get_tag(*in_msg);
  if (tag != block::gen::InMsg::msg_import_imm && tag != block::gen::InMsg::msg_import_fin &&
      tag != block::gen::InMsg::msg_import_deferred_fin) {
    return nullptr;
  }
  Ref<vm::Cell> msg_env = in_msg->prefetch_ref();
  if (msg_env.is_null()) {
    return td::Status::Error(PSTRING() << "no MsgEnvelope in InMsg for message with hash " << in_msg_hash.to_hex());
  }
  block::tlb::MsgEnvelope::Record_std env;
  if (!block::tlb::unpack_cell(std::move(msg_env), env)) {
    return td::Status::Error(PSTRING() << "failed to unpack MsgEnvelope for message with hash " << in_msg_hash.to_hex());
  }
  if (!env.metadata) {
    return nullptr;
  }
  block::MsgMetadata& metadata = env.metadata.value();
  return create_tl_object<lite_api::liteServer_transactionMetadata>(
      0, metadata.depth,
      create_tl_object<lite_api::liteServer_accountId>(metadata.initiator_wc, metadata.initiator_addr),
      metadata.initiator_lt);
}

void LiteQuery::finish_listBlockTransactions(int mode, int req_count) {
  LOG(INFO) << "completing a listBlockTransactions(" << base_blk_id_.to_str() << ", " << mode << ", " << req_count
            << ", " << acc_addr_.to_hex() << ", " << trans_lt_ << ") liteserver query";
  constexpr int max_answer_transactions = 256;
  CHECK(block_.not_null());
  auto block_root = block_->root_cell();
  CHECK(block_root.not_null());
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == base_blk_id_.root_hash);
  vm::MerkleProofBuilder pb;
  auto virt_root = block_root;
  if (mode & 32) {
    // proof requested
    virt_root = pb.init(std::move(virt_root));
  }
  if ((mode & 192) == 64) {  // reverse order, no starting point
    acc_addr_.set_ones();
    trans_lt_ = ~0ULL;
  }
  bool with_metadata = mode & 256;
  mode &= ~256;
  std::vector<tl_object_ptr<lite_api::liteServer_transactionId>> result;
  bool eof = false;
  ton::LogicalTime reverse = (mode & 64) ? ~0ULL : 0;
  try {
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(std::move(blk.extra), extra))) {
      fatal_error("cannot find account transaction data in block "s + base_blk_id_.to_str());
      return;
    }
    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    int count = 0;
    bool allow_same = true;
    td::Bits256 cur_addr = acc_addr_;
    while (!eof && count < req_count && count < max_answer_transactions) {
      Ref<vm::CellSlice> value;
      try {
        value = acc_dict.extract_value(
            acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, !reverse, allow_same));
      } catch (vm::VmError err) {
        fatal_error("error while traversing account block dictionary: "s + err.get_msg());
        return;
      }
      if (value.is_null()) {
        eof = true;
        break;
      }
      allow_same = false;
      if (cur_addr != acc_addr_) {
        trans_lt_ = reverse;
      }
      block::gen::AccountBlock::Record acc_blk;
      if (!(tlb::csr_unpack(std::move(value), acc_blk) && acc_blk.account_addr == cur_addr)) {
        fatal_error("invalid AccountBlock for account "s + cur_addr.to_hex());
        return;
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
      td::BitArray<64> cur_trans{(long long)trans_lt_};
      while (count < req_count && count < max_answer_transactions) {
        Ref<vm::Cell> tvalue;
        try {
          tvalue = trans_dict.extract_value_ref(
              trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, !reverse));
        } catch (vm::VmError err) {
          fatal_error("error while traversing transaction dictionary of an AccountBlock: "s + err.get_msg());
          return;
        }
        if (tvalue.is_null()) {
          trans_lt_ = reverse;
          break;
        }
        tl_object_ptr<lite_api::liteServer_transactionMetadata> metadata;
        if (with_metadata) {
          auto r_metadata = get_in_msg_metadata(extra.in_msg_descr, tvalue);
          if (r_metadata.is_error()) {
            fatal_error(r_metadata.move_as_error());
            return;
          }
          metadata = r_metadata.move_as_ok();
        }
        result.push_back(create_tl_object<lite_api::liteServer_transactionId>(
            mode | (metadata ? 256 : 0), cur_addr, cur_trans.to_long(), tvalue->get_hash().bits(),
            std::move(metadata)));
        ++count;
      }
    }
  } catch (vm::VmError err) {
    fatal_error("error while parsing AccountBlocks of block "s + base_blk_id_.to_str() + " : " + err.get_msg());
    return;
  }
  td::BufferSlice proof_data;
  if (mode & 32) {
    // create proof
    auto proof_boc = pb.extract_proof_boc();
    if (proof_boc.is_error()) {
      fatal_error(proof_boc.move_as_error());
      return;
    }
    proof_data = proof_boc.move_as_ok();
  }

  LOG(INFO) << "listBlockTransactions() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockTransactions>(
      ton::create_tl_lite_block_id(base_blk_id_), req_count, !eof, std::move(result), std::move(proof_data));
  finish_query(std::move(b));
}

void LiteQuery::perform_listBlockTransactionsExt(BlockIdExt blkid, int mode, int count, Bits256 account, LogicalTime lt) {
  LOG(INFO) << "started a listBlockTransactionsExt(" << blkid.to_str() << ", " << mode << ", " << count << ", "
            << account.to_hex() << ", " << lt << ") liteserver query";
  base_blk_id_ = blkid;
  acc_addr_ = account;
  trans_lt_ = lt;
  set_continuation([this, mode, count]() -> void { finish_listBlockTransactionsExt(mode, count); });
  request_block_data(blkid);
}

static td::Status process_all_in_msg_metadata(const Ref<vm::Cell>& in_msg_descr_root,
                                              const std::vector<Ref<vm::Cell>>& trans_roots) {
  vm::AugmentedDictionary in_msg_descr{vm::load_cell_slice_ref(in_msg_descr_root), 256, block::tlb::aug_InMsgDescr};
  for (const Ref<vm::Cell>& trans_root : trans_roots) {
    block::gen::Transaction::Record transaction;
    if (!block::tlb::unpack_cell(trans_root, transaction)) {
      return td::Status::Error("invalid Transaction in block");
    }
    Ref<vm::Cell> msg = transaction.r1.in_msg->prefetch_ref();
    if (msg.is_null()) {
      continue;
    }
    td::Bits256 in_msg_hash = msg->get_hash().bits();
    Ref<vm::CellSlice> in_msg = in_msg_descr.lookup(in_msg_hash);
    if (in_msg.is_null()) {
      return td::Status::Error(PSTRING() << "no InMsg in InMsgDescr for message with hash " << in_msg_hash.to_hex());
    }
    int tag = block::gen::t_InMsg.get_tag(*in_msg);
    if (tag == block::gen::InMsg::msg_import_imm || tag == block::gen::InMsg::msg_import_fin ||
        tag == block::gen::InMsg::msg_import_deferred_fin) {
      Ref<vm::Cell> msg_env = in_msg->prefetch_ref();
      if (msg_env.is_null()) {
        return td::Status::Error(PSTRING() << "no MsgEnvelope in InMsg for message with hash " << in_msg_hash.to_hex());
      }
      vm::load_cell_slice(msg_env);
    }
  }
  return td::Status::OK();
}

void LiteQuery::finish_listBlockTransactionsExt(int mode, int req_count) {
  LOG(INFO) << "completing a listBlockTransactionsExt(" << base_blk_id_.to_str() << ", " << mode << ", " << req_count
            << ", " << acc_addr_.to_hex() << ", " << trans_lt_ << ") liteserver query";
  constexpr int max_answer_transactions = 256;
  CHECK(block_.not_null());
  auto block_root = block_->root_cell();
  CHECK(block_root.not_null());
  RootHash rhash{block_root->get_hash().bits()};
  CHECK(rhash == base_blk_id_.root_hash);
  vm::MerkleProofBuilder pb;
  auto virt_root = block_root;
  if (mode & 256) {
    // with msg metadata in proof
    mode |= 32;
  }
  if (mode & 32) {
    // proof requested
    virt_root = pb.init(std::move(virt_root));
  }
  if ((mode & 192) == 64) {  // reverse order, no starting point
    acc_addr_.set_ones();
    trans_lt_ = ~0ULL;
  }
  std::vector<Ref<vm::Cell>> trans_roots;
  bool eof = false;
  ton::LogicalTime reverse = (mode & 64) ? ~0ULL : 0;
  try {
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(std::move(blk.extra), extra))) {
      fatal_error("cannot find account transaction data in block "s + base_blk_id_.to_str());
      return;
    }
    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    int count = 0;
    bool allow_same = true;
    td::Bits256 cur_addr = acc_addr_;
    while (!eof && count < req_count && count < max_answer_transactions) {
      Ref<vm::CellSlice> value;
      try {
        value = acc_dict.extract_value(
            acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, !reverse, allow_same));
      } catch (vm::VmError err) {
        fatal_error("error while traversing account block dictionary: "s + err.get_msg());
        return;
      }
      if (value.is_null()) {
        eof = true;
        break;
      }
      allow_same = false;
      if (cur_addr != acc_addr_) {
        trans_lt_ = reverse;
      }
      block::gen::AccountBlock::Record acc_blk;
      if (!(tlb::csr_unpack(std::move(value), acc_blk) && acc_blk.account_addr == cur_addr)) {
        fatal_error("invalid AccountBlock for account "s + cur_addr.to_hex());
        return;
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
      td::BitArray<64> cur_trans{(long long)trans_lt_};
      while (count < req_count && count < max_answer_transactions) {
        Ref<vm::Cell> tvalue;
        try {
          tvalue = trans_dict.extract_value_ref(
              trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, !reverse));
        } catch (vm::VmError err) {
          fatal_error("error while traversing transaction dictionary of an AccountBlock: "s + err.get_msg());
          return;
        }
        if (tvalue.is_null()) {
          trans_lt_ = reverse;
          break;
        }
        trans_roots.push_back(std::move(tvalue));
        ++count;
      }
    }
    if (mode & 256) {
      td::Status S = process_all_in_msg_metadata(extra.in_msg_descr, trans_roots);
      if (S.is_error()) {
        fatal_error(S.move_as_error());
        return;
      }
    }
  } catch (vm::VmError err) {
    fatal_error("error while parsing AccountBlocks of block "s + base_blk_id_.to_str() + " : " + err.get_msg());
    return;
  }
  td::BufferSlice proof_data;
  if (mode & 32) {
    // create proof
    auto proof_boc = pb.extract_proof_boc();
    if (proof_boc.is_error()) {
      fatal_error(proof_boc.move_as_error());
      return;
    }
    proof_data = proof_boc.move_as_ok();
  }
  auto res = vm::std_boc_serialize_multi(std::move(trans_roots));
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockTransactionsExt>(
      ton::create_tl_lite_block_id(base_blk_id_), req_count, !eof, res.move_as_ok(), std::move(proof_data));
  LOG(INFO) << "listBlockTransactionsExt() query completed";
  finish_query(std::move(b));
}

void LiteQuery::perform_getBlockProof(ton::BlockIdExt from, ton::BlockIdExt to, int mode) {
  if (!(mode & 1)) {
    to.invalidate_clear();
  }
  LOG(INFO) << "performing a getBlockProof(" << mode << ", " << from.to_str() << ", " << to.to_str() << ") query";
  if (!from.is_masterchain_ext()) {
    fatal_error("source block "s + from.to_str() + " is not a valid masterchain block id");
    return;
  }
  if ((mode & 1) && !to.is_masterchain_ext()) {
    fatal_error("destination block "s + to.to_str() + " is not a valid masterchain block id");
    return;
  }
  if (mode & 1) {
    if (mode & 0x1000) {
      BlockIdExt bblk = (from.seqno() > to.seqno()) ? from : to;
      td::actor::send_closure_later(manager_, &ValidatorManager::get_shard_state_from_db_short, bblk,
                                    [Self = actor_id(this), from, to, bblk, mode](td::Result<Ref<ShardState>> res) {
                                      if (res.is_error()) {
                                        td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                      } else {
                                        td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockProof, from,
                                                                      to, mode, bblk,
                                                                      Ref<MasterchainStateQ>(res.move_as_ok()));
                                      }
                                    });
    } else {
      td::actor::send_closure_later(
          manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
          [Self = actor_id(this), from, to, mode](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
            if (res.is_error()) {
              td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
            } else {
              auto pair = res.move_as_ok();
              td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockProof, from, to, mode, pair.second,
                                            Ref<MasterchainStateQ>(std::move(pair.first)));
            }
          });
    }
  } else if (mode & 2) {
    td::actor::send_closure_later(
        manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
        [Self = actor_id(this), from, mode](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
          if (res.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
          } else {
            auto pair = res.move_as_ok();
            td::actor::send_closure_later(Self, &LiteQuery::continue_getBlockProof, from, pair.second, mode,
                                          pair.second, Ref<MasterchainStateQ>(std::move(pair.first)));
          }
        });
  } else {
    td::actor::send_closure_later(manager_, &ton::validator::ValidatorManager::get_shard_client_state, false,
                                  [Self = actor_id(this), from, mode](td::Result<BlockIdExt> res) {
                                    if (res.is_error()) {
                                      td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
                                    } else {
                                      td::actor::send_closure_later(Self, &LiteQuery::perform_getBlockProof, from,
                                                                    res.move_as_ok(), mode | 0x1001);
                                    }
                                  });
  }
}

void LiteQuery::continue_getBlockProof(ton::BlockIdExt from, ton::BlockIdExt to, int mode, BlockIdExt baseblk,
                                       Ref<MasterchainStateQ> state) {
  base_blk_id_ = baseblk;
  if (!base_blk_id_.is_masterchain_ext()) {
    fatal_error("reference masterchain block "s + base_blk_id_.to_str() + " for constructing a proof chain is invalid");
    return;
  }
  if (!(mode & 1)) {
    if (!to.is_masterchain_ext()) {
      fatal_error("last masterchain block id "s + to.to_str() + " is invalid");
      return;
    }
  }
  if (state.is_null()) {
    fatal_error("obtained no valid masterchain state for block "s + base_blk_id_.to_str());
    return;
  }
  if (from.seqno() > base_blk_id_.seqno()) {
    fatal_error("client knows block "s + from.to_str() + " newer than the reference masterchain block " +
                base_blk_id_.to_str());
    return;
  }
  if (to.seqno() > base_blk_id_.seqno()) {
    fatal_error("client knows block "s + to.to_str() + " newer than the reference masterchain block " +
                base_blk_id_.to_str());
    return;
  }
  mc_state0_ = Ref<MasterchainStateQ>(state);
  if (base_blk_id_ != state->get_block_id()) {
    fatal_error("the state for "s + base_blk_id_.to_str() + " is in fact a state for different block " +
                state->get_block_id().to_str());
    return;
  }
  LOG(INFO) << "continuing getBlockProof(" << mode << ", " << from.to_str() << ", " << to.to_str()
            << ") query with a state for " << base_blk_id_.to_str();
  if (!state->check_old_mc_block_id(from)) {
    fatal_error("proof source masterchain block "s + from.to_str() +
                " is unknown from the perspective of reference block " + base_blk_id_.to_str());
    return;
  }
  if (!state->check_old_mc_block_id(to)) {
    fatal_error("proof destination masterchain block "s + to.to_str() +
                " is unknown from the perspective of reference block " + base_blk_id_.to_str());
    return;
  }
  chain_ = std::make_unique<block::BlockProofChain>(from, to, mode);
  blk_id_ = from;
  construct_proof_chain(from);
}

bool LiteQuery::construct_proof_chain(ton::BlockIdExt id) {
  CHECK(chain_);
  if (chain_->link_count() >= 16 || id == chain_->to) {
    if (!(chain_->last_link_incomplete() && chain_->last_link().to.seqno())) {
      return finish_proof_chain(std::move(id));
    } else {
      set_continuation([this, id]() { finish_proof_chain(id); });
      return request_proof_link(id);
    }
  }
  if (chain_->to.seqno() == id.seqno()) {
    return fatal_error("cannot have two different masterchain blocks "s + chain_->to.to_str() + " and " + id.to_str() +
                       " of the same height");
  }
  if (chain_->to.seqno() < id.seqno()) {
    return construct_proof_link_back(id, chain_->to);
  }
  auto prev_key_blk = mc_state0_->prev_key_block_id(id.seqno());
  if (!prev_key_blk.is_masterchain_ext()) {
    return fatal_error("cannot compute previous key block for "s + id.to_str());
  }
  if (prev_key_blk.seqno() > id.seqno() || (prev_key_blk.seqno() == id.seqno() && prev_key_blk != id)) {
    return fatal_error("block "s + prev_key_blk.to_str() + " cannot be the previous key block for " + id.to_str());
  }
  if (prev_key_blk.seqno() != id.seqno()) {
    return construct_proof_link_back(id, prev_key_blk);
  }
  auto next_key_blk = mc_state0_->next_key_block_id(id.seqno() + 1);
  if (next_key_blk.is_valid()) {
    if (!(next_key_blk.is_masterchain_ext() && next_key_blk.seqno() > id.seqno())) {
      return fatal_error("block "s + next_key_blk.to_str() + " cannot be the next key block after " + id.to_str());
    }
    return construct_proof_link_forward(id, next_key_blk);
  } else {
    return construct_proof_link_forward(id, chain_->to);
  }
}

// adjust dest_proof and is_key of the last link of existing proof
bool LiteQuery::adjust_last_proof_link(ton::BlockIdExt cur, Ref<vm::Cell> block_root) {
  CHECK(chain_);
  if (!(chain_->last_link_incomplete() && chain_->last_link().to.seqno())) {
    return true;
  }
  auto& link = chain_->last_link();
  CHECK(link.dest_proof.is_null());
  CHECK(link.to == cur);
  if (cur.root_hash != block_root->get_hash().bits()) {
    return fatal_error("root hash mismatch in block root of "s + cur.to_str());
  }
  vm::MerkleProofBuilder mpb{std::move(block_root)};
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(mpb.root(), blk) && tlb::unpack_cell(blk.info, info))) {
    return fatal_error("cannot unpack header of block "s + cur.to_str());
  }
  link.is_key = info.key_block;
  return mpb.extract_proof_to(link.dest_proof);
}

bool LiteQuery::construct_proof_link_forward(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "constructing a forward proof link from " << cur.to_str() << " to " << next.to_str();
  if (!(cur.is_masterchain_ext() && next.is_masterchain_ext() && mc_state0_->check_old_mc_block_id(cur) &&
        mc_state0_->check_old_mc_block_id(next))) {
    return fatal_error("cannot construct forward proof link from "s + cur.to_str() + " to " + next.to_str() +
                       " because these are not known masterchain block ids");
  }
  if (cur.seqno() >= next.seqno()) {
    return fatal_error("cannot construct forward proof link from "s + cur.to_str() + " to " + next.to_str());
  }
  set_continuation([this, cur, next]() { construct_proof_link_forward_cont(cur, next); });
  return (cur.seqno() ? request_proof_link(cur) : request_zero_state(cur)) && request_mc_proof(next);
}

bool LiteQuery::construct_proof_link_forward_cont(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "continue constructing a forward proof link from " << cur.to_str() << " to " << next.to_str();
  CHECK(cur.seqno() ? proof_link_.not_null() && proof_link_->block_id() == cur : !buffer_.empty());
  CHECK(mc_proof_.not_null() && mc_proof_->block_id() == next);
  try {
    Ref<vm::Cell> cur_root, next_root;
    // virtualize roots
    ton::validator::ProofQ::VirtualizedProof virt1;
    if (cur.seqno()) {
      auto vres1 = proof_link_->get_virtual_root();
      if (vres1.is_error()) {
        return fatal_error(vres1.move_as_error());
      }
      virt1 = vres1.move_as_ok();
      cur_root = virt1.root;
    } else {
      // for zero state, lazily deserialize buffer_ instead
      vm::StaticBagOfCellsDbLazy::Options options;
      options.check_crc32c = true;
      auto res = vm::StaticBagOfCellsDbLazy::create(td::BufferSliceBlobView::create(std::move(buffer_)), options);
      if (res.is_error()) {
        return fatal_error(res.move_as_error());
      }
      virt1.boc = res.move_as_ok();
      auto t_root = virt1.boc->get_root_cell(0);
      if (t_root.is_error()) {
        return fatal_error(t_root.move_as_error());
      }
      cur_root = t_root.move_as_ok();
    }
    auto vres2 = mc_proof_->get_virtual_root();
    if (vres2.is_error()) {
      return fatal_error(vres2.move_as_error());
    }
    next_root = vres2.ok().root;
    if (cur.root_hash != cur_root->get_hash().bits()) {
      return fatal_error("incorrect root hash in ProofLink for block "s + cur.to_str());
    }
    if (next.root_hash != next_root->get_hash().bits()) {
      return fatal_error("incorrect root hash in ProofLink for block "s + cur.to_str());
    }
    // adjust dest_proof and is_key of the last link of existing proof
    if (!adjust_last_proof_link(cur, cur_root)) {
      return false;
    }
    // extract configuration from current block
    vm::MerkleProofBuilder cur_mpb{cur_root}, next_mpb{next_root};
    if (cur.seqno()) {
      auto err = block::check_block_header(cur_mpb.root(), cur);
      if (err.is_error()) {
        return fatal_error("incorrect header in ProofLink for block "s + cur.to_str());
      }
    }
    auto cfg_res = cur.seqno()
                       ? block::Config::extract_from_key_block(cur_mpb.root(), block::ConfigInfo::needValidatorSet)
                       : block::Config::extract_from_state(cur_mpb.root(), block::ConfigInfo::needValidatorSet);
    if (cfg_res.is_error()) {
      return fatal_error(cfg_res.move_as_error());
    }
    auto config = cfg_res.move_as_ok();
    // unpack header of next block
    auto err = block::check_block_header(next_mpb.root(), next);
    if (err.is_error()) {
      return fatal_error("incorrect header in ProofLink for block "s + next.to_str());
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(next_mpb.root(), blk) && tlb::unpack_cell(blk.info, info))) {
      return fatal_error("cannot unpack header of block "s + cur.to_str());
    }
    // compute validator set
    ShardIdFull shard{masterchainId};
    auto nodes = config->compute_validator_set(shard, info.gen_utime, info.gen_catchain_seqno);
    if (nodes.empty()) {
      return fatal_error(PSTRING() << "cannot compute validator set for block " << next.to_str() << " with utime "
                                   << info.gen_utime << " and cc_seqno " << info.gen_catchain_seqno
                                   << " starting from previous key block " << cur.to_str());
    }
    auto vset = Ref<ValidatorSetQ>{true, info.gen_catchain_seqno, shard, std::move(nodes)};
    if (vset.is_null()) {
      return fatal_error(PSTRING() << "cannot create validator set for block " << next.to_str() << " with utime "
                                   << info.gen_utime << " and cc_seqno " << info.gen_catchain_seqno
                                   << " starting from previous key block " << cur.to_str());
    }
    auto vset_hash = vset->get_validator_set_hash();
    if (vset_hash != info.gen_validator_list_hash_short) {
      return fatal_error(PSTRING() << "computed validator set for block " << next.to_str() << " with utime "
                                   << info.gen_utime << " and cc_seqno " << info.gen_catchain_seqno
                                   << " starting from previous key block " << cur.to_str() << " has hash " << vset_hash
                                   << " different from " << info.gen_validator_list_hash_short
                                   << " stated in block header");
    }
    // extract signatures
    auto sig_outer_root = vres2.ok().sig_root;
    block::gen::BlockSignatures::Record sign_rec;
    block::gen::BlockSignaturesPure::Record sign_pure;
    if (!(sig_outer_root.not_null() && tlb::unpack_cell(sig_outer_root, sign_rec) &&
          tlb::csr_unpack(sign_rec.pure_signatures, sign_pure))) {
      return fatal_error("cannot extract signature set from proof for block "s + next.to_str());
    }
    auto sigs = BlockSignatureSetQ::fetch(sign_pure.signatures->prefetch_ref());
    if (sigs.is_null()) {
      return fatal_error("cannot deserialize signature set from proof for block "s + next.to_str());
    }
    // check signatures (sanity check; comment later for better performance)
    /*
    auto S = vset->check_signatures(next.root_hash, next.file_hash, sigs);
    if (S.is_error()) {
      return fatal_error("error checking signatures from proof for block "s + next.to_str() + " : " +
                         S.move_as_error().to_string());
    }
    */
    // serialize signatures
    auto& link = chain_->new_link(cur, next, info.key_block);
    link.cc_seqno = info.gen_catchain_seqno;
    link.validator_set_hash = info.gen_validator_list_hash_short;
    link.signatures = std::move(sigs.write().signatures());
    // serialize proofs
    if (!(cur_mpb.extract_proof_to(link.proof) && next_mpb.extract_proof_to(link.dest_proof))) {
      return fatal_error("error constructing Merkle proof for forward proof link from "s + cur.to_str() + " to " +
                         next.to_str());
    }
    // continue constructing from `next`
    return construct_proof_chain(next);
  } catch (vm::VmVirtError&) {
    return fatal_error("virtualization error during construction of forward proof link from "s + cur.to_str() + " to " +
                       next.to_str());
  }
  return fatal_error("construction of forward proof links not implemented yet");
}

bool LiteQuery::construct_proof_link_back(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "constructing a backward proof link from " << cur.to_str() << " to " << next.to_str();
  CHECK(chain_);
  if (!(cur.is_masterchain_ext() && next.is_masterchain_ext() && mc_state0_->check_old_mc_block_id(cur) &&
        mc_state0_->check_old_mc_block_id(next))) {
    return fatal_error("cannot construct backward proof link from "s + cur.to_str() + " to " + next.to_str() +
                       " because these are not known masterchain block ids");
  }
  if (cur.seqno() <= next.seqno()) {
    return fatal_error("cannot construct backward proof link from "s + cur.to_str() + " to " + next.to_str());
  }
  set_continuation([this, cur, next]() { construct_proof_link_back_cont(cur, next); });
  return request_proof_link(cur) && request_mc_block_state(cur);
}

bool LiteQuery::construct_proof_link_back_cont(ton::BlockIdExt cur, ton::BlockIdExt next) {
  LOG(INFO) << "continue constructing a backward proof link from " << cur.to_str() << " to " << next.to_str();
  CHECK(mc_state_.not_null() && proof_link_.not_null() && mc_state_->get_block_id() == cur &&
        proof_link_->block_id() == cur);
  try {
    // virtualize proof link
    auto vres1 = proof_link_->get_virtual_root();
    if (vres1.is_error()) {
      return fatal_error(vres1.move_as_error());
    }
    auto vroot = vres1.ok().root;
    // adjust dest_proof and is_key of the last link of existing proof
    if (!adjust_last_proof_link(cur, vroot)) {
      return false;
    }
    // construct proof that `mc_state_` is the state of `cur`
    Ref<vm::Cell> state_proof, proof;
    if (!make_state_root_proof(proof, mc_state_->root_cell(), vroot, cur)) {
      return fatal_error("cannot construct proof for state of masterchain block "s + cur.to_str());
    }
    // construct proof that `next` is listed in OldMcBlocksInfo of `mc_state_`
    if (!make_ancestor_block_proof(state_proof, mc_state_->root_cell(), next)) {
      return fatal_error("cannot prove that "s + next.to_str() +
                         " is in the previous block set of the masterchain state of " + cur.to_str());
    }
    // create a BlockProofLink for cur -> next (without dest_proof)
    auto& link = chain_->new_link(cur, next, !next.seqno());
    link.proof = std::move(proof);
    link.state_proof = std::move(state_proof);
    // continue constructing proof chain from `next`
    return construct_proof_chain(next);
  } catch (vm::VmVirtError&) {
    return fatal_error("virtualization error during construction of backward proof link from "s + cur.to_str() +
                       " to " + next.to_str());
  }
}

bool LiteQuery::finish_proof_chain(ton::BlockIdExt id) {
  CHECK(chain_);
  LOG(INFO) << "finish constructing block proof chain from " << chain_->from.to_str() << " to " << chain_->to.to_str()
            << " (constructed " << chain_->link_count() << " up to " << id.to_str() << ")";
  try {
    if (chain_->last_link_incomplete() && chain_->last_link().to.seqno()) {
      CHECK(proof_link_.not_null() && proof_link_->block_id() == id);
      auto vres1 = proof_link_->get_virtual_root();
      if (vres1.is_error()) {
        return fatal_error(vres1.move_as_error());
      }
      if (!adjust_last_proof_link(id, vres1.ok().root)) {
        return false;
      }
    }
    chain_->complete = (id == chain_->to);
    chain_->to = id;
    // serialize answer
    std::vector<ton::tl_object_ptr<lite_api::liteServer_BlockLink>> a;
    for (auto& link : chain_->links) {
      td::BufferSlice dest_proof_boc;
      if (link.to.seqno()) {
        auto res = vm::std_boc_serialize(link.dest_proof);
        if (res.is_error()) {
          return fatal_error("error while serializing destination block Merkle proof in block proof link from "s +
                             link.from.to_str() + " to " + link.to.to_str() + " : " + res.move_as_error().to_string());
        }
        dest_proof_boc = res.move_as_ok();
      }
      auto src_proof_boc = vm::std_boc_serialize(link.proof);
      if (src_proof_boc.is_error()) {
        return fatal_error("error while serializing source block Merkle proof in block proof link from "s +
                           link.from.to_str() + " to " + link.to.to_str() + " : " +
                           src_proof_boc.move_as_error().to_string());
      }
      if (link.is_fwd) {
        // serialize forward link
        std::vector<ton::tl_object_ptr<lite_api::liteServer_signature>> b;
        for (auto& sig : link.signatures) {
          b.push_back(create_tl_object<lite_api::liteServer_signature>(sig.node, std::move(sig.signature)));
        }
        a.push_back(create_tl_object<lite_api::liteServer_blockLinkForward>(
            link.is_key, ton::create_tl_lite_block_id(link.from), ton::create_tl_lite_block_id(link.to),
            std::move(dest_proof_boc), src_proof_boc.move_as_ok(),
            create_tl_object<lite_api::liteServer_signatureSet>(link.validator_set_hash, link.cc_seqno, std::move(b))));
      } else {
        // serialize backward link
        auto state_proof_boc = vm::std_boc_serialize(link.state_proof);
        if (state_proof_boc.is_error()) {
          return fatal_error("error while serializing source state Merkle proof in block proof link from "s +
                             link.from.to_str() + " to " + link.to.to_str() + " : " +
                             state_proof_boc.move_as_error().to_string());
        }
        a.push_back(create_tl_object<lite_api::liteServer_blockLinkBack>(
            link.is_key, ton::create_tl_lite_block_id(link.from), ton::create_tl_lite_block_id(link.to),
            std::move(dest_proof_boc), src_proof_boc.move_as_ok(), state_proof_boc.move_as_ok()));
      }
    }
    LOG(INFO) << "getBlockProof() query completed";
    auto c = ton::create_serialize_tl_object<ton::lite_api::liteServer_partialBlockProof>(
        chain_->complete, ton::create_tl_lite_block_id(chain_->from), ton::create_tl_lite_block_id(chain_->to),
        std::move(a));
    return finish_query(std::move(c));
  } catch (vm::VmError& err) {
    return fatal_error("vm error while constructing block proof chain : "s + err.get_msg());
  } catch (vm::VmVirtError& err) {
    return fatal_error("virtualization error while constructing block proof chain : "s + err.get_msg());
  }
}

void LiteQuery::perform_getValidatorStats(BlockIdExt blkid, int mode, int count, Bits256 start_after,
                                          UnixTime min_utime) {
  LOG(INFO) << "started a getValidatorStats(" << blkid.to_str() << ", " << mode << ", " << count << ", "
            << start_after.to_hex() << ", " << min_utime << ") liteserver query";
  if (count <= 0) {
    fatal_error("requested entry count limit must be positive");
    return;
  }
  if ((mode & ~7) != 0) {
    fatal_error("unknown flags set in mode");
    return;
  }
  set_continuation([this, mode, count, min_utime, start_after]() {
    continue_getValidatorStats(mode, count, start_after, min_utime);
  });
  request_mc_block_data_state(blkid);
}

void LiteQuery::continue_getValidatorStats(int mode, int limit, Bits256 start_after, UnixTime min_utime) {
  LOG(INFO) << "completing getValidatorStats(" << base_blk_id_.to_str() << ", " << mode << ", " << limit << ", "
            << start_after.to_hex() << ", " << min_utime << ") liteserver query";
  Ref<vm::Cell> proof1;
  if (!make_mc_state_root_proof(proof1)) {
    return;
  }
  vm::MerkleProofBuilder mpb{mc_state_->root_cell()};
  int count;
  bool complete = false, allow_eq = (mode & 3) != 1;
  limit = std::min(limit, 1000);
  try {
    auto dict = block::get_block_create_stats_dict(mpb.root());
    if (!dict) {
      fatal_error("cannot extract block create stats from mc state");
      return;
    }
    for (count = 0; count < limit; count++) {
      auto v = dict->lookup_nearest_key(start_after, true, allow_eq);
      if (v.is_null()) {
        complete = true;
        break;
      }
      if (!block::gen::t_CreatorStats.validate_csr(std::move(v))) {
        fatal_error("invalid CreatorStats record with key "s + start_after.to_hex());
        return;
      }
      allow_eq = false;
    }
  } catch (vm::VmError& err) {
    fatal_error("error while traversing required block create stats records: "s + err.get_msg());
    return;
  }
  auto res1 = vm::std_boc_serialize(std::move(proof1));
  if (res1.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res1.move_as_error().to_string());
    return;
  }
  auto res2 = mpb.extract_proof_boc();
  if (res2.is_error()) {
    fatal_error("cannot serialize Merkle proof : "s + res2.move_as_error().to_string());
    return;
  }
  LOG(INFO) << "getValidatorStats() query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_validatorStats>(
      mode & 0xff, ton::create_tl_lite_block_id(base_blk_id_), count, complete, res1.move_as_ok(), res2.move_as_ok());
  finish_query(std::move(b));
}

void LiteQuery::perform_getShardBlockProof(BlockIdExt blkid) {
  LOG(INFO) << "started a getMasterchainInfo(" << blkid.to_str() << ") liteserver query";
  if (!blkid.is_valid_ext()) {
    fatal_error("invalid block id");
    return;
  }
  if (blkid.is_masterchain()) {
    LOG(INFO) << "getShardBlockProof() query completed";
    auto b = create_serialize_tl_object<lite_api::liteServer_shardBlockProof>(
        create_tl_lite_block_id(blkid), std::vector<tl_object_ptr<lite_api::liteServer_shardBlockLink>>());
    finish_query(std::move(b));
    return;
  }
  blk_id_ = blkid;
  get_block_handle_checked(blkid, [manager = manager_, Self = actor_id(this)](td::Result<ConstBlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
      return;
    }
    ConstBlockHandle handle = R.move_as_ok();
    if (!handle->inited_masterchain_ref_block()) {
      td::actor::send_closure(Self, &LiteQuery::abort_query, td::Status::Error("block doesn't have masterchain ref"));
      return;
    }
    AccountIdPrefixFull pfx{masterchainId, shardIdAll};
    td::actor::send_closure_later(
        manager, &ValidatorManager::get_block_by_seqno_for_litequery, pfx, handle->masterchain_ref_block(),
        [Self, manager](td::Result<ConstBlockHandle> R) {
          if (R.is_error()) {
            td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
          } else {
            ConstBlockHandle handle = R.move_as_ok();
            td::actor::send_closure_later(
                manager, &ValidatorManager::get_block_data_from_db, handle, [Self](td::Result<Ref<BlockData>> R) {
                  if (R.is_error()) {
                    td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
                  } else {
                    td::actor::send_closure_later(Self, &LiteQuery::continue_getShardBlockProof, R.move_as_ok(),
                                                  std::vector<std::pair<BlockIdExt, td::BufferSlice>>());
                  }
                });
          }
        });
  });
}

void LiteQuery::continue_getShardBlockProof(Ref<BlockData> cur_block,
                                            std::vector<std::pair<BlockIdExt, td::BufferSlice>> result) {
  BlockIdExt cur_id = cur_block->block_id();
  BlockIdExt prev_id;
  vm::MerkleProofBuilder mpb{cur_block->root_cell()};
  if (cur_id.is_masterchain()) {
    base_blk_id_ = cur_id;
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    block::gen::McBlockExtra::Record mc_extra;
    if (!tlb::unpack_cell(mpb.root(), blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
        !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
      fatal_error("cannot unpack header of block "s + cur_id.to_str());
      return;
    }
    block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());
    ShardIdFull shard_id = blk_id_.shard_full();
    shard_id.shard = (shard_id.shard & ~(1 << (63 - shard_id.pfx_len()))) | 1;
    Ref<block::McShardHash> shard_hash = shards.get_shard_hash(shard_id, false);
    if (shard_hash.is_null()) {
      fatal_error("shard not found");
      return;
    }
    prev_id = shard_hash->top_block_id();
  } else {
    std::vector<BlockIdExt> prev;
    BlockIdExt mc_blkid;
    bool after_split;
    td::Status S = block::unpack_block_prev_blk_try(mpb.root(), cur_id, prev, mc_blkid, after_split);
    if (S.is_error()) {
      fatal_error(std::move(S));
      return;
    }
    bool found = false;
    for (const BlockIdExt& id : prev) {
      if (shard_intersects(id.shard_full(), blk_id_.shard_full())) {
        found = true;
        prev_id = id;
        break;
      }
    }
    if (!found) {
      fatal_error("failed to find block chain");
      return;
    }
  }
  auto proof = mpb.extract_proof_boc();
  if (proof.is_error()) {
    fatal_error(proof.move_as_error_prefix("cannot serialize Merkle proof : "));
    return;
  }
  result.emplace_back(prev_id, proof.move_as_ok());

  if (prev_id == blk_id_) {
    CHECK(base_blk_id_.is_masterchain());
    std::vector<tl_object_ptr<lite_api::liteServer_shardBlockLink>> links;
    for (auto& p : result) {
      links.push_back(
          create_tl_object<lite_api::liteServer_shardBlockLink>(create_tl_lite_block_id(p.first), std::move(p.second)));
    }
    LOG(INFO) << "getShardBlockProof() query completed";
    auto b = create_serialize_tl_object<lite_api::liteServer_shardBlockProof>(create_tl_lite_block_id(base_blk_id_),
                                                                              std::move(links));
    finish_query(std::move(b));
    return;
  }
  if (result.size() == 8) {
    // Chains of shardblocks between masterchain blocks can't be longer than 8 (see collator.cpp:991)
    fatal_error("proof chain is too long");
    return;
  }

  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_data_from_db_short, prev_id,
      [Self = actor_id(this), result = std::move(result)](td::Result<Ref<BlockData>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::continue_getShardBlockProof, R.move_as_ok(),
                                        std::move(result));
        }
      });
}

void LiteQuery::perform_getOutMsgQueueSizes(td::optional<ShardIdFull> shard) {
  LOG(INFO) << "started a getOutMsgQueueSizes" << (shard ? shard.value().to_str() : "") << " liteserver query";
  td::actor::send_closure_later(
      manager_, &ton::validator::ValidatorManager::get_last_liteserver_state_block,
      [Self = actor_id(this), shard](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
        if (res.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, res.move_as_error());
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::continue_getOutMsgQueueSizes, shard, res.ok().first);
        }
      });
}

void LiteQuery::continue_getOutMsgQueueSizes(td::optional<ShardIdFull> shard, Ref<MasterchainState> state) {
  std::vector<BlockIdExt> blocks;
  if (!shard || shard_intersects(shard.value(), state->get_shard())) {
    blocks.push_back(state->get_block_id());
  }
  for (auto& x : state->get_shards()) {
    if (!shard || shard_intersects(shard.value(), x->shard())) {
      blocks.push_back(x->top_block_id());
    }
  }
  auto res = std::make_shared<std::vector<tl_object_ptr<lite_api::liteServer_outMsgQueueSize>>>(blocks.size());
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  for (size_t i = 0; i < blocks.size(); ++i) {
    td::actor::send_closure(manager_, &ValidatorManager::get_out_msg_queue_size, blocks[i],
                            [promise = ig.get_promise(), res, i, id = blocks[i]](td::Result<td::uint64> R) mutable {
                              TRY_RESULT_PROMISE(promise, value, std::move(R));
                              res->at(i) = create_tl_object<lite_api::liteServer_outMsgQueueSize>(
                                  create_tl_lite_block_id(id), value);
                              promise.set_value(td::Unit());
                            });
  }
  ig.add_promise([Self = actor_id(this), res](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
      return;
    }
    td::actor::send_closure(Self, &LiteQuery::finish_query,
                            create_serialize_tl_object<lite_api::liteServer_outMsgQueueSizes>(
                                std::move(*res), Collator::get_skip_externals_queue_size()),
                            false);
  });
}

void LiteQuery::perform_getBlockOutMsgQueueSize(int mode, BlockIdExt blkid) {
  LOG(INFO) << "started a getBlockOutMsgQueueSize(" << blkid.to_str() << ", " << mode << ") liteserver query";
  mode_ = mode;
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  set_continuation([=]() -> void { finish_getBlockOutMsgQueueSize(); });
  request_block_data_state(blkid);
}

void LiteQuery::finish_getBlockOutMsgQueueSize() {
  LOG(INFO) << "completing getBlockOutNsgQueueSize() query";
  bool with_proof = mode_ & 1;
  Ref<vm::Cell> state_root = state_->root_cell();
  vm::MerkleProofBuilder pb;
  if (with_proof) {
    pb = vm::MerkleProofBuilder{state_root};
    state_root = pb.root();
  }
  block::gen::ShardStateUnsplit::Record sstate;
  block::gen::OutMsgQueueInfo::Record out_msg_queue_info;
  if (!tlb::unpack_cell(state_root, sstate) || !tlb::unpack_cell(sstate.out_msg_queue_info, out_msg_queue_info)) {
    fatal_error("cannot unpack shard state");
    return;
  }
  vm::CellSlice& extra_slice = out_msg_queue_info.extra.write();
  if (extra_slice.fetch_long(1) == 0) {
    fatal_error("no out_msg_queue_size in shard state");
    return;
  }
  block::gen::OutMsgQueueExtra::Record out_msg_queue_extra;
  if (!tlb::unpack(extra_slice, out_msg_queue_extra)) {
    fatal_error("cannot unpack OutMsgQueueExtra");
    return;
  }
  vm::CellSlice& size_slice = out_msg_queue_extra.out_queue_size.write();
  if (size_slice.fetch_long(1) == 0) {
    fatal_error("no out_msg_queue_size in shard state");
    return;
  }
  td::uint64 size = size_slice.prefetch_ulong(48);

  td::BufferSlice proof;
  if (with_proof) {
    Ref<vm::Cell> proof1, proof2;
    if (!make_state_root_proof(proof1)) {
      return;
    }
    if (!pb.extract_proof_to(proof2)) {
      fatal_error("unknown error creating Merkle proof");
      return;
    }
    auto r_proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
    if (r_proof.is_error()) {
      fatal_error(r_proof.move_as_error());
      return;
    }
    proof = r_proof.move_as_ok();
  }
  LOG(INFO) << "getBlockOutMsgQueueSize(" << blk_id_.to_str() << ", " << mode_ << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_blockOutMsgQueueSize>(
      mode_, ton::create_tl_lite_block_id(blk_id_), size, std::move(proof));
  finish_query(std::move(b));
}

void LiteQuery::perform_getDispatchQueueInfo(int mode, BlockIdExt blkid, StdSmcAddress after_addr, int max_accounts) {
  LOG(INFO) << "started a getDispatchQueueInfo(" << blkid.to_str() << ", " << mode << ") liteserver query";
  mode_ = mode;
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  if (max_accounts <= 0) {
    fatal_error("invalid max_accounts");
    return;
  }
  set_continuation([=]() -> void { finish_getDispatchQueueInfo(after_addr, max_accounts); });
  request_block_data_state(blkid);
}

void LiteQuery::finish_getDispatchQueueInfo(StdSmcAddress after_addr, int max_accounts) {
  LOG(INFO) << "completing getDispatchQueueInfo() query";
  bool with_proof = mode_ & 1;
  Ref<vm::Cell> state_root = state_->root_cell();
  vm::MerkleProofBuilder pb;
  if (with_proof) {
    pb = vm::MerkleProofBuilder{state_root};
    state_root = pb.root();
  }

  std::unique_ptr<vm::AugmentedDictionary> dispatch_queue;

  block::gen::ShardStateUnsplit::Record sstate;
  block::gen::OutMsgQueueInfo::Record out_msg_queue_info;
  block::gen::OutMsgQueueExtra::Record out_msg_queue_extra;
  if (!tlb::unpack_cell(state_root, sstate) || !tlb::unpack_cell(sstate.out_msg_queue_info, out_msg_queue_info)) {
    fatal_error("cannot unpack shard state");
    return;
  }
  vm::CellSlice& extra_slice = out_msg_queue_info.extra.write();
  if (extra_slice.fetch_long(1)) {
    if (!tlb::unpack(extra_slice, out_msg_queue_extra)) {
      fatal_error("cannot unpack OutMsgQueueExtra");
      return;
    }
    dispatch_queue = std::make_unique<vm::AugmentedDictionary>(out_msg_queue_extra.dispatch_queue, 256,
                                                               block::tlb::aug_DispatchQueue);
  } else {
    dispatch_queue = std::make_unique<vm::AugmentedDictionary>(256, block::tlb::aug_DispatchQueue);
  }

  int remaining = std::min(max_accounts, 64);
  bool complete = false;
  std::vector<tl_object_ptr<lite_api::liteServer_accountDispatchQueueInfo>> result;
  bool allow_eq;
  if (mode_ & 2) {
    allow_eq = false;
  } else {
    allow_eq = true;
    after_addr = td::Bits256::zero();
  }
  while (true) {
    auto value = dispatch_queue->extract_value(dispatch_queue->lookup_nearest_key(after_addr, true, allow_eq));
    allow_eq = false;
    if (value.is_null()) {
      complete = true;
      break;
    }
    if (remaining == 0) {
      break;
    }
    --remaining;
    StdSmcAddress addr = after_addr;
    vm::Dictionary dict{64};
    td::uint64 dict_size;
    if (!block::unpack_account_dispatch_queue(value, dict, dict_size)) {
      fatal_error(PSTRING() << "invalid account dispatch queue for account " << addr.to_hex());
      return;
    }
    CHECK(dict_size > 0);
    td::BitArray<64> min_lt, max_lt;
    dict.get_minmax_key(min_lt.bits(), 64, false, false);
    dict.get_minmax_key(max_lt.bits(), 64, true, false);
    result.push_back(create_tl_object<lite_api::liteServer_accountDispatchQueueInfo>(addr, dict_size, min_lt.to_ulong(),
                                                                                     max_lt.to_ulong()));
  }

  td::BufferSlice proof;
  if (with_proof) {
    Ref<vm::Cell> proof1, proof2;
    if (!make_state_root_proof(proof1)) {
      return;
    }
    if (!pb.extract_proof_to(proof2)) {
      fatal_error("unknown error creating Merkle proof");
      return;
    }
    auto r_proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
    if (r_proof.is_error()) {
      fatal_error(r_proof.move_as_error());
      return;
    }
    proof = r_proof.move_as_ok();
  }
  LOG(INFO) << "getDispatchQueueInfo(" << blk_id_.to_str() << ", " << mode_ << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_dispatchQueueInfo>(
      mode_, ton::create_tl_lite_block_id(blk_id_), std::move(result), complete, std::move(proof));
  finish_query(std::move(b));
}

void LiteQuery::perform_getDispatchQueueMessages(int mode, BlockIdExt blkid, StdSmcAddress addr, LogicalTime lt,
                                                 int max_messages) {
  LOG(INFO) << "started a getDispatchQueueMessages(" << blkid.to_str() << ", " << mode << ") liteserver query";
  mode_ = mode;
  if (!blkid.is_valid_full()) {
    fatal_error("invalid BlockIdExt");
    return;
  }
  if (max_messages <= 0) {
    fatal_error("invalid max_messages");
    return;
  }
  set_continuation([=]() -> void { finish_getDispatchQueueMessages(addr, lt, max_messages); });
  request_block_data_state(blkid);
}

void LiteQuery::finish_getDispatchQueueMessages(StdSmcAddress addr, LogicalTime lt, int max_messages) {
  LOG(INFO) << "completing getDispatchQueueMessages() query";
  bool with_proof = mode_ & lite_api::liteServer_getDispatchQueueMessages::WANT_PROOF_MASK;
  bool one_account = mode_ & lite_api::liteServer_getDispatchQueueMessages::ONE_ACCOUNT_MASK;
  bool with_messages_boc = mode_ & lite_api::liteServer_getDispatchQueueMessages::MESSAGES_BOC_MASK;
  Ref<vm::Cell> state_root = state_->root_cell();
  vm::MerkleProofBuilder pb;
  if (with_proof) {
    pb = vm::MerkleProofBuilder{state_root};
    state_root = pb.root();
  }

  std::unique_ptr<vm::AugmentedDictionary> dispatch_queue;

  block::gen::ShardStateUnsplit::Record sstate;
  block::gen::OutMsgQueueInfo::Record out_msg_queue_info;
  block::gen::OutMsgQueueExtra::Record out_msg_queue_extra;
  if (!tlb::unpack_cell(state_root, sstate) || !tlb::unpack_cell(sstate.out_msg_queue_info, out_msg_queue_info)) {
    fatal_error("cannot unpack shard state");
    return;
  }
  vm::CellSlice& extra_slice = out_msg_queue_info.extra.write();
  if (extra_slice.fetch_long(1)) {
    if (!tlb::unpack(extra_slice, out_msg_queue_extra)) {
      fatal_error("cannot unpack OutMsgQueueExtra");
      return;
    }
    dispatch_queue = std::make_unique<vm::AugmentedDictionary>(out_msg_queue_extra.dispatch_queue, 256,
                                                               block::tlb::aug_DispatchQueue);
  } else {
    dispatch_queue = std::make_unique<vm::AugmentedDictionary>(256, block::tlb::aug_DispatchQueue);
  }

  int remaining = std::min(max_messages, with_messages_boc ? 16 : 64);
  bool complete = false;
  std::vector<tl_object_ptr<lite_api::liteServer_dispatchQueueMessage>> result;
  std::vector<td::Ref<vm::Cell>> message_roots;
  td::Bits256 orig_addr = addr;
  bool first = true;
  while (remaining > 0) {
    auto value = dispatch_queue->extract_value(dispatch_queue->lookup_nearest_key(addr, true, first));
    if (value.is_null() || (one_account && addr != orig_addr)) {
      complete = true;
      break;
    }
    vm::Dictionary account_queue{64};
    td::uint64 dict_size;
    if (!block::unpack_account_dispatch_queue(value, account_queue, dict_size)) {
      fatal_error(PSTRING() << "invalid account dispatch queue for account " << addr.to_hex());
      return;
    }
    CHECK(dict_size > 0);
    while (true) {
      td::BitArray<64> lt_key;
      lt_key.store_ulong(lt);
      auto value2 = account_queue.lookup_nearest_key(lt_key, true, false);
      if (value2.is_null()) {
        break;
      }
      lt = lt_key.to_ulong();
      if (remaining == 0) {
        break;
      }
      --remaining;
      auto msg_env = value2->prefetch_ref();
      block::tlb::MsgEnvelope::Record_std env;
      if (msg_env.is_null() || !tlb::unpack_cell(msg_env, env)) {
        fatal_error(PSTRING() << "invalid message in dispatch queue for account " << addr.to_hex() << ", lt " << lt);
        return;
      }
      message_roots.push_back(env.msg);
      tl_object_ptr<lite_api::liteServer_transactionMetadata> metadata_tl;
      if (env.metadata) {
        auto& metadata = env.metadata.value();
        metadata_tl = create_tl_object<lite_api::liteServer_transactionMetadata>(
            0, metadata.depth,
            create_tl_object<lite_api::liteServer_accountId>(metadata.initiator_wc, metadata.initiator_addr),
            metadata.initiator_lt);
      } else {
        metadata_tl = create_tl_object<lite_api::liteServer_transactionMetadata>(
            0, -1, create_tl_object<lite_api::liteServer_accountId>(workchainInvalid, td::Bits256::zero()), -1);
      }
      result.push_back(create_tl_object<lite_api::liteServer_dispatchQueueMessage>(addr, lt, env.msg->get_hash().bits(),
                                                                                   std::move(metadata_tl)));
    }
    first = false;
    lt = 0;
  }

  td::BufferSlice proof;
  if (with_proof) {
    Ref<vm::Cell> proof1, proof2;
    if (!make_state_root_proof(proof1)) {
      return;
    }
    if (!pb.extract_proof_to(proof2)) {
      fatal_error("unknown error creating Merkle proof");
      return;
    }
    auto r_proof = vm::std_boc_serialize_multi({std::move(proof1), std::move(proof2)});
    if (r_proof.is_error()) {
      fatal_error(r_proof.move_as_error());
      return;
    }
    proof = r_proof.move_as_ok();
  }
  td::BufferSlice messages_boc;
  if (with_messages_boc) {
    auto r_messages_boc = vm::std_boc_serialize_multi(std::move(message_roots));
    if (r_messages_boc.is_error()) {
      fatal_error(r_messages_boc.move_as_error());
      return;
    }
    messages_boc = r_messages_boc.move_as_ok();
  }
  LOG(INFO) << "getDispatchQueueMessages(" << blk_id_.to_str() << ", " << mode_ << ") query completed";
  auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_dispatchQueueMessages>(
      mode_, ton::create_tl_lite_block_id(blk_id_), std::move(result), complete, std::move(proof),
      std::move(messages_boc));
  finish_query(std::move(b));
}

void LiteQuery::perform_nonfinal_getCandidate(td::Bits256 source, BlockIdExt blkid, td::Bits256 collated_data_hash) {
  LOG(INFO) << "started a nonfinal.getCandidate liteserver query";
  td::actor::send_closure_later(
      manager_, &ValidatorManager::get_block_candidate_for_litequery, PublicKey{pubkeys::Ed25519{source}}, blkid, collated_data_hash,
      [Self = actor_id(this)](td::Result<BlockCandidate> R) {
        if (R.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        } else {
          BlockCandidate cand = R.move_as_ok();
          td::actor::send_closure_later(
              Self, &LiteQuery::finish_query,
              create_serialize_tl_object<lite_api::liteServer_nonfinal_candidate>(
                  create_tl_object<lite_api::liteServer_nonfinal_candidateId>(
                      create_tl_lite_block_id(cand.id), cand.pubkey.as_bits256(), cand.collated_file_hash),
                  std::move(cand.data), std::move(cand.collated_data)),
              false);
        }
      });
}

void LiteQuery::perform_nonfinal_getValidatorGroups(int mode, ShardIdFull shard) {
  bool with_shard = mode & 1;
  LOG(INFO) << "started a nonfinal.getValidatorGroups" << (with_shard ? shard.to_str() : "(all)")
            << " liteserver query";
  td::optional<ShardIdFull> maybe_shard;
  if (with_shard) {
    maybe_shard = shard;
  }
  td::actor::send_closure(
      manager_, &ValidatorManager::get_validator_groups_info_for_litequery, maybe_shard,
      [Self = actor_id(this)](td::Result<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> R) {
        if (R.is_error()) {
          td::actor::send_closure(Self, &LiteQuery::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure_later(Self, &LiteQuery::finish_query, serialize_tl_object(R.move_as_ok(), true),
                                        false);
        }
      });
}

}  // namespace validator
}  // namespace ton
