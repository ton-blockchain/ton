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

    Copyright 2019-2020 Telegram Systems LLP
*/
#include "import-db-slice.hpp"

#include "validator/db/fileref.hpp"
#include "td/utils/overloaded.h"
#include "validator/fabric.h"
#include "td/actor/MultiPromise.h"
#include "common/checksum.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"
#include "downloaders/download-state.hpp"

#include <delay.h>

namespace ton {

namespace validator {

ArchiveImporter::ArchiveImporter(std::string db_root, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                                 td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                                 std::vector<std::string> to_import_files,
                                 td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise)
    : db_root_(std::move(db_root))
    , last_masterchain_state_(std::move(state))
    , shard_client_seqno_(shard_client_seqno)
    , start_import_seqno_(shard_client_seqno + 1)
    , opts_(std::move(opts))
    , manager_(manager)
    , to_import_files_(std::move(to_import_files))
    , use_imported_files_(!to_import_files_.empty())
    , promise_(std::move(promise)) {
}

void ArchiveImporter::start_up() {
  if (use_imported_files_) {
    LOG(INFO) << "Importing archive for masterchain seqno #" << start_import_seqno_ << " from disk";
    for (const std::string& path : to_import_files_) {
      LOG(INFO) << "Importing file from disk " << path;
      td::Status S = process_package(path, true);
      if (S.is_error()) {
        LOG(INFO) << "Error processing package " << path << ": " << S;
      }
    }
    files_to_cleanup_.clear();
    processed_mc_archive();
    return;
  }
  LOG(INFO) << "Importing archive for masterchain seqno #" << start_import_seqno_ << " from net";
  td::actor::send_closure(manager_, &ValidatorManager::send_download_archive_request, start_import_seqno_,
                          ShardIdFull{masterchainId}, db_root_ + "/tmp/", td::Timestamp::in(3600.0),
                          [SelfId = actor_id(this)](td::Result<std::string> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &ArchiveImporter::abort_query, R.move_as_error());
                            } else {
                              td::actor::send_closure(SelfId, &ArchiveImporter::downloaded_mc_archive, R.move_as_ok());
                            }
                          });
}

void ArchiveImporter::downloaded_mc_archive(std::string path) {
  td::Status S = process_package(path, true);
  if (S.is_error()) {
    abort_query(std::move(S));
    return;
  }
  processed_mc_archive();
}

void ArchiveImporter::processed_mc_archive() {
  if (masterchain_blocks_.empty()) {
    LOG(DEBUG) << "No masterhchain blocks in archive";
    last_masterchain_seqno_ = last_masterchain_state_->get_seqno();
    checked_all_masterchain_blocks();
    return;
  }

  auto seqno = masterchain_blocks_.begin()->first;
  LOG(DEBUG) << "First mc seqno in archive = " << seqno;
  if (seqno > last_masterchain_state_->get_seqno() + 1) {
    abort_query(td::Status::Error(ErrorCode::notready, "too big first masterchain seqno"));
    return;
  }

  check_masterchain_block(seqno);
}

td::Status ArchiveImporter::process_package(std::string path, bool with_masterchain) {
  LOG(DEBUG) << "Processing package " << path << " (with_masterchain=" << with_masterchain << ")";
  files_to_cleanup_.push_back(path);
  TRY_RESULT(p, Package::open(path, false, false));
  auto package = std::make_shared<Package>(std::move(p));

  td::Status S = td::Status::OK();
  package->iterate([&](std::string filename, td::BufferSlice, td::uint64 offset) -> bool {
    auto F = FileReference::create(filename);
    if (F.is_error()) {
      S = F.move_as_error();
      return false;
    }
    auto f = F.move_as_ok();

    BlockIdExt b;
    bool is_proof = false;
    bool ignore = true;

    f.ref().visit(td::overloaded(
        [&](const fileref::Proof &p) {
          b = p.block_id;
          ignore = !b.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::ProofLink &p) {
          b = p.block_id;
          ignore = b.is_masterchain();
          is_proof = true;
        },
        [&](const fileref::Block &p) {
          b = p.block_id;
          ignore = false;
          is_proof = false;
        },
        [&](const auto &) { ignore = true; }));

    if (!ignore && (with_masterchain || !b.is_masterchain())) {
      if (is_proof) {
        blocks_[b].proof_pkg = package;
        blocks_[b].proof_offset = offset;
      } else {
        blocks_[b].data_pkg = package;
        blocks_[b].data_offset = offset;
      }
      if (b.is_masterchain()) {
        masterchain_blocks_[b.seqno()] = b;
        last_masterchain_seqno_ = std::max(last_masterchain_seqno_, b.seqno());
      } else {
        have_shard_blocks_ = true;
      }
    }
    return true;
  });
  return S;
}

