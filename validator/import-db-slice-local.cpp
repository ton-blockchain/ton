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
*/
#include <delay.h>

#include "block/block-auto.h"
#include "common/checksum.h"
#include "downloaders/download-state.hpp"
#include "td/actor/MultiPromise.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"
#include "validator/db/fileref.hpp"
#include "validator/fabric.h"

#include "import-db-slice-local.hpp"

namespace ton {

namespace validator {

ArchiveImporterLocal::ArchiveImporterLocal(std::string db_root, td::Ref<MasterchainState> state,
                                           BlockSeqno shard_client_seqno, td::Ref<ValidatorManagerOptions> opts,
                                           td::actor::ActorId<ValidatorManager> manager,
                                           std::vector<std::string> to_import_files,
                                           td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise)
    : db_root_(std::move(db_root))
    , last_masterchain_state_(std::move(state))
    , shard_client_seqno_(shard_client_seqno)
    , opts_(std::move(opts))
    , manager_(manager)
    , to_import_files_(std::move(to_import_files))
    , promise_(std::move(promise))
    , perf_timer_("import-slice-local", 10.0, [manager](double duration) {
      send_closure(manager, &ValidatorManager::add_perf_timer_stat, "import-slice-local", duration);
    }) {
}

void ArchiveImporterLocal::start_up() {
  LOG(WARNING) << "Importing archive for masterchain seqno #" << shard_client_seqno_ + 1 << " from disk";
  for (const std::string &path : to_import_files_) {
    LOG(INFO) << "Importing file from disk " << path;
    td::Status S = process_package(path);
    if (S.is_error()) {
      LOG(WARNING) << "Error processing package " << path << ": " << S;
    }
  }

  process_masterchain_blocks();
}

td::Status ArchiveImporterLocal::process_package(std::string path) {
  LOG(DEBUG) << "Processing package " << path;
  TRY_RESULT(p, Package::open(path, false, false));
  auto package = std::make_shared<Package>(std::move(p));

  td::Status S = td::Status::OK();
  package->iterate([&](std::string filename, td::BufferSlice data, td::uint64) -> bool {
    auto F = FileReference::create(filename);
    if (F.is_error()) {
      S = F.move_as_error();
      return false;
    }
    auto f = F.move_as_ok();

    BlockIdExt block_id;
    bool is_proof = false;
    bool ignore = true;

    f.ref().visit(td::overloaded(
        [&](const fileref::Proof &p) {
          block_id = p.block_id;
          ignore = !block_id.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::ProofLink &p) {
          block_id = p.block_id;
          ignore = block_id.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::Block &p) {
          block_id = p.block_id;
          ignore = false;
          is_proof = false;
        },
        [&](const auto &) { ignore = true; }));

    if (ignore || block_id.is_masterchain() && block_id.seqno() <= last_masterchain_state_->get_seqno()) {
      return true;
    }

    if (is_proof) {
      if (block_id.is_masterchain()) {
        auto R = create_proof(block_id, std::move(data));
        if (R.is_error()) {
          S = R.move_as_error();
          return false;
        }
        blocks_[block_id].proof = R.move_as_ok();
      } else {
        auto R = create_proof_link(block_id, std::move(data));
        if (R.is_error()) {
          S = R.move_as_error();
          return false;
        }
        blocks_[block_id].proof_link = R.move_as_ok();
      }
    } else {
      if (td::sha256_bits256(data) != block_id.file_hash) {
        S = td::Status::Error(ErrorCode::protoviolation, "bad block file hash");
        return false;
      }
      auto R = create_block(block_id, std::move(data));
      if (R.is_error()) {
        S = R.move_as_error();
        return false;
      }
      blocks_[block_id].block = R.move_as_ok();
    }
    if (block_id.is_masterchain()) {
      masterchain_blocks_[block_id.seqno()] = block_id;
    }
    return true;
  });
  return S;
}

void ArchiveImporterLocal::process_masterchain_blocks() {
  if (masterchain_blocks_.empty()) {
    LOG(INFO) << "No masterchain blocks in the archive";
    checked_masterchain_proofs();
    return;
  }

  if (masterchain_blocks_.begin()->first != last_masterchain_state_->get_seqno() + 1) {
    abort_query(td::Status::Error(ErrorCode::notready, PSTRING() << "expected masterchain seqno "
                                                                 << last_masterchain_state_->get_seqno() + 1
                                                                 << ", found " << masterchain_blocks_.begin()->first));
    return;
  }
  {
    BlockSeqno expected_seqno = last_masterchain_state_->get_seqno() + 1;
    for (auto &[seqno, _] : masterchain_blocks_) {
      if (seqno != expected_seqno) {
        abort_query(td::Status::Error(ErrorCode::protoviolation, "non-consecutive masterchain blocks in the archive"));
        return;
      }
      ++expected_seqno;
    }
  }
  BlockInfo &first_block = blocks_[masterchain_blocks_.begin()->second];
  if (first_block.proof.is_null()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block proof"));
    return;
  }
  if (first_block.block.is_null()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block data"));
    return;
  }
  block::gen::Block::Record rec;
  block::gen::BlockInfo::Record info;
  if (!(block::gen::unpack_cell(first_block.block->root_cell(), rec) && block::gen::unpack_cell(rec.info, info))) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "cannot unpack masterchain block info"));
    return;
  }
  if (info.key_block) {
    import_first_key_block();
    return;
  }

  process_masterchain_blocks_cont();
}

