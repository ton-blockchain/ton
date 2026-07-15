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
#include "adnl/utils.hpp"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "interfaces/validator-manager.h"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"
#include "validator/invariants.hpp"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cells/MerkleProof.h"

#include "accept-block.hpp"
#include "fabric.h"
#include "full-node.h"
#include "top-shard-descr.hpp"

namespace ton {

namespace validator {
using namespace std::literals::string_literals;

AcceptBlockQuery::AcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                   td::Ref<block::ValidatorSet> validator_set,
                                   td::Ref<block::BlockSignatureSet> signatures, bool apply,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , validator_set_(std::move(validator_set))
    , signatures_(std::move(signatures))
    , is_fake_(false)
    , is_fork_(false)
    , apply_(apply)
    , manager_(manager)
    , promise_(std::move(promise))
    , perf_timer_("acceptblock", 0.1, [manager](double duration) {
      send_closure(manager, &ValidatorManager::add_perf_timer_stat, "acceptblock", duration);
    }) {
  state_keep_old_hash_.clear();
  state_old_hash_.clear();
  state_hash_.clear();
}

AcceptBlockQuery::AcceptBlockQuery(AcceptBlockQuery::IsFake fake, BlockIdExt id, td::Ref<BlockData> data,
                                   std::vector<BlockIdExt> prev, td::Ref<block::ValidatorSet> validator_set,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , prev_(std::move(prev))
    , validator_set_(std::move(validator_set))
    , signatures_(block::BlockSignatureSet::create_ordinary(std::vector<BlockSignature>{},
                                                            validator_set_->get_catchain_seqno(),
                                                            validator_set_->get_validator_set_hash()))
    , is_fake_(true)
    , is_fork_(false)
    , manager_(manager)
    , promise_(std::move(promise))
    , perf_timer_("acceptblock", 0.1, [manager](double duration) {
      send_closure(manager, &ValidatorManager::add_perf_timer_stat, "acceptblock", duration);
    }) {
  state_keep_old_hash_.clear();
  state_old_hash_.clear();
  state_hash_.clear();
}

AcceptBlockQuery::AcceptBlockQuery(ForceFork ffork, BlockIdExt id, td::Ref<BlockData> data,
                                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise)
    : id_(id)
    , data_(std::move(data))
    , signatures_(block::BlockSignatureSet::create_ordinary(std::vector<BlockSignature>{}, 0, 0))
    , is_fake_(true)
    , is_fork_(true)
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
  VLOG(validator, DEBUG) << "precheck_header()";
  // 0. sanity check
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
  if (prev_.size() == 0) {
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
  before_split_ = info.before_split;
  return true;
}

bool AcceptBlockQuery::create_new_proof() {
  // 0. check block's root hash
  VLOG(validator, DEBUG) << "create_new_proof() : start";
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
    return fatal_error(PSTRING() << "block header of " << id_ << " announces " << info.after_merge + 1
                                 << " previous blocks, but " << prev_.size() << " are actually present");
  }
  if (is_masterchain() && (info.after_merge | info.after_split | info.before_split)) {
    return fatal_error("masterchain block header of "s + id_.to_str() + " announces merge/split in its header");
  }
  if (!is_masterchain() && is_key_block_) {
    return fatal_error("non-masterchain block header of "s + id_.to_str() + " announces this block to be a key block");
  }
  // 3. check state update
  vm::CellSlice upd_cs{vm::NoVm(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  // 4. visit validator-set related fields in key blocks
  if (is_key_block_) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && mc_extra.key_block &&
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
  auto r_proof = vm::MerkleProof::generate(block_root_, usage_tree.get());
  if (r_proof.is_error()) {
    return fatal_error("cannot create proof");
  }
  auto proof = r_proof.move_as_ok();
  // 6. extract some information from state update
  state_old_hash_ = upd_cs.prefetch_ref(0)->get_hash(0).bits();
  state_hash_ = upd_cs.prefetch_ref(1)->get_hash(0).bits();
  lt_ = info.end_lt;
  created_at_ = info.gen_utime;
  if (is_masterchain() && !is_key_block_) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) &&
          !mc_extra.key_block)) {
      return fatal_error("extra header of non-key masterchain block "s + blk_id.to_str() +
                         " is invalid or contains extra information reserved for key blocks only");
    }
  }
  // 7. check signatures
  if (!is_fake_) {
    td::Result<td::uint64> sign_chk;
    if (signatures_->is_final()) {
      sign_chk = signatures_->check_signatures(validator_set_, id_);
    } else {
      sign_chk = signatures_->check_approve_signatures(validator_set_, id_);
    }
    if (sign_chk.is_error()) {
      auto err = sign_chk.move_as_error();
      VLOG(validator, WARNING) << "signature check failed : " << err.to_string();
      abort_query(std::move(err));
      return false;
    }
  }
  Ref<vm::Cell> signatures_cell;
  if (signatures_->is_final()) {
    // 8. serialize signatures
    if (!is_fake_) {
      vm::CellBuilder cb2;
      auto r_sign_cell = signatures_->serialize(validator_set_);
      if (r_sign_cell.is_error()) {
        abort_query(
            r_sign_cell.move_as_error_prefix("cannot serialize BlockSignatures for the newly-accepted block: "));
        return false;
      }
      signatures_cell = r_sign_cell.move_as_ok();
    } else {  // FAKE
      vm::CellBuilder cb2;
      if (!(cb2.store_long_bool(0x11, 8)  // block_signatures#11
            && cb2.store_long_bool(validator_set_.not_null() ? validator_set_->get_validator_set_hash() : 0,
                                   32)  // validator_info$_ validator_set_hash_short:uint32
            && cb2.store_long_bool(validator_set_.not_null() ? validator_set_->get_catchain_seqno() : 0,
                                   32)     //   validator_set_ts:uint32
            && cb2.store_long_bool(0, 32)  // sig_count:uint32
            && cb2.store_long_bool(0, 64)  // sig_weight:uint32
            && cb2.store_bool_bool(false)  // (HashmapE 16 CryptoSignaturePair)
            && cb2.finalize_to(signatures_cell))) {
        return fatal_error("cannot serialize fake BlockSignatures for the newly-accepted block");
      }
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
          && cb.store_ref_bool(signatures_cell)      //   ^BlockSignatures)
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
    FLOG(WARNING) {
      sb << "BlockProof object just created failed to pass automated consistency checks: ";
      block::gen::t_BlockProof.print_ref(sb, bs_cell);
      vm::load_cell_slice(bs_cell).print_rec(sb);
    };
    return fatal_error("BlockProof object just created failed to pass automated consistency checks");
  }
  // 11. create a proof object from this cell
  if (is_masterchain()) {
    proof_ = create_proof(id_, vm::std_boc_serialize(bs_cell, 0).move_as_ok()).move_as_ok();
  } else {
    proof_link_ = create_proof_link(id_, vm::std_boc_serialize(bs_cell, 0).move_as_ok()).move_as_ok();
  }
  VLOG(validator, DEBUG) << "create_new_proof() : end";
  return true;
}

void AcceptBlockQuery::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(validator, WARNING) << "aborting accept block query: " << reason;
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
  VLOG(validator, DEBUG) << "finish_query()";
  if (apply_) {
    ValidatorInvariants::check_post_accept(handle_);
  }
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
  VLOG(validator, DEBUG) << "start_up()";
  alarm_timestamp() = timeout_;

