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
#include "accept-block.hpp"
#include "adnl/utils.hpp"
#include "interfaces/validator-manager.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

#include "fabric.h"
#include "top-shard-descr.hpp"

#include "vm/cells.h"
#include "vm/cells/MerkleProof.h"
#include "vm/boc.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"

#include "validator/invariants.hpp"

namespace ton {

namespace validator {
using namespace std::literals::string_literals;

AcceptBlockQuery::AcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                   td::Ref<ValidatorSet> validator_set, td::Ref<BlockSignatureSet> signatures,
                                   td::Ref<BlockSignatureSet> approve_signatures, bool send_broadcast,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , validator_set_(std::move(validator_set))
    , signatures_(std::move(signatures))
    , approve_signatures_(std::move(approve_signatures))
    , is_fake_(false)
    , is_fork_(false)
    , send_broadcast_(send_broadcast)
    , manager_(manager)
    , promise_(std::move(promise))
    , perf_timer_("acceptblock", 0.1, [manager](double duration) {
        send_closure(manager, &ValidatorManager::add_perf_timer_stat, "acceptblock", duration);
      }) {
  state_keep_old_hash_.clear();
  state_old_hash_.clear();
  state_hash_.clear();
  CHECK(prev_.size() > 0);
}

AcceptBlockQuery::AcceptBlockQuery(AcceptBlockQuery::IsFake fake, BlockIdExt id, td::Ref<BlockData> data,
                                   std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , validator_set_(std::move(validator_set))
    , is_fake_(true)
    , is_fork_(false)
    , send_broadcast_(false)
    , manager_(manager)
    , promise_(std::move(promise))
    , perf_timer_("acceptblock", 0.1, [manager](double duration) {
        send_closure(manager, &ValidatorManager::add_perf_timer_stat, "acceptblock", duration);
      }) {
  state_keep_old_hash_.clear();
  state_old_hash_.clear();
  state_hash_.clear();
  CHECK(prev_.size() > 0);
}

AcceptBlockQuery::AcceptBlockQuery(ForceFork ffork, BlockIdExt id, td::Ref<BlockData> data,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , is_fake_(true)
    , is_fork_(true)
    , send_broadcast_(false)
    , manager_(manager)
    , promise_(std::move(promise))
    , perf_timer_("acceptblock", 0.1, [manager](double duration) {
        send_closure(manager, &ValidatorManager::add_perf_timer_stat, "acceptblock", duration);
      }) {
  state_keep_old_hash_.clear();
  state_old_hash_.clear();
  state_hash_.clear();
}

bool AcceptBlockQuery::precheck_header() {
  VLOG(VALIDATOR_DEBUG) << "precheck_header()";
  // 0. sanity check
  CHECK(data_.not_null());
  block_root_ = data_->root_cell();
  if (data_->block_id() != id_) {
    return fatal_error("incorrect block id in block data: "s + data_->block_id().to_str() + " instead of " +
                       id_.to_str());
  }
  // 1. root hash and file hash check
  RootHash blk_rhash{block_root_->get_hash().bits()};
  if (blk_rhash != id_.root_hash) {
    return fatal_error("block root hash mismatch: expected "s + id_.root_hash.to_hex() + ", found " +
                       blk_rhash.to_hex());
  }
  if (is_fake_ || is_fork_) {
    FileHash blk_fhash;
    td::sha256(data_->data().as_slice(), blk_fhash.as_slice());
    if (blk_fhash != id_.file_hash) {
      return fatal_error("block file hash mismatch: expected "s + id_.file_hash.to_hex() + ", computed " +
                         blk_fhash.to_hex());
    }
  }
  // 2. check header fields
  std::vector<ton::BlockIdExt> prev;
  ton::BlockIdExt mc_blkid;
  bool after_split;
  auto res = block::unpack_block_prev_blk_try(block_root_, id_, prev, mc_blkid, after_split);
  if (res.is_error()) {
    return fatal_error("invalid block header in AcceptBlock: "s + res.to_string());
  }
  if (is_fork_) {
    prev_ = prev;
  } else if (prev_ != prev) {
    return fatal_error("invalid previous block reference(s) in block header");
  }
  // 3. unpack header and check vert_seqno fields
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(block_root_, blk) && tlb::unpack_cell(blk.info, info))) {
    return fatal_error("cannot unpack block header");
  }
  if (info.vert_seqno_incr && !is_fork_) {
    return fatal_error("block header has vert_seqno_incr set in an ordinary AcceptBlock");
  }
  if (!info.vert_seqno_incr && is_fork_) {
    return fatal_error("fork block header has no vert_seqno_incr");
  }
  if (is_fork_ && !info.key_block) {
    return fatal_error("fork block is not a key block");
  }
  return true;
}

bool AcceptBlockQuery::create_new_proof() {
  // 0. check block's root hash
  VLOG(VALIDATOR_DEBUG) << "create_new_proof() : start";
  RootHash blk_rhash{block_root_->get_hash().bits()};
  if (blk_rhash != id_.root_hash) {
    return fatal_error("block root hash mismatch: expected "s + id_.root_hash.to_hex() + ", found " +
                       blk_rhash.to_hex());
  }
  // 1. visit block header while building a Merkle proof
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root_, usage_tree->root_ptr());
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  block::gen::ExtBlkRef::Record mcref{};  // _ ExtBlkRef = BlkMasterInfo;
  block::CurrencyCollection fees;
  ShardIdFull shard;
  if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) &&
        block::gen::BlkPrevInfo{info.after_merge}.validate_ref(info.prev_ref) &&
        tlb::unpack_cell(std::move(blk.extra), extra) && block::gen::t_ValueFlow.force_validate_ref(blk.value_flow) &&
        (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)))) {
    return fatal_error("cannot unpack block header");
  }
  is_key_block_ = info.key_block;
  // 2. check some header fields, especially shard
  if (info.not_master != !shard.is_masterchain()) {
    return fatal_error("block has invalid not_master flag in its header");
  }
  BlockId blk_id{shard, (unsigned)info.seq_no};
  if (blk_id != id_.id) {
    return fatal_error("block header corresponds to another block id: expected "s + id_.id.to_str() + ", found " +
                       blk_id.to_str());
  }
  if (info.after_merge + 1U != prev_.size()) {
    return fatal_error(PSTRING() << "block header of " << id_.to_str() << " announces " << info.after_merge + 1
                                 << " previous blocks, but " << prev_.size() << " are actually present");
  }
  if (is_masterchain() && (info.after_merge | info.after_split | info.before_split)) {
    return fatal_error("masterchain block header of "s + id_.to_str() + " announces merge/split in its header");
  }
  if (!is_masterchain() && is_key_block_) {
    return fatal_error("non-masterchain block header of "s + id_.to_str() + " announces this block to be a key block");
  }
  // 3. check state update
  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  // 4. visit validator-set related fields in key blocks
  if (is_key_block_) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && mc_extra.key_block &&
          mc_extra.config.not_null())) {
      return fatal_error("cannot unpack extra header of key masterchain block "s + blk_id.to_str());
    }
    auto cfg = block::Config::unpack_config(std::move(mc_extra.config));
    if (cfg.is_error()) {
      return fatal_error("cannot extract configuration from extra header of key masterchain block "s + blk_id.to_str() +
                         " : " + cfg.move_as_error().to_string());
    }
    auto res = cfg.move_as_ok()->visit_validator_params();
    if (res.is_error()) {
      return fatal_error("cannot extract validator set configuration from extra header of key masterchain block "s +
                         blk_id.to_str() + " : " + res.to_string());
    }
  }
  // 5. finish constructing Merkle proof from visited cells
  auto proof = vm::MerkleProof::generate(block_root_, usage_tree.get());
  proof_roots_.push_back(proof);
  // 6. extract some information from state update
  state_old_hash_ = upd_cs.prefetch_ref(0)->get_hash(0).bits();
  state_hash_ = upd_cs.prefetch_ref(1)->get_hash(0).bits();
  lt_ = info.end_lt;
  created_at_ = info.gen_utime;
  if (!is_masterchain()) {
    mc_blkid_.id = BlockId{masterchainId, shardIdAll, mcref.seq_no};
    mc_blkid_.root_hash = mcref.root_hash;
    mc_blkid_.file_hash = mcref.file_hash;
  } else if (!is_key_block_) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && !mc_extra.key_block)) {
      return fatal_error("extra header of non-key masterchain block "s + blk_id.to_str() +
                         " is invalid or contains extra information reserved for key blocks only");
    }
  }
  // 7. check signatures
  td::Result<td::uint64> sign_chk;
  if (!is_fake_) {
    sign_chk = validator_set_->check_signatures(id_.root_hash, id_.file_hash, signatures_);
    if (sign_chk.is_error()) {
      auto err = sign_chk.move_as_error();
      VLOG(VALIDATOR_WARNING) << "signature check failed : " << err.to_string();
      abort_query(std::move(err));
      return false;
    }
  }
  // 8. serialize signatures
  if (!is_fake_) {
    vm::CellBuilder cb2;
    Ref<vm::Cell> sign_cell;
    if (!(cb2.store_long_bool(0x11, 8)  // block_signatures#11
          && cb2.store_long_bool(validator_set_->get_validator_set_hash(),
                                 32)  // validator_info$_ validator_set_hash_short:uint32
          && cb2.store_long_bool(validator_set_->get_catchain_seqno(),
                                 32)                         //   validator_set_ts:uint32 = ValidatorInfo
          && cb2.store_long_bool(signatures_->size(), 32)    // sig_count:uint32
          && cb2.store_long_bool(sign_chk.move_as_ok(), 64)  // sig_weight:uint32
          && signatures_->serialize_to(sign_cell)            // (HashmapE 16 CryptoSignaturePair)
          && cb2.store_maybe_ref(std::move(sign_cell)) && cb2.finalize_to(signatures_cell_))) {
      return fatal_error("cannot serialize BlockSignatures for the newly-accepted block");
    }
  } else {  // FAKE
    vm::CellBuilder cb2;
    if (!(cb2.store_long_bool(0x11, 8)  // block_signatures#11
          && cb2.store_long_bool(validator_set_.not_null() ? validator_set_->get_validator_set_hash() : 0,
                                 32)  // validator_info$_ validator_set_hash_short:uint32
          && cb2.store_long_bool(validator_set_.not_null() ? validator_set_->get_catchain_seqno() : 0,
                                 32)     //   validator_set_ts:uint32 = ValidatorInfo
          && cb2.store_long_bool(0, 32)  // sig_count:uint32
          && cb2.store_long_bool(0, 64)  // sig_weight:uint32
          && cb2.store_bool_bool(false)  // (HashmapE 16 CryptoSignaturePair)
          && cb2.finalize_to(signatures_cell_))) {
      return fatal_error("cannot serialize fake BlockSignatures for the newly-accepted block");
    }
  }
  Ref<vm::Cell> bs_cell;
  if (is_masterchain()) {
    // 9a. now create serialized proof
    vm::CellBuilder cb;
    if (!(cb.store_long_bool(0xc3, 8)                // block_proof#c3
          && block::tlb::t_BlockIdExt.pack(cb, id_)  // proof_for:BlockIdExt
          && cb.store_ref_bool(std::move(proof))     // proof:^Cell
          && cb.store_bool_bool(true)                // signatures:(Maybe
          && cb.store_ref_bool(signatures_cell_)     //   ^BlockSignatures)
          && cb.finalize_to(bs_cell))) {
      return fatal_error("cannot serialize BlockProof for the newly-accepted block");
    }
  } else {
    // 9b. create serialized proof (link)
    vm::CellBuilder cb;
    if (!(cb.store_long_bool(0xc3, 8)                // block_proof#c3
          && block::tlb::t_BlockIdExt.pack(cb, id_)  // proof_for:BlockIdExt
          && cb.store_ref_bool(std::move(proof))     // proof:^Cell
          && cb.store_bool_bool(false)               // signatures:(Maybe ^BlockSignatures)
          && cb.finalize_to(bs_cell))) {
      return fatal_error("cannot serialize BlockProof for the newly-accepted block");
    }
  }
  // 10. check resulting object
  if (!block::gen::t_BlockProof.validate_ref(bs_cell)) {
    block::gen::t_BlockProof.print_ref(std::cerr, bs_cell);
    vm::load_cell_slice(bs_cell).print_rec(std::cerr);
    return fatal_error("BlockProof object just created failed to pass automated consistency checks");
  }
  // 11. create a proof object from this cell
  if (is_masterchain()) {
    proof_ = create_proof(id_, vm::std_boc_serialize(bs_cell, 0).move_as_ok()).move_as_ok();
  } else {
    proof_link_ = create_proof_link(id_, vm::std_boc_serialize(bs_cell, 0).move_as_ok()).move_as_ok();
  }
  VLOG(VALIDATOR_DEBUG) << "create_new_proof() : end";
  return true;
}