void ArchiveImporterLocal::import_first_key_block() {
  BlockIdExt block_id = masterchain_blocks_.begin()->second;
  BlockInfo &first_block = blocks_[block_id];
  LOG(INFO) << "First block in archive is key block : " << block_id.id.to_str();

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), prev_block_id = last_masterchain_state_->get_block_id()](
                                     td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
          return;
        }
        auto handle = R.move_as_ok();
        CHECK(!handle->merge_before());
        if (handle->one_prev(true) != prev_block_id) {
          td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query,
                                  td::Status::Error(ErrorCode::protoviolation, "prev block mismatch"));
          return;
        }
        td::actor::send_closure(SelfId, &ArchiveImporterLocal::checked_key_block_proof, std::move(handle));
      });

  run_check_proof_query(block_id, first_block.proof, manager_, td::Timestamp::in(600.0), std::move(P),
                        last_masterchain_state_, opts_->is_hardfork(block_id));
}

void ArchiveImporterLocal::checked_key_block_proof(BlockHandle handle) {
  BlockIdExt block_id = masterchain_blocks_.begin()->second;
  CHECK(block_id == handle->id());
  BlockInfo &first_block = blocks_[block_id];
  run_apply_block_query(
      handle->id(), first_block.block, handle->id(), manager_, td::Timestamp::in(600.0),
      [SelfId = actor_id(this), manager = manager_, handle](td::Result<td::Unit> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
          return;
        }
        td::actor::send_closure(
            manager, &ValidatorManager::get_shard_state_from_db, handle, [=](td::Result<td::Ref<ShardState>> R2) {
              if (R2.is_error()) {
                td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R2.move_as_error());
                return;
              }
              td::actor::send_closure(SelfId, &ArchiveImporterLocal::applied_key_block,
                                      td::Ref<MasterchainState>{R2.move_as_ok()});
            });
      });
}

void ArchiveImporterLocal::applied_key_block(td::Ref<MasterchainState> state) {
  CHECK(state->get_block_id() == masterchain_blocks_.begin()->second);
  last_masterchain_state_ = state;
  imported_any_ = true;
  masterchain_blocks_.erase(masterchain_blocks_.begin());
  blocks_.erase(state->get_block_id());
  LOG(INFO) << "Imported key block " << state->get_block_id().id.to_str();
  if (masterchain_blocks_.empty()) {
    LOG(INFO) << "No more masterchain blocks in the archive";
    checked_masterchain_proofs();
    return;
  }
  process_masterchain_blocks_cont();
}

void ArchiveImporterLocal::process_masterchain_blocks_cont() {
  LOG(INFO) << "Importing masterchain blocks from " << masterchain_blocks_.begin()->first << " to "
            << masterchain_blocks_.rbegin()->first;

  td::MultiPromise mp;
  auto ig = mp.init_guard();

  BlockIdExt prev_block_id = last_masterchain_state_->get_block_id();
  for (auto &[_, block_id] : masterchain_blocks_) {
    auto &info = blocks_[block_id];
    info.import = true;
    if (info.proof.is_null()) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block proof"));
      return;
    }
    if (info.block.is_null()) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block data"));
      return;
    }
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), prev_block_id, promise = ig.get_promise()](td::Result<BlockHandle> R) mutable {
          TRY_RESULT_PROMISE(promise, handle, std::move(R));
          CHECK(!handle->merge_before());
          if (handle->one_prev(true) != prev_block_id) {
            promise.set_error(td::Status::Error(ErrorCode::protoviolation, "prev block mismatch"));
            return;
          }
          promise.set_result(td::Unit());
        });
    run_check_proof_query(block_id, info.proof, manager_, td::Timestamp::in(600.0), std::move(P),
                          last_masterchain_state_, opts_->is_hardfork(block_id));
    prev_block_id = block_id;
  }
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
    } else {
      LOG(INFO) << "Checked proofs for masterchain blocks";
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::checked_masterchain_proofs);
    }
  });
}