void ArchiveImporter::check_masterchain_block(BlockSeqno seqno) {
  auto it = masterchain_blocks_.find(seqno);
  if (it == masterchain_blocks_.end()) {
    if (seqno == 0) {
      abort_query(td::Status::Error(ErrorCode::notready, "no new blocks"));
      return;
    }
    checked_all_masterchain_blocks();
    return;
  }
  while (seqno <= last_masterchain_state_->get_block_id().seqno()) {
    if (seqno < last_masterchain_state_->get_block_id().seqno()) {
      if (!last_masterchain_state_->check_old_mc_block_id(it->second)) {
        abort_query(td::Status::Error(ErrorCode::protoviolation, "bad old masterchain block id"));
        return;
      }
    } else {
      if (last_masterchain_state_->get_block_id() != it->second) {
        abort_query(td::Status::Error(ErrorCode::protoviolation, "bad old masterchain block id"));
        return;
      }
    }
    seqno++;
    it = masterchain_blocks_.find(seqno);
    if (it == masterchain_blocks_.end()) {
      checked_all_masterchain_blocks();
      return;
    }
  }
  LOG(DEBUG) << "Checking masterchain block #" << seqno;
  if (seqno != last_masterchain_state_->get_block_id().seqno() + 1) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "hole in masterchain seqno"));
    return;
  }
  auto it2 = blocks_.find(it->second);
  CHECK(it2 != blocks_.end());
  if (!it2->second.proof_pkg) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block proof"));
    return;
  }
  if (!it2->second.data_pkg) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "no masterchain block data"));
    return;
  }

  auto R1 = it2->second.proof_pkg->read(it2->second.proof_offset);
  if (R1.is_error()) {
    abort_query(R1.move_as_error());
    return;
  }

  auto proofR = create_proof(it->second, std::move(R1.move_as_ok().second));
  if (proofR.is_error()) {
    abort_query(proofR.move_as_error());
    return;
  }

  auto R2 = it2->second.data_pkg->read(it2->second.data_offset);
  if (R2.is_error()) {
    abort_query(R2.move_as_error());
    return;
  }

  if (sha256_bits256(R2.ok().second.as_slice()) != it->second.file_hash) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "bad block file hash"));
    return;
  }
  auto dataR = create_block(it->second, std::move(R2.move_as_ok().second));
  if (dataR.is_error()) {
    abort_query(dataR.move_as_error());
    return;
  }

  auto proof = proofR.move_as_ok();
  auto data = dataR.move_as_ok();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id = last_masterchain_state_->get_block_id(),
                                       data](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporter::abort_query, R.move_as_error());
      return;
    }
    auto handle = R.move_as_ok();
    CHECK(!handle->merge_before());
    if (handle->one_prev(true) != id) {
      td::actor::send_closure(SelfId, &ArchiveImporter::abort_query,
                              td::Status::Error(ErrorCode::protoviolation, "prev block mismatch"));
      return;
    }
    td::actor::send_closure(SelfId, &ArchiveImporter::checked_masterchain_proof, std::move(handle), std::move(data));
  });

  run_check_proof_query(it->second, std::move(proof), manager_, td::Timestamp::in(2.0), std::move(P),
                        last_masterchain_state_, opts_->is_hardfork(it->second));
}

void ArchiveImporter::checked_masterchain_proof(BlockHandle handle, td::Ref<BlockData> data) {
  LOG(DEBUG) << "Checked proof for masterchain block #" << handle->id().seqno();
  CHECK(data.not_null());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveImporter::applied_masterchain_block, std::move(handle));
  });
  run_apply_block_query(handle->id(), std::move(data), handle->id(), manager_, td::Timestamp::in(600.0), std::move(P));
}

void ArchiveImporter::applied_masterchain_block(BlockHandle handle) {
  LOG(DEBUG) << "Applied masterchain block #" << handle->id().seqno();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveImporter::got_new_materchain_state,
                            td::Ref<MasterchainState>(R.move_as_ok()));
  });
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db, handle, std::move(P));
}

void ArchiveImporter::got_new_materchain_state(td::Ref<MasterchainState> state) {
  last_masterchain_state_ = std::move(state);
  imported_any_ = true;
  check_masterchain_block(last_masterchain_state_->get_block_id().seqno() + 1);
}