void AcceptBlockQuery::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "aborting accept block query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

bool AcceptBlockQuery::fatal_error(std::string msg, int code) {
  abort_query(td::Status::Error(code, std::move(msg)));
  return false;
}

bool AcceptBlockQuery::check_send_error(td::actor::ActorId<AcceptBlockQuery> SelfId, td::Status error) {
  if (error.is_error()) {
    td::actor::send_closure(std::move(SelfId), &AcceptBlockQuery::abort_query, std::move(error));
    return true;
  } else {
    return false;
  }
}

void AcceptBlockQuery::finish_query() {
  ValidatorInvariants::check_post_accept(handle_);
  if (is_masterchain()) {
    CHECK(handle_->inited_proof());
  } else {
    CHECK(handle_->inited_proof_link());
  }
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void AcceptBlockQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void AcceptBlockQuery::start_up() {
  VLOG(VALIDATOR_DEBUG) << "start_up()";
  alarm_timestamp() = timeout_;

  if (!is_fork_ && validator_set_.is_null()) {
    fatal_error("no real ValidatorSet passed to AcceptBlockQuery");
    return;
  }
  if (!is_fake_ && signatures_.is_null()) {
    fatal_error("no real SignatureSet passed to AcceptBlockQuery");
    return;
  }
  if (!is_fake_ && is_fork_) {
    fatal_error("a non-fake AcceptBlockQuery for a forced fork block");
    return;
  }
  if (!is_fork_ && !prev_.size()) {
    fatal_error("no previous blocks passed to AcceptBlockQuery");
    return;
  }
  if (is_fork_ && !is_masterchain()) {
    fatal_error("cannot accept a non-masterchain fork block");
    return;
  }
  if (is_fork_ && data_.is_null()) {
    fatal_error("cannot accept a fork block without explicit data");
    return;
  }
  if (data_.not_null() && !precheck_header()) {
    fatal_error("invalid block header in AcceptBlock");
    return;
  }

  td::actor::send_closure(
      manager_, &ValidatorManager::get_block_handle, id_, true, [SelfId = actor_id(this)](td::Result<BlockHandle> R) {
        check_send_error(SelfId, R) ||
            td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_block_handle, R.move_as_ok());
      });
}