void ArchiveImporterLocal::checked_masterchain_proofs() {
  if (shard_client_seqno_ == last_masterchain_state_->get_seqno()) {
    got_shard_client_state(last_masterchain_state_);
  } else {
    CHECK(shard_client_seqno_ < last_masterchain_state_->get_seqno());
    BlockIdExt block_id;
    if (!last_masterchain_state_->get_old_mc_block_id(shard_client_seqno_, block_id)) {
      abort_query(td::Status::Error("failed to get shard client block id"));
      return;
    }
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id,
                            [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                              if (R.is_error()) {
                                td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query,
                                                        R.move_as_error_prefix("failed to get shard client state: "));
                                return;
                              }
                              td::actor::send_closure(SelfId, &ArchiveImporterLocal::got_shard_client_state,
                                                      td::Ref<MasterchainState>{R.move_as_ok()});
                            });
  }
}

void ArchiveImporterLocal::got_shard_client_state(td::Ref<MasterchainState> state) {
  CHECK(state->get_seqno() == shard_client_seqno_);
  LOG(DEBUG) << "got_shard_client_state " << shard_client_seqno_;
  shard_client_state_ = state;
  new_shard_client_seqno_ = shard_client_seqno_;
  current_shard_client_seqno_ = shard_client_seqno_;
  for (auto &shard : state->get_shards()) {
    visited_shard_blocks_.insert(shard->top_block_id());
  }
  try_advance_shard_client_seqno();
}

void ArchiveImporterLocal::try_advance_shard_client_seqno() {
  BlockSeqno seqno = new_shard_client_seqno_ + 1;
  auto it = masterchain_blocks_.find(seqno);
  if (it != masterchain_blocks_.end()) {
    try_advance_shard_client_seqno_cont(blocks_[it->second].block);
    return;
  }
  if (seqno > last_masterchain_state_->get_seqno()) {
    processed_shard_blocks();
    return;
  }
  BlockIdExt block_id;
  if (!last_masterchain_state_->get_old_mc_block_id(seqno, block_id)) {
    abort_query(td::Status::Error("failed to get old mc block id"));
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db_short, block_id,
                          [SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query,
                                                      R.move_as_error_prefix("failed to get block data: "));
                              return;
                            }
                            td::actor::send_closure(SelfId, &ArchiveImporterLocal::try_advance_shard_client_seqno_cont,
                                                    R.move_as_ok());
                          });
}

void ArchiveImporterLocal::try_advance_shard_client_seqno_cont(td::Ref<BlockData> mc_block) {
  CHECK(mc_block.not_null());
  CHECK(mc_block->block_id().seqno() == new_shard_client_seqno_ + 1);
  LOG(DEBUG) << "try_advance_shard_client_seqno " << new_shard_client_seqno_ + 1;

  block::gen::Block::Record rec;
  block::gen::BlockExtra::Record extra;
  block::gen::McBlockExtra::Record mc_extra;
  CHECK(block::gen::unpack_cell(mc_block->root_cell(), rec) && block::gen::unpack_cell(rec.extra, extra) &&
        block::gen::unpack_cell(extra.custom->prefetch_ref(), mc_extra));
  auto shard_config = std::make_unique<block::ShardConfig>(mc_extra.shard_hashes->prefetch_ref());

  std::vector<BlockIdExt> blocks_to_import;
  std::function<td::Status(const BlockIdExt &)> dfs = [&](const BlockIdExt &block_id) -> td::Status {
    if (visited_shard_blocks_.contains(block_id)) {
      return td::Status::OK();
    }
    if (block_id.seqno() == 0) {
      new_zerostates_.insert(block_id);
      return td::Status::OK();
    }
    visited_shard_blocks_.insert(block_id);
    auto &info = blocks_[block_id];
    if (info.block.is_null()) {
      return td::Status::Error(PSTRING() << "no shard block " << block_id.to_str());
    }
    blocks_to_import.push_back(block_id);

    std::vector<BlockIdExt> prev;
    BlockIdExt mc_blkid;
    bool after_split;
    TRY_STATUS(block::unpack_block_prev_blk_try(info.block->root_cell(), block_id, prev, mc_blkid, after_split));
    for (const BlockIdExt &prev_block_id : prev) {
      TRY_STATUS(dfs(prev_block_id));
    }

    return td::Status::OK();
  };
  td::Status S = td::Status::OK();
  std::vector<BlockIdExt> top_shard_blocks;
  shard_config->process_shard_hashes([&](block::McShardHash &shard) {
    if (!opts_->need_monitor(shard.shard(), shard_client_state_)) {
      return 0;
    }
    S = dfs(shard.top_block_id());
    top_shard_blocks.push_back(shard.top_block_id());
    if (S.is_error()) {
      return -1;
    }
    return 0;
  });
  if (S.is_error()) {
    LOG(DEBUG) << "Cannot advance shard client seqno to " << new_shard_client_seqno_ + 1 << " : " << S;
    processed_shard_blocks();
    return;
  }
  shard_configs_[mc_block->block_id().seqno()] = {mc_block->block_id(), std::move(top_shard_blocks)};
  ++new_shard_client_seqno_;
  LOG(DEBUG) << "Advancing shard client seqno to " << new_shard_client_seqno_;
  for (const BlockIdExt &block_id : blocks_to_import) {
    blocks_[block_id].import = true;
  }
  td::actor::send_closure(actor_id(this), &ArchiveImporterLocal::try_advance_shard_client_seqno);
}