  if (!is_fork_ && validator_set_.is_null()) {
    fatal_error("no real ValidatorSet passed to AcceptBlockQuery");
    return;
  }
  if (!is_fake_ && !signatures_->is_final() && is_masterchain()) {
    fatal_error("no real SignatureSet passed to AcceptBlockQuery for masterchain");
    return;
  }
  if (!is_fake_ && is_fork_) {
    fatal_error("a non-fake AcceptBlockQuery for a forced fork block");
    return;
  }
  if (is_fork_ && !is_masterchain()) {
    fatal_error("cannot accept a non-masterchain fork block");
    return;
  }
  if (data_.is_null()) {
    fatal_error("cannot accept block without explicit data");
    return;
  }
  if (!precheck_header()) {
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
  VLOG(validator, DEBUG) << "got_block_handle()";
  handle_ = std::move(handle);
  if (handle_->received() && handle_->received_state() &&
      (handle_->inited_signatures() || !signatures_->is_final() || is_fork_) && handle_->inited_split_after() &&
      handle_->inited_merge_before() && handle_->inited_prev() && handle_->inited_logical_time() &&
      handle_->inited_state_root_hash() &&
      (is_masterchain() ? handle_->inited_proof() && handle_->is_applied() && handle_->inited_is_key_block()
                        : handle_->inited_proof_link())) {
    finish_query();
    return;
  }
  got_block_handle_cont();
}

void AcceptBlockQuery::got_block_handle_cont() {
  VLOG(validator, DEBUG) << "got_block_handle_cont()";
  if (!handle_->received()) {
    td::actor::send_closure(
        manager_, &ValidatorManager::set_block_data, handle_, data_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
          check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_data);
        });
  } else {
    written_block_data();
  }
}