void AcceptBlockQuery::got_block_handle(BlockHandle handle) {
  VLOG(VALIDATOR_DEBUG) << "got_block_handle()";
  handle_ = std::move(handle);
  if (handle_->received() && handle_->received_state() && handle_->inited_signatures() &&
      handle_->inited_split_after() && handle_->inited_merge_before() && handle_->inited_prev() &&
      handle_->inited_logical_time() && handle_->inited_state_root_hash() &&
      (is_masterchain() ? handle_->inited_proof() && handle_->is_applied() && handle_->inited_is_key_block()
                        : handle_->inited_proof_link())) {
    finish_query();
    return;
  }
  if (data_.not_null() && !handle_->received()) {
    td::actor::send_closure(
        manager_, &ValidatorManager::set_block_data, handle_, data_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
          check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_data);
        });
  } else {
    written_block_data();
  }
}

void AcceptBlockQuery::written_block_data() {
  VLOG(VALIDATOR_DEBUG) << "written_block_data()";
  if (handle_->inited_signatures()) {
    written_block_signatures();
    return;
  }
  if (is_fake_) {
    signatures_ = Ref<BlockSignatureSetQ>(create_signature_set(std::vector<BlockSignature>{}));
  }
  td::actor::send_closure(manager_, &ValidatorManager::set_block_signatures, handle_, signatures_,
                          [SelfId = actor_id(this)](td::Result<td::Unit> R) {
                            check_send_error(SelfId, R) ||
                                td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_signatures);
                          });
}