void ArchiveImporter::checked_all_masterchain_blocks() {
  LOG(DEBUG) << "Done importing masterchain blocks. Last block seqno = " << last_masterchain_seqno_;
  if (start_import_seqno_ > last_masterchain_state_->get_seqno()) {
    abort_query(td::Status::Error("no new masterchain blocks were imported"));
    return;
  }
  BlockIdExt block_id;
  CHECK(last_masterchain_state_->get_old_mc_block_id(start_import_seqno_, block_id));
  td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id,
                          [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                            R.ensure();
                            td::Ref<MasterchainState> state{R.move_as_ok()};
                            td::actor::send_closure(SelfId, &ArchiveImporter::download_shard_archives,
                                                    std::move(state));
                          });
}

void ArchiveImporter::download_shard_archives(td::Ref<MasterchainState> start_state) {
  start_state_ = start_state;
  td::uint32 monitor_min_split = start_state->monitor_min_split_depth(basechainId);
  LOG(DEBUG) << "Monitor min split = " << monitor_min_split;
  // If monitor_min_split == 0, we use the old archive format (packages are not separated by shard)
  // If masterchain package has shard blocks then it's old archive format, don't need to download shards
  if (monitor_min_split > 0 && !have_shard_blocks_ && !use_imported_files_) {
    for (td::uint64 i = 0; i < (1ULL << monitor_min_split); ++i) {
      ShardIdFull shard_prefix{basechainId, (i * 2 + 1) << (64 - monitor_min_split - 1)};
      if (opts_->need_monitor(shard_prefix, start_state)) {
        ++pending_shard_archives_;
        LOG(DEBUG) << "Downloading shard archive #" << start_import_seqno_ << " " << shard_prefix.to_str();
        download_shard_archive(shard_prefix);
      }
    }
  } else {
    LOG(DEBUG) << "Skip downloading shard archives";
  }
  if (pending_shard_archives_ == 0) {
    check_next_shard_client_seqno(shard_client_seqno_ + 1);
  }
}

void ArchiveImporter::download_shard_archive(ShardIdFull shard_prefix) {
  td::actor::send_closure(
      manager_, &ValidatorManager::send_download_archive_request, start_import_seqno_, shard_prefix, db_root_ + "/tmp/",
      td::Timestamp::in(3600.0),
      [SelfId = actor_id(this), seqno = start_import_seqno_, shard_prefix](td::Result<std::string> R) {
        if (R.is_error()) {
          LOG(WARNING) << "Failed to download archive slice #" << seqno << " for shard " << shard_prefix.to_str();
          delay_action(
              [=]() { td::actor::send_closure(SelfId, &ArchiveImporter::download_shard_archive, shard_prefix); },
              td::Timestamp::in(2.0));
        } else {
          LOG(DEBUG) << "Downloaded shard archive #" << seqno << " " << shard_prefix.to_str();
          td::actor::send_closure(SelfId, &ArchiveImporter::downloaded_shard_archive, R.move_as_ok());
        }
      });
}

void ArchiveImporter::downloaded_shard_archive(std::string path) {
  td::Status S = process_package(path, false);
  if (S.is_error()) {
    LOG(INFO) << "Error processing package: " << S;
  }
  --pending_shard_archives_;
  if (pending_shard_archives_ == 0) {
    check_next_shard_client_seqno(shard_client_seqno_ + 1);
  }
}

void ArchiveImporter::check_next_shard_client_seqno(BlockSeqno seqno) {
  if (seqno > last_masterchain_state_->get_seqno() || seqno > last_masterchain_seqno_) {
    finish_query();
  } else if (seqno == last_masterchain_state_->get_seqno()) {
    got_masterchain_state(last_masterchain_state_);
  } else {
    BlockIdExt b;
    bool f = last_masterchain_state_->get_old_mc_block_id(seqno, b);
    CHECK(f);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ArchiveImporter::got_masterchain_state,
                              td::Ref<MasterchainState>{R.move_as_ok()});
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_shard_state_from_db_short, b, std::move(P));
  }
}

void ArchiveImporter::got_masterchain_state(td::Ref<MasterchainState> state) {
  if (state->get_seqno() != start_import_seqno_ && state->is_key_state()) {
    finish_query();
    return;
  }
  LOG(DEBUG) << "Applying shard client seqno " << state->get_seqno();
  auto s = state->get_shards();
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  for (auto &shard : s) {
    if (opts_->need_monitor(shard->shard(), state)) {
      apply_shard_block(shard->top_block_id(), state->get_block_id(), ig.get_promise());
    }
  }
  ig.add_promise([SelfId = actor_id(this), seqno = state->get_seqno()](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveImporter::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporter::checked_shard_client_seqno, seqno);
    }
  });
}