void AcceptBlockQuery::written_block_data() {
  VLOG(validator, DEBUG) << "written_block_data()";
  if (handle_->inited_signatures() || !signatures_->is_final() || is_fork_) {
    written_block_signatures();
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManager::set_block_signatures, handle_, signatures_, validator_set_,
                          [SelfId = actor_id(this)](td::Result<td::Unit> R) {
                            check_send_error(SelfId, R) ||
                                td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_signatures);
                          });
}

void AcceptBlockQuery::written_block_signatures() {
  VLOG(validator, DEBUG) << "written_block_signatures()";
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
  VLOG(validator, DEBUG) << "written block info";
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
  if (!apply_) {
    written_state({});
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    check_send_error(SelfId, R) ||
        td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::got_prev_state, R.move_as_ok());
  });

  VLOG(validator, DEBUG) << "wait_prev_block_state";
  td::actor::send_closure(manager_, &ValidatorManager::wait_prev_block_state, handle_, priority(), timeout_,
                          std::move(P));
}

void AcceptBlockQuery::got_prev_state(td::Ref<ShardState> state) {
  VLOG(validator, DEBUG) << "got prev state";
  state_ = std::move(state);

  state_keep_old_hash_ = state_->root_hash();
  td::actor::send_closure(manager_, &ValidatorManager::set_block_state_from_data, handle_, data_,
                          [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                            check_send_error(SelfId, R) ||
                                td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_state, R.move_as_ok());
                          });
}

void AcceptBlockQuery::written_state(td::Ref<ShardState> upd_state) {
  VLOG(validator, DEBUG) << "written state";
  state_ = std::move(upd_state);

  if (apply_ && state_keep_old_hash_ != state_old_hash_) {
    fatal_error(PSTRING() << "invalid previous state hash in newly-created proof: expected "
                          << state_->root_hash().to_hex() << ", found in update " << state_old_hash_.to_hex());
    return;
  }

  handle_->set_split(before_split_);
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
  VLOG(validator, DEBUG) << "written_block_proof()";
  if (!is_masterchain()) {
    written_block_next();
    return;
  }
  CHECK(prev_.size() == 1);
  td::actor::send_closure(
      manager_, &ValidatorManager::set_next_block, prev_[0], id_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
        check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_next);
      });
}

void AcceptBlockQuery::written_block_next() {
  VLOG(validator, DEBUG) << "written_block_next()";
  if (handle_->need_flush()) {
    handle_->flush(manager_, handle_, [SelfId = actor_id(this)](td::Result<td::Unit> R) {
      check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &AcceptBlockQuery::written_block_info_2);
    });
  } else {
    written_block_info_2();
  }
}

void AcceptBlockQuery::written_block_info_2() {
  VLOG(validator, DEBUG) << "written_block_info_2()";
  td::actor::send_closure(manager_, &ValidatorManager::on_block_accepted, id_);
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
  finish_query();
}

}  // namespace validator

}  // namespace ton
