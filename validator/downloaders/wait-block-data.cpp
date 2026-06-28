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
#include "common/delay.h"
#include "crypto/block/mc-config.h"
#include "ton/ton-io.hpp"
#include "vm/cells/MerkleProof.h"

#include "block-auto.h"
#include "block-parse.h"
#include "fabric.h"
#include "wait-block-data.hpp"

namespace ton {

namespace validator {

td::Result<td::Ref<vm::Cell>> WaitBlockData::generate_block_proof_root(BlockIdExt id, td::Ref<vm::Cell> block_root,
                                                                       UnixTime* gen_utime) {
  if (block_root.is_null()) {
    return td::Status::Error("block root is null");
  }

  RootHash block_root_hash{block_root->get_hash().bits()};
  if (block_root_hash != id.root_hash) {
    return td::Status::Error(PSTRING() << "block root hash mismatch: expected " << id.root_hash.to_hex() << ", found "
                                       << block_root_hash.to_hex());
  }

  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  block::gen::ExtBlkRef::Record mcref{};  // _ ExtBlkRef = BlkMasterInfo;
  ShardIdFull shard;
  if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) &&
        block::gen::BlkPrevInfo{info.after_merge}.validate_ref(info.prev_ref) &&
        tlb::unpack_cell(std::move(blk.extra), extra) && block::gen::t_ValueFlow.force_validate_ref(blk.value_flow) &&
        (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)))) {
    return td::Status::Error("cannot unpack block header");
  }
  if (info.not_master != !shard.is_masterchain()) {
    return td::Status::Error("block has invalid not_master flag in its header");
  }
  BlockId block_id{shard, static_cast<unsigned>(info.seq_no)};
  if (block_id != id.id) {
    return td::Status::Error(PSTRING() << "block header corresponds to another block id: expected " << id.id
                                       << ", found " << block_id);
  }
  if (shard.is_masterchain() && (info.after_merge | info.after_split | info.before_split)) {
    return td::Status::Error(PSTRING() << "masterchain block header of " << id << " announces merge/split");
  }
  if (!shard.is_masterchain() && info.key_block) {
    return td::Status::Error(PSTRING() << "non-masterchain block header of " << id << " announces this block to be "
                                       << "a key block");
  }

  vm::CellSlice upd_cs{vm::NoVm(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4 && upd_cs.size_ext() == 0x20228)) {
    return td::Status::Error("invalid Merkle update in block");
  }

  if (info.key_block) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && mc_extra.key_block &&
          mc_extra.config.not_null())) {
      return td::Status::Error(PSTRING() << "cannot unpack extra header of key masterchain block " << block_id);
    }
    auto cfg = block::Config::unpack_config(std::move(mc_extra.config));
    if (cfg.is_error()) {
      return cfg.move_as_error_prefix(PSTRING() << "cannot extract configuration from extra header of key masterchain "
                                                << "block " << block_id << ": ");
    }
    auto res = cfg.move_as_ok()->visit_validator_params();
    if (res.is_error()) {
      return res.move_as_error_prefix(PSTRING() << "cannot extract validator set configuration from extra header of "
                                                << "key masterchain block " << block_id << ": ");
    }
  }

  TRY_RESULT(proof, vm::MerkleProof::generate(block_root, usage_tree.get()));

  if (shard.is_masterchain() && !info.key_block) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) &&
          !mc_extra.key_block)) {
      return td::Status::Error(PSTRING() << "extra header of non-key masterchain block " << block_id
                                         << " is invalid or contains extra information reserved for key blocks only");
    }
  }

  if (gen_utime) {
    *gen_utime = info.gen_utime;
  }
  return proof;
}

static td::Result<td::BufferSlice> serialize_block_proof(BlockIdExt id, td::Ref<vm::Cell> proof, bool has_signatures,
                                                         td::Ref<vm::Cell> signatures_cell = {}) {
  vm::CellBuilder cb;
  td::Ref<vm::Cell> proof_cell;
  if (!(cb.store_long_bool(0xc3, 8)               // block_proof#c3
        && block::tlb::t_BlockIdExt.pack(cb, id)  // proof_for:BlockIdExt
        && cb.store_ref_bool(std::move(proof))    // proof:^Cell
        && cb.store_bool_bool(has_signatures)     // signatures:(Maybe ^BlockSignatures)
        && (!has_signatures || cb.store_ref_bool(std::move(signatures_cell))) && cb.finalize_to(proof_cell))) {
    return td::Status::Error("cannot serialize BlockProof");
  }
  if (!block::gen::t_BlockProof.validate_ref(proof_cell)) {
    return td::Status::Error("created BlockProof failed automated consistency checks");
  }
  return std_boc_serialize(proof_cell, 0);
}