void ArchiveImporter::checked_shard_client_seqno(BlockSeqno seqno) {
  CHECK(shard_client_seqno_ + 1 == seqno);
  shard_client_seqno_++;
  imported_any_ = true;
  check_next_shard_client_seqno(seqno + 1);
}

void ArchiveImporter::apply_shard_block(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                        td::Promise<td::Unit> promise) {
  LOG(DEBUG) << "Applying shard block " << block_id.id.to_str();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), masterchain_block_id, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont1, R.move_as_ok(), masterchain_block_id,
                                std::move(promise));
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, true, std::move(P));
}

void ArchiveImporter::apply_shard_block_cont1(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    promise.set_value(td::Unit());
    return;
  }

  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda(
        [promise = std::move(promise)](td::Result<td::Ref<ShardState>>) mutable { promise.set_value(td::Unit()); });
    td::actor::create_actor<DownloadShardState>("downloadstate", handle->id(), masterchain_block_id, 2, manager_,
                                                td::Timestamp::in(3600), std::move(P))
        .release();
    return;
  }

  auto it = blocks_.find(handle->id());
  if (it == blocks_.end() || !it->second.proof_pkg || !it->second.data_pkg) {
    promise.set_error(
        td::Status::Error(ErrorCode::notready, PSTRING() << "no data/proof for shard block " << handle->id()));
    return;
  }
  TRY_RESULT_PROMISE(promise, proof_data, it->second.proof_pkg->read(it->second.proof_offset));
  TRY_RESULT_PROMISE(promise, proof, create_proof_link(handle->id(), std::move(proof_data.second)));
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, masterchain_block_id,
                                       promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont2, std::move(handle),
                              masterchain_block_id, std::move(promise));
    }
  });
  run_check_proof_link_query(handle->id(), std::move(proof), manager_, td::Timestamp::in(10.0), std::move(P));
}

void ArchiveImporter::apply_shard_block_cont2(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    promise.set_value(td::Unit());
    return;
  }
  CHECK(handle->id().seqno() > 0);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, masterchain_block_id,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveImporter::apply_shard_block_cont3, std::move(handle),
                              masterchain_block_id, std::move(promise));
    }
  });
  if (!handle->merge_before() && handle->one_prev(true).shard_full() == handle->id().shard_full()) {
    apply_shard_block(handle->one_prev(true), masterchain_block_id, std::move(P));
  } else {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise(std::move(P));
    check_shard_block_applied(handle->one_prev(true), ig.get_promise());
    if (handle->merge_before()) {
      check_shard_block_applied(handle->one_prev(false), ig.get_promise());
    }
  }
}

void ArchiveImporter::apply_shard_block_cont3(BlockHandle handle, BlockIdExt masterchain_block_id,
                                              td::Promise<td::Unit> promise) {
  auto it = blocks_.find(handle->id());
  CHECK(it != blocks_.end() && it->second.data_pkg);
  TRY_RESULT_PROMISE(promise, data, it->second.data_pkg->read(it->second.data_offset));
  if (sha256_bits256(data.second.as_slice()) != handle->id().file_hash) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad block file hash"));
    return;
  }
  TRY_RESULT_PROMISE(promise, block, create_block(handle->id(), std::move(data.second)));

  run_apply_block_query(handle->id(), std::move(block), masterchain_block_id, manager_, td::Timestamp::in(600.0),
                        std::move(promise));
}

void ArchiveImporter::check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          if (!handle->is_applied()) {
            promise.set_error(td::Status::Error(ErrorCode::notready, "not applied"));
          } else {
            LOG(DEBUG) << "Applied shard block " << handle->id().id.to_str();
            promise.set_value(td::Unit());
          }
        }
      });
  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, block_id, false, std::move(P));
}

void ArchiveImporter::abort_query(td::Status error) {
  if (!imported_any_) {
    for (const std::string &f : files_to_cleanup_) {
      td::unlink(f).ignore();
    }
    promise_.set_error(std::move(error));
    stop();
    return;
  }
  LOG(INFO) << "Archive import: " << error;
  finish_query();
}

void ArchiveImporter::finish_query() {
  for (const std::string &f : files_to_cleanup_) {
    td::unlink(f).ignore();
  }
  if (promise_) {
    promise_.set_value({last_masterchain_state_->get_seqno(),
                        std::min<BlockSeqno>(last_masterchain_state_->get_seqno(), shard_client_seqno_)});
  }
  stop();
}

}  // namespace validator

}  // namespace ton
