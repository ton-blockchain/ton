/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more etails.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "ton/ton-tl.hpp"

#include "archiver.hpp"
#include "rootdb.hpp"

namespace ton {

namespace validator {

BlockArchiver::BlockArchiver(BlockHandle handle, td::actor::ActorId<ArchiveManager> archive_db,
                             td::actor::ActorId<Db> db, td::Promise<td::Unit> promise)
    : handle_(std::move(handle)), archive_(archive_db), db_(std::move(db)), promise_(std::move(promise)) {
}

void BlockArchiver::start_up() {
  run().start().detach();
}

td::actor::Task<> BlockArchiver::run() {
  auto result = co_await run_inner().wrap();
  if (result.is_error()) {
    VLOG(VALIDATOR_WARNING) << "failed to archive block " << handle_->id() << ": " << result.error();
  } else {
    VLOG(VALIDATOR_DEBUG) << "finished archiving block in " << timer_.elapsed() << " s";
  }
  promise_.set_result(std::move(result));
  stop();
  co_return {};
}

td::actor::Task<> BlockArchiver::run_inner() {
  VLOG(VALIDATOR_DEBUG) << "started block archiver for " << handle_->id().to_str();
  if (handle_->moved_to_archive()) {
    VLOG(VALIDATOR_DEBUG) << "already moved";
    co_return {};
  }
  if (handle_->id().is_masterchain()) {
    auto state = td::Ref<MasterchainState>{co_await td::actor::ask(db_, &Db::get_block_state, handle_)};
    td::uint32 monitor_min_split = state->monitor_min_split_depth(basechainId);
    td::actor::send_closure(archive_, &ArchiveManager::set_current_shard_split_depth, monitor_min_split);
  }
  std::vector<FileReference> file_refs;
  if (handle_->received()) {
    file_refs.push_back(fileref::Block{handle_->id()});
  }
  if (handle_->inited_proof()) {
    file_refs.push_back(fileref::Proof{handle_->id()});
  }
  if (handle_->inited_proof_link()) {
    file_refs.push_back(fileref::ProofLink{handle_->id()});
  }
  std::vector<td::actor::StartedTask<std::pair<FileReference, td::BufferSlice>>> tasks;
  for (FileReference ref : file_refs) {
    tasks.push_back(td::actor::ask(archive_, &ArchiveManager::get_file, handle_, ref)
                        .then([ref](td::BufferSlice data) { return std::make_pair(ref, std::move(data)); })
                        .start());
  }
  VLOG(VALIDATOR_DEBUG) << "loading data";
  std::vector<std::pair<FileReference, td::BufferSlice>> files = co_await td::actor::all(std::move(tasks));
  VLOG(VALIDATOR_DEBUG) << "loaded data";
  co_await td::actor::ask(archive_, &ArchiveManager::move_block_to_archive, handle_, std::move(files));
  co_return {};
}

}  // namespace validator

}  // namespace ton