void WaitBlockData::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void WaitBlockData::abort_query(td::Status reason) {
  if (promise_) {
    if (priority_ > 0 || (reason.code() != ErrorCode::timeout && reason.code() != ErrorCode::notready)) {
      LOG(WARNING) << "aborting wait block data query for " << handle_->id() << " priority=" << priority_ << ": "
                   << reason;
    } else {
      LOG(DEBUG) << "aborting wait block data query for " << handle_->id() << " priority=" << priority_ << ": "
                 << reason;
    }
    promise_.set_error(reason.move_as_error_prefix(PSTRING() << "failed to download " << handle_->id() << ": "));
  }
  stop();
}

void WaitBlockData::finish_query() {
  CHECK(handle_->received());
  if (promise_) {
    promise_.set_result(data_);
  }
  stop();
}

void WaitBlockData::start_up() {
  alarm_timestamp() = timeout_;

  CHECK(handle_);
  if (!handle_->id().is_masterchain()) {
    start();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<bool> R) {
      R.ensure();
      auto value = R.move_as_ok();
      td::actor::send_closure(SelfId, &WaitBlockData::set_is_hardfork, value);
    });
    td::actor::send_closure(manager_, &ValidatorManager::check_is_hardfork, handle_->id(), std::move(P));
  }
}

void WaitBlockData::set_is_hardfork(bool value) {
  is_hardfork_ = value;
  start();
}

void WaitBlockData::start() {
  if (reading_from_db_) {
    return;
  }
  if (handle_->received() &&
      (handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link())) {
    reading_from_db_ = true;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db get error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::got_block_data_from_db, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle_, std::move(P));
  } else if (try_read_static_file_.is_in_past() && (is_hardfork_ || !handle_->id().is_masterchain())) {
    try_read_static_file_ = td::Timestamp::in(30.0);

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::start);
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::got_static_file, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::try_get_static_file, handle_->id().file_hash, std::move(P));
  } else if (try_get_candidate_ && !handle_->id().is_masterchain()) {
    try_get_candidate_ = false;
    td::actor::send_closure(manager_, &ValidatorManager::get_cached_candidate_data, handle_->id(),
                            [SelfId = actor_id(this), id = handle_->id()](td::Result<td::BufferSlice> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &WaitBlockData::start);
                              } else {
                                td::actor::send_closure(SelfId, &WaitBlockData::loaded_data,
                                                        ReceivedBlock{id, R.move_as_ok()});
                              }
                            });
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ReceivedBlock> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::failed_to_get_block_data_from_net,
                                R.move_as_error_prefix("net error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::loaded_data, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::send_get_block_request, handle_->id(), priority_,
                            std::move(P));
  }
}

void WaitBlockData::got_block_data_from_db(td::Ref<BlockData> data) {
  data_ = std::move(data);
  finish_query();
}

void WaitBlockData::failed_to_get_block_data_from_net(td::Status reason) {
  if (reason.code() == ErrorCode::notready) {
    LOG(DEBUG) << "failed to get block " << handle_->id() << " data from net: " << reason;
  } else {
    LOG(WARNING) << "failed to get block " << handle_->id() << " data from net: " << reason;
  }

  delay_action([SelfId = actor_id(this)]() mutable { td::actor::send_closure(SelfId, &WaitBlockData::start); },
               td::Timestamp::in(0.1));
}

void WaitBlockData::loaded_data(ReceivedBlock block) {
  auto X = create_block(std::move(block));
  if (X.is_error()) {
    failed_to_get_block_data_from_net(X.move_as_error_prefix("bad block from net: "));
    return;
  }
  loaded_block_data(X.move_as_ok());
}