void AcceptBlockQuery::written_block_signatures() {
  VLOG(VALIDATOR_DEBUG) << "written_block_signatures()";
  handle_->set_merge(prev_.size() == 2);

  for (auto& p : prev_) {
    handle_->set_prev(p);
  }

  if (handle_->need_flush()) {
    handle_->flush(manager_, handle_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
      check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_info);
    });
  } else {
    written_block_info();
  }
}

void AcceptBlockQuery::written_block_info() {
  VLOG(VALIDATOR_DEBUG) << "written block info";
  if (data_.not_null()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      check_send_error(SelfId, R) ||
          td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_prev_state, R.move_as_ok());
    });

    td::actor::send_closure(manager_, &ValidatorManager::wait_prev_block_state, handle_, priority(), timeout_,
                            std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::wait_block_data, handle_, priority(), timeout_,
                            [SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
                              check_send_error(SelfId, R) ||
                                  td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_block_data,
                                                               R.move_as_ok());
                            });
  }
}

void AcceptBlockQuery::got_block_data(td::Ref<BlockData> data) {
  VLOG(VALIDATOR_DEBUG) << "got_block_data()";
  data_ = std::move(data);
  CHECK(data_.not_null());
  if (data_->root_cell().is_null()) {
    fatal_error("block data does not contain a root cell");
    return;
  }
  if (!precheck_header()) {
    fatal_error("invalid block header in AcceptBlock");
    return;
  }
  if (handle_->received()) {
    written_block_data();
  } else {
    td::actor::send_closure(
        manager_, &ValidatorManager::set_block_data, handle_, data_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
          check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_data);
        });
  }
}

void AcceptBlockQuery::got_prev_state(td::Ref<ShardState> state) {
  VLOG(VALIDATOR_DEBUG) << "got prev state";
  state_ = std::move(state);

  state_keep_old_hash_ = state_->root_hash();

  auto err = state_.write().apply_block(id_, data_);
  if (err.is_error()) {
    abort_query(std::move(err));
    return;
  }

  handle_->set_split(state_->before_split());

  td::actor::send_closure(manager_, &ValidatorManager::set_block_state, handle_, state_,
                          [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                            check_send_error(SelfId, R) ||
                                td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_state, R.move_as_ok());
                          });
}