void ArchiveImporterLocal::processed_shard_blocks() {
  if (new_shard_client_seqno_ == shard_client_seqno_) {
    LOG(INFO) << "No new shard blocks";
  } else {
    LOG(INFO) << "New shard client seqno = " << new_shard_client_seqno_;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  for (const BlockIdExt &block_id : new_zerostates_) {
    LOG(INFO) << "Downloading zerostate " << block_id.to_str();
    td::actor::create_actor<DownloadShardState>(
        "downloadstate", block_id, shard_client_state_->get_block_id(),
        shard_client_state_->persistent_state_split_depth(block_id.id.workchain), 2, manager_, td::Timestamp::in(3600),
        ig.get_promise().wrap([](td::Ref<ShardState> &&) { return td::Unit(); }))
        .release();
  }
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::store_data);
    }
  });
}

void ArchiveImporterLocal::store_data() {
  td::MultiPromise mp;
  auto ig = mp.init_guard();

  if (opts_->get_permanent_celldb()) {
    std::vector<td::Ref<BlockData>> blocks;
    for (auto &[_, info] : blocks_) {
      if (info.import) {
        blocks.push_back(info.block);
      }
    }
    td::actor::send_closure(manager_, &ValidatorManager::set_block_state_from_data_preliminary, std::move(blocks),
                            ig.get_promise());
  }
  for (auto &[block_id, info] : blocks_) {
    if (info.import) {
      td::actor::send_closure(
          manager_, &ValidatorManager::get_block_handle, block_id, true,
          [promise = ig.get_promise(), block = info.block, manager = manager_](td::Result<BlockHandle> R) mutable {
            TRY_RESULT_PROMISE(promise, handle, std::move(R));
            td::actor::send_closure(manager, &ValidatorManager::set_block_data, handle, std::move(block),
                                    std::move(promise));
          });
      if (info.proof_link.not_null()) {
        run_check_proof_link_query(block_id, info.proof_link, manager_, td::Timestamp::in(600.0),
                                   ig.get_promise().wrap([](BlockHandle &&) { return td::Unit(); }));
      }
    }
  }

  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::apply_next_masterchain_block);
    }
  });
}

void ArchiveImporterLocal::apply_next_masterchain_block() {
  auto it = masterchain_blocks_.find(last_masterchain_state_->get_seqno() + 1);
  if (it == masterchain_blocks_.end()) {
    LOG(INFO) << "Applied masterchain blocks, last seqno = " << last_masterchain_state_->get_seqno();
    apply_shard_blocks();
    return;
  }
  BlockIdExt block_id = it->second;
  LOG(DEBUG) << "Applying masterchain block " << block_id.to_str();
  BlockInfo &info = blocks_[block_id];
  run_apply_block_query(block_id, info.block, block_id, manager_, td::Timestamp::in(600.0),
                        [=, SelfId = actor_id(this), manager = manager_](td::Result<td::Unit> R) {
                          if (R.is_error()) {
                            td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
                            return;
                          }
                          td::actor::send_closure(
                              manager, &ValidatorManager::get_shard_state_from_db_short, block_id,
                              [=](td::Result<td::Ref<ShardState>> R2) {
                                if (R2.is_error()) {
                                  td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query,
                                                          R2.move_as_error());
                                  return;
                                }
                                td::actor::send_closure(SelfId, &ArchiveImporterLocal::applied_next_masterchain_block,
                                                        td::Ref<MasterchainState>{R2.move_as_ok()});
                              });
                        });
}

