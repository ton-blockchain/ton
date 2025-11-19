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
#include "block/block-auto.h"
#include "common/checksum.h"
#include "downloaders/download-state.hpp"
#include "td/actor/MultiPromise.h"
#include "td/actor/coro_utils.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/path.h"
#include "validator/db/fileref.hpp"
#include "validator/db/package.hpp"
#include "validator/fabric.h"

#include "import-db-slice-local.hpp"

namespace ton {

namespace validator {

ArchiveImporterLocal::ArchiveImporterLocal(std::string db_root, td::Ref<MasterchainState> state,
                                           BlockSeqno shard_client_seqno, td::Ref<ValidatorManagerOptions> opts,
                                           td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<Db> db,
                                           std::vector<std::string> to_import_files,
                                           td::Promise<std::pair<BlockSeqno, BlockSeqno>> promise)
    : db_root_(std::move(db_root))
    , last_masterchain_state_(std::move(state))
    , shard_client_seqno_(shard_client_seqno)
    , opts_(std::move(opts))
    , manager_(manager)
    , db_(db)
    , to_import_files_(std::move(to_import_files))
    , promise_(std::move(promise))
    , perf_timer_("import-slice-local", 10.0, [manager](double duration) {
      send_closure(manager, &ValidatorManager::add_perf_timer_stat, "import-slice-local", duration);
    }) {
}

void ArchiveImporterLocal::start_up() {
  run().start().detach();
}

td::actor::Task<td::Unit> ArchiveImporterLocal::run() {
  auto R = co_await run_inner().wrap();
  if (R.is_error()) {
    LOG(ERROR) << "Archive import: " << R.error();
    if (!imported_any_) {
      promise_.set_error(R.move_as_error());
      stop();
      co_return td::Unit{};
    }
  }
  LOG(WARNING) << "Imported archive in " << perf_timer_.elapsed() << "s : mc_seqno=" << final_masterchain_state_seqno_
               << " shard_seqno=" << final_shard_client_seqno_;
  promise_.set_value({final_masterchain_state_seqno_,
                      std::min<BlockSeqno>(final_masterchain_state_seqno_, final_shard_client_seqno_)});
  stop();
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::run_inner() {
  LOG(WARNING) << "Importing archive for masterchain seqno #" << shard_client_seqno_ + 1 << " from disk";
  read_files();
  co_await process_masterchain_blocks();
  co_await process_shard_blocks();
  co_await store_data();
  co_await apply_blocks();
  co_return td::Unit{};
}

void ArchiveImporterLocal::read_files() {
  for (const std::string &path : to_import_files_) {
    LOG(INFO) << "Importing file from disk " << path;
    td::Status S = process_package(path);
    if (S.is_error()) {
      LOG(WARNING) << "Error processing package " << path << ": " << S;
    }
  }
}

td::Status ArchiveImporterLocal::process_package(std::string path) {
  TRY_RESULT(p, Package::open(path, false, false));
  auto package = std::make_shared<Package>(std::move(p));

  td::Status S = td::Status::OK();
  package->iterate([&](std::string filename, td::BufferSlice data, td::uint64) -> bool {
    auto F = FileReference::create(filename);
    if (F.is_error()) {
      return true;
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

    if (ignore || (block_id.is_masterchain() && block_id.seqno() <= last_masterchain_state_->get_seqno())) {
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

td::actor::Task<td::Unit> ArchiveImporterLocal::process_masterchain_blocks() {
  final_masterchain_state_seqno_ = last_masterchain_state_->get_seqno();
  final_shard_client_seqno_ = shard_client_seqno_;
  if (masterchain_blocks_.empty()) {
    LOG(INFO) << "No masterchain blocks in the archive";
    co_return td::Unit{};
  }

  if (masterchain_blocks_.begin()->first != last_masterchain_state_->get_seqno() + 1) {
    co_return td::Status::Error(ErrorCode::notready, PSTRING() << "expected masterchain seqno "
                                                               << last_masterchain_state_->get_seqno() + 1 << ", found "
                                                               << masterchain_blocks_.begin()->first);
  }
  {
    BlockSeqno expected_seqno = last_masterchain_state_->get_seqno() + 1;
    for (auto &[seqno, _] : masterchain_blocks_) {
      if (seqno != expected_seqno) {
        co_return td::Status::Error(ErrorCode::protoviolation, "non-consecutive masterchain blocks in the archive");
      }
      ++expected_seqno;
    }
  }
  BlockInfo &first_block = blocks_[masterchain_blocks_.begin()->second];
  if (first_block.proof.is_null()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "no masterchain block proof");
  }
  if (first_block.block.is_null()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "no masterchain block data");
  }
  block::gen::Block::Record rec;
  block::gen::BlockInfo::Record info;
  if (!(block::gen::unpack_cell(first_block.block->root_cell(), rec) && block::gen::unpack_cell(rec.info, info))) {
    co_return td::Status::Error(ErrorCode::protoviolation, "cannot unpack masterchain block info");
  }
  if (info.key_block) {
    co_await import_first_key_block();
    if (masterchain_blocks_.empty()) {
      LOG(INFO) << "No more masterchain blocks in the archive";
      co_return td::Unit{};
    }
  }
  co_await check_masterchain_proofs();
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::import_first_key_block() {
  BlockIdExt block_id = masterchain_blocks_.begin()->second;
  BlockInfo &first_block = blocks_[block_id];
  LOG(INFO) << "First block in archive is key block : " << block_id.id.to_str();
  auto [task, promise] = td::actor::StartedTask<BlockHandle>::make_bridge();
  run_check_proof_query(block_id, first_block.proof, manager_, td::Timestamp::in(600.0), std::move(promise),
                        last_masterchain_state_, opts_->is_hardfork(block_id));
  BlockHandle handle = co_await std::move(task);
  CHECK(!handle->merge_before());
  CHECK(block_id == handle->id());
  if (handle->one_prev(true) != last_masterchain_state_->get_block_id()) {
    co_return td::Status::Error(ErrorCode::protoviolation, "prev block mismatch");
  }

  auto [task2, promise2] = td::actor::StartedTask<td::Unit>::make_bridge();
  run_apply_block_query(handle->id(), first_block.block, handle->id(), manager_, td::Timestamp::in(600.0),
                        std::move(promise2));
  co_await std::move(task2);
  auto state =
      td::Ref<MasterchainState>{co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, handle)};

  CHECK(state->get_block_id() == masterchain_blocks_.begin()->second);
  last_masterchain_state_ = state;
  final_masterchain_state_seqno_ = state->get_seqno();
  imported_any_ = true;
  masterchain_blocks_.erase(masterchain_blocks_.begin());
  blocks_.erase(state->get_block_id());
  LOG(WARNING) << "Imported key block " << state->get_block_id().id.to_str();
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::check_masterchain_proofs() {
  LOG(INFO) << "Checking masterchain blocks from " << masterchain_blocks_.begin()->first << " to "
            << masterchain_blocks_.rbegin()->first;

  std::vector<td::actor::StartedTask<BlockHandle>> tasks;

  for (auto &[_, block_id] : masterchain_blocks_) {
    auto &info = blocks_[block_id];
    if (info.proof.is_null()) {
      co_return td::Status::Error(ErrorCode::protoviolation, "no masterchain block proof");
    }
    if (info.block.is_null()) {
      co_return td::Status::Error(ErrorCode::protoviolation, "no masterchain block data");
    }
    auto [task, promise] = td::actor::StartedTask<BlockHandle>::make_bridge();
    run_check_proof_query(block_id, info.proof, manager_, td::Timestamp::in(600.0), std::move(promise),
                          last_masterchain_state_, opts_->is_hardfork(block_id));
    tasks.push_back(std::move(task));
  }

  auto handles = co_await td::actor::all(std::move(tasks));
  BlockIdExt prev_block_id = last_masterchain_state_->get_block_id();
  size_t i = 0;
  for (auto &[_, block_id] : masterchain_blocks_) {
    CHECK(i < handles.size());
    CHECK(!handles[i]->merge_before());
    if (handles[i]->one_prev(true) != prev_block_id) {
      co_return td::Status::Error(ErrorCode::protoviolation, "prev block mismatch");
    }
    blocks_to_apply_mc_.emplace_back(block_id, block_id);
    blocks_[block_id].import = true;
    CHECK(handles[i]->inited_is_key_block());
    if (handles[i]->is_key_block()) {
      co_return td::Status::Error(ErrorCode::protoviolation,
                                  "package contains a key block, and it is not the first block");
    }
    prev_block_id = block_id;
    ++i;
  }
  LOG(INFO) << "Checked proofs for masterchain blocks";
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::process_shard_blocks() {
  td::Ref<MasterchainState> state;
  if (shard_client_seqno_ == last_masterchain_state_->get_seqno()) {
    state = last_masterchain_state_;
  } else {
    CHECK(shard_client_seqno_ < last_masterchain_state_->get_seqno());
    BlockIdExt block_id;
    if (!last_masterchain_state_->get_old_mc_block_id(shard_client_seqno_, block_id)) {
      co_return td::Status::Error("failed to get shard client block id");
    }
    state = td::Ref<MasterchainState>{
        co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id)};
  }
  CHECK(state->get_seqno() == shard_client_seqno_);
  LOG(DEBUG) << "got_shard_client_state " << shard_client_seqno_;
  shard_client_state_ = state;
  new_shard_client_seqno_ = shard_client_seqno_;
  for (auto &shard : state->get_shards()) {
    visited_shard_blocks_.insert(shard->top_block_id());
  }

  while (co_await try_advance_shard_client_seqno()) {
    LOG(DEBUG) << "advanced shard client seqno to " << new_shard_client_seqno_;
  }
  if (new_shard_client_seqno_ == shard_client_seqno_) {
    LOG(INFO) << "No new shard blocks, shard client seqno = " << new_shard_client_seqno_;
  } else {
    LOG(INFO) << "New shard client seqno = " << new_shard_client_seqno_;
  }

  for (const BlockIdExt &block_id : new_zerostates_) {
    LOG(INFO) << "Downloading zerostate " << block_id.to_str();
    auto [task, promise] = td::actor::StartedTask<td::Ref<ShardState>>::make_bridge();
    td::actor::create_actor<DownloadShardState>(
        "downloadstate", block_id, shard_client_state_->get_block_id(),
        shard_client_state_->persistent_state_split_depth(block_id.id.workchain), 2, manager_, td::Timestamp::in(3600),
        std::move(promise))
        .release();
    co_await std::move(task);
  }
  co_return td::Unit{};
}

td::actor::Task<bool> ArchiveImporterLocal::try_advance_shard_client_seqno() {
  BlockSeqno seqno = new_shard_client_seqno_ + 1;
  auto it = masterchain_blocks_.find(seqno);
  if (it == masterchain_blocks_.end() && seqno > last_masterchain_state_->get_seqno()) {
    co_return false;
  }
  td::Ref<BlockData> mc_block;
  if (it != masterchain_blocks_.end()) {
    mc_block = blocks_[it->second].block;
  } else {
    BlockIdExt block_id;
    if (!last_masterchain_state_->get_old_mc_block_id(seqno, block_id)) {
      co_return td::Status::Error("failed to get old mc block id");
    }
    mc_block = co_await td::actor::ask(manager_, &ValidatorManager::get_block_data_from_db_short, block_id);
  }

  CHECK(mc_block.not_null() && mc_block->block_id().seqno() == new_shard_client_seqno_ + 1);
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

    std::vector<BlockIdExt> prev;
    BlockIdExt mc_blkid;
    bool after_split;
    TRY_STATUS(block::unpack_block_prev_blk_try(info.block->root_cell(), block_id, prev, mc_blkid, after_split));
    for (const BlockIdExt &prev_block_id : prev) {
      TRY_STATUS(dfs(prev_block_id));
    }
    blocks_to_import.push_back(block_id);
    return td::Status::OK();
  };
  td::Status S = td::Status::OK();
  std::vector<BlockIdExt> top_shard_blocks;
  shard_config->process_shard_hashes([&](const block::McShardHash &shard) {
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
    co_return false;
  }
  shard_configs_[mc_block->block_id().seqno()] = {mc_block->block_id(), std::move(top_shard_blocks)};
  ++new_shard_client_seqno_;
  LOG(DEBUG) << "Advancing shard client seqno to " << new_shard_client_seqno_;
  for (const BlockIdExt &block_id : blocks_to_import) {
    blocks_to_apply_shards_.emplace_back(block_id, mc_block->block_id());
    blocks_[block_id].import = true;
  }
  co_return true;
}

td::actor::Task<td::Unit> ArchiveImporterLocal::store_data() {
  std::vector<td::actor::StartedTask<td::Unit>> tasks;
  if (opts_->get_permanent_celldb()) {
    std::vector<td::Ref<BlockData>> blocks;
    for (auto &[block_id, info] : blocks_) {
      if (info.import) {
        blocks.push_back(info.block);
      }
    }
    tasks.push_back(td::actor::ask(manager_, &ValidatorManager::set_block_state_from_data_bulk, std::move(blocks)));
  }
  for (auto &[block_id, info] : blocks_) {
    if (!info.import) {
      continue;
    }
    tasks.push_back(store_block_data(info.block).start());
    if (info.proof_link.not_null()) {
      auto [task, promise] = td::actor::StartedTask<BlockHandle>::make_bridge();
      run_check_proof_link_query(block_id, info.proof_link, manager_, td::Timestamp::in(600.0), std::move(promise));
      tasks.push_back(
          std::move(task).then([](BlockHandle &&) -> td::actor::Task<td::Unit> { co_return td::Unit{}; }).start());
    }
  }
  co_await td::actor::all(std::move(tasks));
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::store_block_data(td::Ref<BlockData> block) {
  BlockHandle handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, block->block_id(), true);
  co_await td::actor::ask(manager_, &ValidatorManager::set_block_data, handle, block);
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::apply_blocks() {
  LOG(WARNING) << "Applying " << blocks_to_apply_mc_.size() << " mc blocks, " << blocks_to_apply_shards_.size()
               << " shard blocks";
  if (opts_->get_permanent_celldb()) {
    co_await apply_blocks_async(blocks_to_apply_mc_);
    final_masterchain_state_seqno_ =
        blocks_to_apply_mc_.empty() ? last_masterchain_state_->get_seqno() : blocks_to_apply_mc_.back().first.seqno();
    if (!blocks_to_apply_mc_.empty()) {
      imported_any_ = true;
    }
    co_await apply_blocks_async(blocks_to_apply_shards_);
    if (!blocks_to_apply_shards_.empty()) {
      imported_any_ = true;
    }
  } else {
    for (const auto &[block_id, _] : blocks_to_apply_mc_) {
      auto it = blocks_.find(block_id);
      CHECK(it != blocks_.end());
      td::Ref<BlockData> block = it->second.block;
      CHECK(block.not_null());
      auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
      run_apply_block_query(block_id, block, block_id, manager_, td::Timestamp::in(600.0), std::move(promise));
      co_await std::move(task);
      imported_any_ = true;
      final_masterchain_state_seqno_ = block_id.seqno();
    }
    for (const auto &[block_id, mc_block_id] : blocks_to_apply_shards_) {
      auto it = blocks_.find(block_id);
      CHECK(it != blocks_.end());
      td::Ref<BlockData> block = it->second.block;
      CHECK(block.not_null());
      auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
      run_apply_block_query(block_id, block, mc_block_id, manager_, td::Timestamp::in(600.0), std::move(promise));
      co_await std::move(task);
      imported_any_ = true;
    }
  }
  final_shard_client_seqno_ = new_shard_client_seqno_;
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::apply_blocks_async(
    const std::vector<std::pair<BlockIdExt, BlockIdExt>> &blocks) {
  std::vector<td::actor::StartedTask<BlockHandle>> tasks_1;
  for (const auto &[block_id, mc_block_id] : blocks) {
    tasks_1.push_back(apply_block_async_1(block_id, mc_block_id).start());
  }
  auto handles = co_await td::actor::all(std::move(tasks_1));

  std::vector<td::actor::StartedTask<td::Unit>> tasks_2;
  for (const auto &handle : handles) {
    tasks_2.push_back(apply_block_async_2(handle).start());
  }
  co_await td::actor::all(std::move(tasks_2));

  std::vector<td::actor::StartedTask<td::Unit>> tasks_3;
  for (const auto &handle : handles) {
    tasks_3.push_back(apply_block_async_3(handle).start());
  }
  co_await td::actor::all(std::move(tasks_3));

  std::vector<td::actor::StartedTask<td::Unit>> tasks_4;
  for (const auto &handle : handles) {
    tasks_4.push_back(apply_block_async_4(handle).start());
  }
  co_await td::actor::all(std::move(tasks_4));

  co_return td::Unit{};
}

td::actor::Task<BlockHandle> ArchiveImporterLocal::apply_block_async_1(BlockIdExt block_id, BlockIdExt mc_block_id) {
  // apply_block_async_1-4 are same as ApplyBlock, but in parallel and without setting "apply" flag
  CHECK(block_id.seqno() != 0);
  auto it = blocks_.find(block_id);
  CHECK(it != blocks_.end());
  td::Ref<BlockData> block = it->second.block;
  CHECK(block.not_null());
  LOG(DEBUG) << "Applying block " << block_id.to_str() << ", mc_block_seqno=" << mc_block_id.to_str();
  BlockHandle handle = co_await td::actor::ask(manager_, &ValidatorManager::get_block_handle, block_id, true);

  CHECK(!handle->id().is_masterchain() || handle->inited_proof());
  CHECK(handle->id().is_masterchain() || handle->inited_proof_link());
  CHECK(handle->inited_merge_before());
  CHECK(handle->inited_split_after());
  CHECK(handle->inited_prev());
  CHECK(handle->inited_state_root_hash());
  CHECK(handle->inited_logical_time());

  td::Ref<ShardState> state =
      co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state, handle, 0, td::Timestamp::in(600.0), true);
  CHECK(handle->received_state());
  if (!handle->is_applied()) {
    co_await td::actor::ask(manager_, &ValidatorManager::set_next_block, handle->one_prev(true), block_id);
    if (handle->merge_before()) {
      co_await td::actor::ask(manager_, &ValidatorManager::set_next_block, handle->one_prev(false), block_id);
    }
  }

  if (!block_id.is_masterchain()) {
    handle->set_masterchain_ref_block(mc_block_id.seqno());
  } else {
    td::Ref<MasterchainState> mc_state{state};
    td::uint32 monitor_min_split = mc_state->monitor_min_split_depth(basechainId);
    td::actor::send_closure(db_, &Db::set_archive_current_shard_split_depth, monitor_min_split);
  }

  co_return handle;
}

td::actor::Task<td::Unit> ArchiveImporterLocal::apply_block_async_2(BlockHandle handle) {
  // Running add_handle in order is required to properly update ltdb in ArchiveSlice
  co_await td::actor::ask(db_, &Db::add_handle_to_archive, handle);
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::apply_block_async_3(BlockHandle handle) {
  td::Ref<ShardState> state = co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db, handle);
  co_await td::actor::ask(manager_, &ValidatorManager::new_block, handle, state);
  co_return td::Unit{};
}

td::actor::Task<td::Unit> ArchiveImporterLocal::apply_block_async_4(BlockHandle handle) {
  handle->set_applied();
  CHECK(handle->handle_moved_to_archive());
  CHECK(handle->moved_to_archive());
  if (handle->need_flush()) {
    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    handle->flush(manager_, handle, std::move(promise));
    co_await std::move(task);
  }
  co_return td::Unit{};
}

}  // namespace validator

}  // namespace ton