void AcceptBlockQuery::written_state(td::Ref<ShardState> upd_state) {
  VLOG(VALIDATOR_DEBUG) << "written state";
  CHECK(data_.not_null());
  state_ = std::move(upd_state);

  block_root_ = data_->root_cell();
  if (block_root_.is_null()) {
    fatal_error("block data does not contain a root cell");
    return;
  }
  // generate proof
  if (!create_new_proof()) {
    fatal_error("cannot generate proof for block "s + id_.to_str());
    return;
  }

  if (state_keep_old_hash_ != state_old_hash_) {
    fatal_error(PSTRING() << "invalid previous state hash in newly-created proof: expected "
                          << state_->root_hash().to_hex() << ", found in update " << state_old_hash_.to_hex());
    return;
  }

  //handle_->set_masterchain_block(prev_[0]);
  handle_->set_state_root_hash(state_hash_);
  handle_->set_logical_time(lt_);
  handle_->set_unix_time(created_at_);
  handle_->set_is_key_block(is_key_block_);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_proof);
  });

  if (is_masterchain()) {
    td::actor::send_closure(manager_, &ValidatorManager::set_block_proof, handle_, proof_, std::move(P));
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::set_block_proof_link, handle_, proof_link_, std::move(P));
  }
}

void AcceptBlockQuery::written_block_proof() {
  VLOG(VALIDATOR_DEBUG) << "written_block_proof()";
  if (!is_masterchain()) {
    td::actor::send_closure(manager_, &ValidatorManager::get_top_masterchain_state_block,
                            [SelfId = actor_id(this)](td::Result<std::pair<td::Ref<MasterchainState>, BlockIdExt>> R) {
                              check_send_error(SelfId, R) ||
                                  td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_last_mc_block,
                                                               R.move_as_ok());
                            });
    return;
  }
  CHECK(prev_.size() == 1);

  td::actor::send_closure(
      manager_, &ValidatorManager::set_next_block, prev_[0], id_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
        check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_next);
      });
}

void AcceptBlockQuery::got_last_mc_block(std::pair<td::Ref<MasterchainState>, BlockIdExt> last) {
  VLOG(VALIDATOR_DEBUG) << "got_last_mc_block(): " << last.second.to_str();
  last_mc_state_ = Ref<MasterchainStateQ>(std::move(last.first));
  last_mc_id_ = std::move(last.second);
  CHECK(last_mc_state_.not_null());
  if (last_mc_id_.id.seqno < mc_blkid_.id.seqno) {
    VLOG(VALIDATOR_DEBUG) << "shardchain block refers to newer masterchain block " << mc_blkid_.to_str()
                          << ", trying to obtain it";
    td::actor::send_closure_later(manager_, &ValidatorManager::wait_block_state_short, mc_blkid_, priority(), timeout_,
                                  [SelfId = actor_id(this)](td::Result<Ref<ShardState>> R) {
                                    check_send_error(SelfId, R) ||
                                        td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_mc_state,
                                                                     R.move_as_ok());
                                  });
    return;
  } else if (last_mc_id_.id.seqno > mc_blkid_.id.seqno) {
    if (!last_mc_state_->check_old_mc_block_id(mc_blkid_)) {
      fatal_error("shardchain block refers to masterchain block "s + mc_blkid_.to_str() +
                  " which is not a antecessor of last masterchain block " + last_mc_id_.to_str());
      return;
    }
  } else if (last_mc_id_ != mc_blkid_) {
    fatal_error("shardchain block refers to masterchain block "s + mc_blkid_.to_str() +
                " distinct from last masterchain block " + last_mc_id_.to_str() + " of the same height");
    return;
  }
  find_known_ancestors();
}

void AcceptBlockQuery::got_mc_state(Ref<ShardState> res) {
  VLOG(VALIDATOR_DEBUG) << "got_mc_state()";
  auto new_state = Ref<MasterchainStateQ>(std::move(res));
  CHECK(new_state.not_null());
  if (!new_state->check_old_mc_block_id(last_mc_id_)) {
    fatal_error("shardchain block refers to masterchain block "s + mc_blkid_.to_str() +
                " which is not a successor of last masterchain block " + last_mc_id_.to_str());
    return;
  }
  last_mc_id_ = mc_blkid_;
  last_mc_state_ = std::move(new_state);
  find_known_ancestors();
}