void ArchiveImporterLocal::applied_next_masterchain_block(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = state;
  imported_any_ = true;
  LOG(DEBUG) << "Applied masterchain block " << state->get_block_id().to_str();
  apply_next_masterchain_block();
}

void ArchiveImporterLocal::apply_shard_blocks() {
  if (current_shard_client_seqno_ == new_shard_client_seqno_) {
    finish_query();
    return;
  }
  auto it = shard_configs_.find(current_shard_client_seqno_ + 1);
  if (it == shard_configs_.end()) {
    abort_query(td::Status::Error("no shard config for the next shard client seqno"));
    return;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  BlockIdExt mc_block_id = it->second.first;
  LOG(DEBUG) << "Applying top shard blocks from " << current_shard_client_seqno_ + 1;
  for (const BlockIdExt &block_id : it->second.second) {
    apply_shard_block(block_id, mc_block_id, ig.get_promise());
  }

  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporterLocal::abort_query, R.move_as_error());
      return;
    }
    td::actor::send_closure(SelfId, &ArchiveImporterLocal::applied_shard_blocks);
  });
}

void ArchiveImporterLocal::applied_shard_blocks() {
  LOG(DEBUG) << "Applied top shard blocks from " << current_shard_client_seqno_ + 1;
  ++current_shard_client_seqno_;
  imported_any_ = true;
  apply_shard_blocks();
}

void ArchiveImporterLocal::apply_shard_block(BlockIdExt block_id, BlockIdExt mc_block_id,
                                             td::Promise<td::Unit> promise) {
  td::actor::send_closure(
      manager_, &ValidatorManager::get_block_handle, block_id, true,
      [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ArchiveImporterLocal::apply_shard_block_cont1, R.move_as_ok(), mc_block_id,
                                std::move(promise));
      });
}

void ArchiveImporterLocal::apply_shard_block_cont1(BlockHandle handle, BlockIdExt mc_block_id,
                                                   td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    promise.set_value(td::Unit());
    return;
  }

  promise = [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    TRY_STATUS_PROMISE(promise, R.move_as_status());
    td::actor::send_closure(SelfId, &ArchiveImporterLocal::apply_shard_block_cont2, handle, mc_block_id,
                            std::move(promise));
  };

  if (!handle->merge_before() && handle->one_prev(true).shard_full() == handle->id().shard_full()) {
    apply_shard_block(handle->one_prev(true), mc_block_id, std::move(promise));
  } else {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise(std::move(promise));
    check_shard_block_applied(handle->one_prev(true), ig.get_promise());
    if (handle->merge_before()) {
      check_shard_block_applied(handle->one_prev(false), ig.get_promise());
    }
  }
}

void ArchiveImporterLocal::apply_shard_block_cont2(BlockHandle handle, BlockIdExt mc_block_id,
                                                   td::Promise<td::Unit> promise) {
  td::Ref<BlockData> block = blocks_[handle->id()].block;
  CHECK(block.not_null());
  LOG(DEBUG) << "Applying shard block " << handle->id().to_str();
  run_apply_block_query(handle->id(), std::move(block), mc_block_id, manager_, td::Timestamp::in(600.0),
                        std::move(promise));
}

void ArchiveImporterLocal::check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, false,
                          [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
                            TRY_RESULT_PROMISE(promise, handle, std::move(R));
                            if (!handle->is_applied()) {
                              promise.set_error(td::Status::Error(ErrorCode::notready, "not applied"));
                            } else {
                              promise.set_value(td::Unit());
                            }
                          });
}

void ArchiveImporterLocal::abort_query(td::Status error) {
  if (!imported_any_) {
    LOG(ERROR) << "Archive import: " << error;
    promise_.set_error(std::move(error));
    stop();
  } else {
    LOG(WARNING) << "Archive import: " << error;
    finish_query();
  }
}

void ArchiveImporterLocal::finish_query() {
  LOG(WARNING) << "Imported archive in " << perf_timer_.elapsed()
               << "s : mc_seqno=" << last_masterchain_state_->get_seqno()
               << " shard_seqno=" << current_shard_client_seqno_;
  promise_.set_value({last_masterchain_state_->get_seqno(),
                      std::min<BlockSeqno>(last_masterchain_state_->get_seqno(), current_shard_client_seqno_)});
  stop();
}

}  // namespace validator

}  // namespace ton