void WaitBlockData::loaded_block_data(td::Ref<BlockData> block) {
  if (data_.not_null()) {
    return;
  }
  data_ = std::move(block);
  if (handle_->received()) {
    finish_query();
    return;
  }
  if (!handle_->id().is_masterchain() && !handle_->inited_proof_link()) {
    // This can happen if we get block from candidates cache.
    // Proof link can be derived from the block (but not for masterchain block).
    auto r_proof_link = generate_proof_link(handle_->id(), data_->root_cell());
    if (r_proof_link.is_error()) {
      abort_query(r_proof_link.move_as_error_prefix("failed to create proof link for block: "));
      return;
    }
    td::actor::send_closure(manager_, &ValidatorManager::validate_block_proof_link, handle_->id(),
                            r_proof_link.move_as_ok(),
                            [id = handle_->id().id, SelfId = actor_id(this)](td::Result<td::Unit> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &WaitBlockData::abort_query,
                                                        R.move_as_error_prefix("validate proof link error: "));
                                return;
                              }
                              LOG(DEBUG) << "Created and validated proof link for " << id;
                              td::actor::send_closure(SelfId, &WaitBlockData::checked_proof_link);
                            });
    return;
  }
  // After send_get_block_request, inited_proof() == true for masterchain (see DownloadBlockNew)
  checked_proof_link();
}

void WaitBlockData::checked_proof_link() {
  CHECK(handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link());
  if (!handle_->received()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db set error: "));
      } else {
        td::actor::send_closure(SelfId, &WaitBlockData::finish_query);
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, data_, std::move(P));
  } else {
    finish_query();
  }
}

void WaitBlockData::force_read_from_db() {
  if (reading_from_db_) {
    return;
  }
  CHECK(handle_->id().is_masterchain() ? handle_->inited_proof() : handle_->inited_proof_link());
  CHECK(handle_->received());
  reading_from_db_ = true;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("db read error: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockData::got_block_data_from_db, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle_, std::move(P));
}

void WaitBlockData::got_static_file(td::BufferSlice data) {
  CHECK(td::sha256_bits256(data.as_slice()) == handle_->id().file_hash);

  auto R = create_block(handle_->id(), std::move(data));
  if (R.is_error()) {
    LOG(ERROR) << "bad static file block: " << R.move_as_error();
    start();
    return;
  }
  data_ = R.move_as_ok();

  CHECK(is_hardfork_ || !handle_->id().is_masterchain());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &WaitBlockData::abort_query, R.move_as_error_prefix("bad static file block: "));
    } else {
      td::actor::send_closure(SelfId, &WaitBlockData::finish_query);
    }
  });
  run_hardfork_accept_block_query(handle_->id(), data_, manager_, std::move(P));
}

td::Result<td::BufferSlice> WaitBlockData::generate_proof(BlockIdExt id, td::Ref<vm::Cell> block_root,
                                                          td::Ref<block::BlockSignatureSet> signatures,
                                                          td::Ref<MasterchainState> state) {
  if (!id.is_masterchain()) {
    return td::Status::Error("cannot create proof for non-masterchain block");
  }
  if (signatures.is_null()) {
    return td::Status::Error("block signatures are null");
  }
  if (!signatures->is_final()) {
    return td::Status::Error("cannot create masterchain proof with non-final signatures");
  }
  if (state.is_null()) {
    return td::Status::Error(ErrorCode::notready, "masterchain state is not ready");
  }

  UnixTime gen_utime;
  TRY_RESULT(proof_root, generate_block_proof_root(id, std::move(block_root), &gen_utime));
  TRY_RESULT(config, state->get_config_holder());
  auto vset = config->get_validator_set(id.shard_full(), gen_utime, signatures->get_catchain_seqno());
  if (vset.is_null()) {
    return td::Status::Error(ErrorCode::notready, "failed to compute validator set for masterchain proof");
  }
  TRY_RESULT(signatures_cell, signatures->serialize(vset));
  return serialize_block_proof(id, std::move(proof_root), true, std::move(signatures_cell));
}

td::Result<td::BufferSlice> WaitBlockData::generate_proof_link(BlockIdExt id, td::Ref<vm::Cell> block_root) {
  // Creating proof link. Similar to accept-block.cpp
  if (id.is_masterchain()) {
    return td::Status::Error("cannot create proof link for masterchain block");
  }
  TRY_RESULT(proof_root, generate_block_proof_root(id, std::move(block_root)));
  return serialize_block_proof(id, std::move(proof_root), false);
}

}  // namespace validator

}  // namespace ton