void AcceptBlockQuery::find_known_ancestors() {
  VLOG(VALIDATOR_DEBUG) << "find_known_ancestors()";
  prev_mc_blkid_ = mc_blkid_;
  auto config = last_mc_state_->get_config();
  CHECK(config);
  auto shard = ton::ShardIdFull(id_);
  auto ancestor = config->get_shard_hash(shard, false);
  if (ancestor.is_null()) {
    ancestor = config->get_shard_hash(ton::shard_child(shard, true));
    auto ancestor2 = config->get_shard_hash(ton::shard_child(shard, false));
    if (ancestor.is_null() || ancestor2.is_null()) {
      VLOG(VALIDATOR_WARNING) << " cannot retrieve information about shard " + shard.to_str() +
                                     " from masterchain block " + last_mc_id_.to_str() +
                                     ", skipping ShardTopBlockDescr creation";
      if (last_mc_id_.id.seqno <= mc_blkid_.id.seqno) {
        fatal_error(" cannot retrieve information about shard "s + shard.to_str() + " from masterchain block " +
                    last_mc_id_.to_str());
        return;
      }
      written_block_next();
      return;
    }
    VLOG(VALIDATOR_DEBUG) << "found two ancestors: " << ancestor->blk_.to_str() << " and " << ancestor2->blk_.to_str();
    ancestors_seqno_ = std::max(ancestor->blk_.id.seqno, ancestor2->blk_.id.seqno);
    ancestors_.emplace_back(std::move(ancestor));
    ancestors_.emplace_back(std::move(ancestor2));
  } else if (ancestor->shard() == shard) {
    VLOG(VALIDATOR_DEBUG) << "found one regular ancestor " << ancestor->blk_.to_str();
    ancestors_seqno_ = ancestor->seqno();
    ancestors_.emplace_back(std::move(ancestor));
  } else if (ton::shard_is_parent(ancestor->shard(), shard)) {
    VLOG(VALIDATOR_DEBUG) << "found one parent ancestor " << ancestor->blk_.to_str();
    ancestors_seqno_ = ancestor->seqno();
    ancestors_.emplace_back(std::move(ancestor));
    ancestors_split_ = true;
  } else {
    VLOG(VALIDATOR_WARNING) << " cannot retrieve information about shard " + shard.to_str() +
                                   " from masterchain block " + last_mc_id_.to_str() +
                                   ", skipping ShardTopBlockDescr creation";
    if (last_mc_id_.id.seqno <= mc_blkid_.id.seqno || ancestor->seqno() <= id_.id.seqno) {
      fatal_error(" cannot retrieve information about shard "s + shard.to_str() + " from masterchain block " +
                  last_mc_id_.to_str());
      return;
    }
    written_block_next();
    return;
  }
  if (ancestors_seqno_ >= id_.id.seqno) {
    VLOG(VALIDATOR_WARNING) << "skipping ShardTopBlockDescr creation for " << id_.to_str() << " because a newer block "
                            << ancestors_.at(0)->blk_.to_str() << " is already present in masterchain block "
                            << last_mc_id_.to_str();
    written_block_next();
    return;
  }
  if (id_.id.seqno > ancestors_seqno_ + 8) {
    fatal_error("cannot accept shardchain block "s + id_.to_str() +
                " because it requires including a chain of more than eight new shardchain blocks");
    return;
  }
  if (id_.id.seqno == ancestors_seqno_ + 1) {
    create_topshard_blk_descr();
    return;
  }
  CHECK(prev_.size() == 1);
  require_proof_link(prev_[0]);
}

void AcceptBlockQuery::require_proof_link(BlockIdExt id) {
  VLOG(VALIDATOR_DEBUG) << "require_proof_link(" << id.to_str() << ")";
  CHECK(ton::ShardIdFull(id) == ton::ShardIdFull(id_));
  CHECK(id.id.seqno == id_.id.seqno - 1 - proof_links_.size());
  td::actor::send_closure_later(manager_, &ValidatorManager::wait_block_proof_link_short, id, timeout_,
                                [SelfId = actor_id(this), id](td::Result<Ref<ProofLink>> R) {
                                  check_send_error(SelfId, R) ||
                                      td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_proof_link, id,
                                                                   R.move_as_ok());
                                });
}

