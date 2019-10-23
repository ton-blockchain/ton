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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "archiver.hpp"
#include "rootdb.hpp"
#include "ton/ton-tl.hpp"

namespace ton {

namespace validator {

BlockArchiver::BlockArchiver(BlockIdExt block_id, td::actor::ActorId<RootDb> root_db,
                             td::actor::ActorId<FileDb> file_db, td::actor::ActorId<FileDb> archive_db,
                             td::actor::ActorId<ArchiveManager> archive, td::Promise<td::Unit> promise)
    : block_id_(block_id)
    , root_db_(root_db)
    , file_db_(file_db)
    , archive_db_(archive_db)
    , archive_(archive)
    , promise_(std::move(promise)) {
}

void BlockArchiver::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::got_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(root_db_, &RootDb::get_block_handle_external, block_id_, false, std::move(P));
}

void BlockArchiver::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  if (handle_->moved_to_archive()) {
    finish_query();
    return;
  }

  if (!handle_->is_applied() && !handle_->is_archived() &&
      (!handle_->inited_is_key_block() || !handle_->is_key_block())) {
    // no need for this block
    // probably this block not in final chain
    // this will eventually delete all associated data
    written_block_data();
    return;
  }

  if (!handle_->inited_proof()) {
    written_proof();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::got_proof, R.move_as_ok());
  });

  if (handle_->moved_to_storage()) {
    td::actor::send_closure(archive_db_, &FileDb::load_file, FileDb::RefId{fileref::Proof{block_id_}}, std::move(P));
  } else {
    td::actor::send_closure(file_db_, &FileDb::load_file, FileDb::RefId{fileref::Proof{block_id_}}, std::move(P));
  }
}

void BlockArchiver::got_proof(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_proof);
  });
  td::actor::send_closure(archive_, &ArchiveManager::write, handle_->unix_time(), handle_->is_key_block(),
                          FileDb::RefId{fileref::Proof{block_id_}}, std::move(data), std::move(P));
}

void BlockArchiver::written_proof() {
  if (!handle_->inited_proof_link()) {
    written_proof_link();
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::got_proof_link, R.move_as_ok());
  });
  if (handle_->moved_to_storage()) {
    td::actor::send_closure(archive_db_, &FileDb::load_file, FileDb::RefId{fileref::ProofLink{block_id_}},
                            std::move(P));
  } else {
    td::actor::send_closure(file_db_, &FileDb::load_file, FileDb::RefId{fileref::ProofLink{block_id_}}, std::move(P));
  }
}

void BlockArchiver::got_proof_link(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_proof_link);
  });
  td::actor::send_closure(archive_, &ArchiveManager::write, handle_->unix_time(), handle_->is_key_block(),
                          FileDb::RefId{fileref::ProofLink{block_id_}}, std::move(data), std::move(P));
}

void BlockArchiver::written_proof_link() {
  if (!handle_->received()) {
    written_block_data();
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::got_block_data, R.move_as_ok());
  });
  if (handle_->moved_to_storage()) {
    td::actor::send_closure(archive_db_, &FileDb::load_file, FileDb::RefId{fileref::Block{block_id_}}, std::move(P));
  } else {
    td::actor::send_closure(file_db_, &FileDb::load_file, FileDb::RefId{fileref::Block{block_id_}}, std::move(P));
  }
}

void BlockArchiver::got_block_data(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_block_data);
  });
  td::actor::send_closure(archive_, &ArchiveManager::write, handle_->unix_time(), handle_->is_key_block(),
                          FileDb::RefId{fileref::Block{block_id_}}, std::move(data), std::move(P));
}

void BlockArchiver::written_block_data() {
  handle_->set_moved_to_archive();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::finish_query);
  });
  td::actor::send_closure(root_db_, &RootDb::store_block_handle, handle_, std::move(P));
}

void BlockArchiver::finish_query() {
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void BlockArchiver::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "failed to archive block " << block_id_ << ": " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

}  // namespace validator

}  // namespace ton
