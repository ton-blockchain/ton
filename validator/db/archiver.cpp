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
#include "archiver.hpp"
#include "rootdb.hpp"
#include "ton/ton-tl.hpp"

namespace ton {

namespace validator {

BlockArchiver::BlockArchiver(BlockHandle handle, td::actor::ActorId<ArchiveManager> archive_db,
                             td::Promise<td::Unit> promise)
    : handle_(std::move(handle)), archive_(archive_db), promise_(std::move(promise)) {
}

void BlockArchiver::start_up() {
  if (handle_->handle_moved_to_archive()) {
    moved_handle();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &BlockArchiver::moved_handle);
    });
    td::actor::send_closure(archive_, &ArchiveManager::add_handle, handle_, std::move(P));
  }
}

void BlockArchiver::moved_handle() {
  CHECK(handle_->handle_moved_to_archive());
  if (handle_->moved_to_archive()) {
    finish_query();
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

  td::actor::send_closure(archive_, &ArchiveManager::get_file, handle_, fileref::Proof{handle_->id()}, std::move(P));
}

void BlockArchiver::got_proof(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_proof);
  });
  td::actor::send_closure(archive_, &ArchiveManager::add_file, handle_, fileref::Proof{handle_->id()}, std::move(data),
                          std::move(P));
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

  td::actor::send_closure(archive_, &ArchiveManager::get_file, handle_, fileref::ProofLink{handle_->id()},
                          std::move(P));
}

void BlockArchiver::got_proof_link(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_proof_link);
  });
  td::actor::send_closure(archive_, &ArchiveManager::add_file, handle_, fileref::ProofLink{handle_->id()},
                          std::move(data), std::move(P));
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

  td::actor::send_closure(archive_, &ArchiveManager::get_file, handle_, fileref::Block{handle_->id()}, std::move(P));
}

void BlockArchiver::got_block_data(td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::written_block_data);
  });
  td::actor::send_closure(archive_, &ArchiveManager::add_file, handle_, fileref::Block{handle_->id()}, std::move(data),
                          std::move(P));
}

void BlockArchiver::written_block_data() {
  handle_->set_moved_to_archive();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &BlockArchiver::finish_query);
  });
  td::actor::send_closure(archive_, &ArchiveManager::update_handle, handle_, std::move(P));
}

void BlockArchiver::finish_query() {
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void BlockArchiver::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "failed to archive block " << handle_->id() << ": " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

}  // namespace validator

}  // namespace ton