bool AcceptBlockQuery::unpack_proof_link(BlockIdExt id, Ref<ProofLink> proof_link) {
  auto res0 = vm::std_boc_deserialize(proof_link->data());
  if (res0.is_error()) {
    return fatal_error("cannot deserialize proof link for "s + id.to_str() + ": " + res0.move_as_error().to_string());
  }
  Ref<vm::Cell> proof_root = res0.move_as_ok();
  block::gen::BlockProof::Record proof;
  BlockIdExt proof_blk_id;
  if (!(tlb::unpack_cell(proof_root, proof) &&
        block::tlb::t_BlockIdExt.unpack(proof.proof_for.write(), proof_blk_id))) {
    return false;
  }
  if (proof_blk_id != id) {
    return fatal_error("block proof link is for another block: expected "s + id.to_str() + ", found " +
                       proof_blk_id.to_str());
  }
  auto virt_root = vm::MerkleProof::virtualize(proof.root, 1);
  if (virt_root.is_null()) {
    return fatal_error("block proof link for block "s + id.to_str() +
                       " does not contain a valid Merkle proof for the block header");
  }
  RootHash virt_hash{virt_root->get_hash().bits()};
  if (virt_hash != id.root_hash) {
    return fatal_error("block proof link for block "s + id.to_str() +
                       " contains a Merkle proof with incorrect root hash: expected " + id.root_hash.to_hex() +
                       ", found " + virt_hash.to_hex());
  }
  bool after_split;
  BlockIdExt mc_blkid;
  auto res = block::unpack_block_prev_blk_try(virt_root, id, link_prev_, mc_blkid, after_split);
  if (res.is_error()) {
    return fatal_error("error in block header in proof link for "s + id.to_str() + ": " +
                       res.move_as_error().to_string());
  }
  if (mc_blkid.id.seqno > prev_mc_blkid_.id.seqno) {
    return fatal_error("previous shardchain block "s + id.to_str() + " refers to a newer masterchain block " +
                       mc_blkid.id.to_str() + " than that referred to by the next one: " + prev_mc_blkid_.id.to_str());
  } else if (mc_blkid.id.seqno < prev_mc_blkid_.id.seqno) {
    if (!last_mc_state_->check_old_mc_block_id(mc_blkid)) {
      return fatal_error(
          "previous shardchain block "s + id.to_str() + " refers to masterchain block " + mc_blkid.id.to_str() +
          " which is not an ancestor of that referred to by the next one: " + prev_mc_blkid_.id.to_str());
    }
    prev_mc_blkid_ = mc_blkid;
  } else if (mc_blkid != prev_mc_blkid_) {
    return fatal_error("previous shardchain block "s + id.to_str() + " refers to masterchain block " +
                       mc_blkid.id.to_str() +
                       " with the same height as, but distinct from that referred to by the next shardchain block: " +
                       prev_mc_blkid_.id.to_str());
  }
  try {
    block::gen::Block::Record block;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(virt_root, block) && block::gen::t_ValueFlow.force_validate_ref(block.value_flow))) {
      return fatal_error("block proof link for block "s + id.to_str() + " does not contain value flow information");
    }
    /* TEMP (uncomment later)
    if (!tlb::unpack_cell(std::move(block.extra), extra)) {
      return fatal_error("block proof link for block "s + id.to_str() + " does not contain BlockExtra information");
    }
    */
  } catch (vm::VmError& err) {
    return fatal_error("error unpacking proof link for block "s + id.to_str() + " : " + err.get_msg());
  } catch (vm::VmVirtError& err) {
    return fatal_error("virtualization error unpacking proof link for block "s + id.to_str() + " : " + err.get_msg());
  }
  proof_roots_.push_back(std::move(proof.root));
  return true;
}

void AcceptBlockQuery::got_proof_link(BlockIdExt id, Ref<ProofLink> proof) {
  VLOG(VALIDATOR_DEBUG) << "got_proof_link(" << id.to_str() << ")";
  CHECK(proof.not_null());
  proof_links_.push_back(proof);
  if (!unpack_proof_link(id, std::move(proof))) {
    fatal_error("cannot unpack proof link for "s + id.to_str());
    return;
  }
  if (id.id.seqno == ancestors_seqno_ + 1) {
    // first link in chain
    if (ancestors_.size() != link_prev_.size() || ancestors_[0]->blk_ != link_prev_[0] ||
        (ancestors_.size() == 2 && ancestors_[1]->blk_ != link_prev_[1])) {
      fatal_error("invalid first link at block "s + id.to_str() + " for shardchain block " + id_.to_str(),
                  ErrorCode::cancelled);
      return;
    }
    create_topshard_blk_descr();
  } else {
    CHECK(id.id.seqno > ancestors_seqno_);
    // intermediate link
    if (link_prev_.size() != 1 || ton::ShardIdFull(link_prev_[0].id) != ton::ShardIdFull(id_) ||
        link_prev_[0].id.seqno + 1 != id.id.seqno) {
      fatal_error("invalid intermediate link at block "s + id.to_str() + " for shardchain block " + id_.to_str(),
                  ErrorCode::cancelled);
      return;
    }
    require_proof_link(link_prev_[0]);
  }
}

bool AcceptBlockQuery::create_top_shard_block_description() {
  VLOG(VALIDATOR_DEBUG) << "create_top_shard_block_description()";
  CHECK(proof_roots_.size() == proof_links_.size() + 1);
  int n = (int)proof_roots_.size();
  CHECK(n <= 8);
  Ref<vm::Cell> root;
  for (int i = n - 1; i > 0; i--) {
    vm::CellBuilder cb;
    if (!(cb.store_ref_bool(proof_roots_[i]) && (root.is_null() || cb.store_ref_bool(root)) && cb.finalize_to(root))) {
      return fatal_error("error serializing ProofChain");
    }
  }
  vm::CellBuilder cb;
  Ref<vm::Cell> td_cell;
  if (!(cb.store_long_bool(0xd5, 8)                // top_block_descr#d5
        && block::tlb::t_BlockIdExt.pack(cb, id_)  // proof_for:BlockIdExt
        && cb.store_bool_bool(true)                // signatures:(Maybe
        && cb.store_ref_bool(signatures_cell_)     //   ^BlockSignatures)
        && cb.store_long_bool(n, 8)                // len:(## 8)
        && n <= 8                                  // { len <= 8 }
        && cb.store_ref_bool(proof_roots_[0])      // chain:(ProofChain len)
        && (root.is_null() || cb.store_ref_bool(std::move(root))) && cb.finalize_to(td_cell))) {
    return fatal_error("cannot serialize ShardTopBlockDescription for the newly-accepted block "s + id_.to_str());
  }
  if (false) {
    // debug output
    std::cerr << "new ShardTopBlockDescription: ";
    block::gen::t_TopBlockDescr.print_ref(std::cerr, td_cell);
    vm::load_cell_slice(td_cell).print_rec(std::cerr);
  }
  if (!block::gen::t_TopBlockDescr.validate_ref(td_cell)) {
    block::gen::t_TopBlockDescr.print_ref(std::cerr, td_cell);
    vm::load_cell_slice(td_cell).print_rec(std::cerr);
    return fatal_error("just created ShardTopBlockDescription for "s + id_.to_str() + " is invalid");
  }
  auto res = vm::std_boc_serialize(td_cell, 0);
  if (res.is_error()) {
    return fatal_error("cannot serialize a ShardTopBlockDescription object for "s + id_.to_str() + ": " +
                       res.move_as_error().to_string());
  }
  top_block_descr_data_ = res.move_as_ok();
  VLOG(VALIDATOR_DEBUG) << "create_top_shard_block_description() : end";
  return true;
}

void AcceptBlockQuery::create_topshard_blk_descr() {
  VLOG(VALIDATOR_DEBUG) << "create_topshard_blk_descr()";
  // generate top shard block description
  if (!create_top_shard_block_description()) {
    fatal_error("cannot generate top shard block description for "s + id_.to_str());
    return;
  }
  CHECK(top_block_descr_data_.size());
  td::actor::create_actor<ValidateShardTopBlockDescr>(
      "topshardfetchchk", std::move(top_block_descr_data_), last_mc_id_, BlockHandle{}, last_mc_state_, manager_,
      timeout_, is_fake_,
      [SelfId = actor_id(this)](td::Result<Ref<ShardTopBlockDescription>> R) {
        td::actor::send_closure_later(SelfId, &AcceptBlockQuery::top_block_descr_validated, std::move(R));
      })
      .release();
}

void AcceptBlockQuery::top_block_descr_validated(td::Result<Ref<ShardTopBlockDescription>> R) {
  VLOG(VALIDATOR_DEBUG) << "top_block_descr_validated()";
  if (R.is_error()) {
    VLOG(VALIDATOR_WARNING) << "error validating newly-created ShardTopBlockDescr for " << id_.to_str() << ": "
                            << R.move_as_error().to_string();
  } else {
    top_block_descr_ = R.move_as_ok();
    CHECK(top_block_descr_.not_null());
    td::actor::send_closure_later(manager_, &ValidatorManager::send_top_shard_block_description, top_block_descr_);
  }
  written_block_next();
}

void AcceptBlockQuery::written_block_next() {
  VLOG(VALIDATOR_DEBUG) << "written_block_next()";
  if (handle_->need_flush()) {
    handle_->flush(manager_, handle_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
      check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_info_2);
    });
  } else {
    written_block_info_2();
  }
}

void AcceptBlockQuery::written_block_info_2() {
  VLOG(VALIDATOR_DEBUG) << "written_block_info_2()";
  if (handle_->id().is_masterchain()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::applied);
    });
    run_apply_block_query(handle_->id(), data_, handle_->id(), manager_, timeout_, std::move(P));
  } else {
    applied();
  }
}

void AcceptBlockQuery::applied() {
  if (!send_broadcast_) {
    finish_query();
    return;
  }

  BlockBroadcast b;
  b.data = data_->data();
  b.block_id = id_;
  std::vector<BlockSignature> sigs;
  if (!is_fake_) {
    for (auto& v : signatures_->signatures()) {
      sigs.emplace_back(BlockSignature{v.node, v.signature.clone()});
    }
  }
  b.signatures = std::move(sigs);
  b.catchain_seqno = validator_set_->get_catchain_seqno();
  b.validator_set_hash = validator_set_->get_validator_set_hash();
  if (is_masterchain()) {
    b.proof = proof_->data();
  } else {
    b.proof = proof_link_->data();
  }

  // do not wait for answer
  td::actor::send_closure_later(manager_, &ValidatorManager::send_block_broadcast, std::move(b));

  finish_query();
}

}  // namespace validator

}  // namespace ton
