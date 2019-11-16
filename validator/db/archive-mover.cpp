#include "archive-mover.hpp"
#include "td/actor/MultiPromise.h"
#include "validator/fabric.h"

namespace ton {

namespace validator {

void ArchiveFileMover::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_handle1, std::move(R));
  });
  td::actor::send_closure(archive_manager_, &ArchiveManager::get_handle, block_id_, std::move(P));
}

void ArchiveFileMover::got_block_handle0(td::Result<BlockHandle> R) {
  if (R.is_ok()) {
    handle_ = R.move_as_ok();
    CHECK(handle_->moved_to_archive());
    CHECK(handle_->handle_moved_to_archive());
    finish_query();
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_handle1, std::move(R));
  });
  td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read_handle, block_id_, std::move(P));
}

void ArchiveFileMover::got_block_handle1(td::Result<BlockHandle> R) {
  if (R.is_ok()) {
    handle_ = R.move_as_ok();
    got_block_handle();
    return;
  }

  if (R.error().code() != ErrorCode::notready) {
    abort_query(R.move_as_error());
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_handle1, std::move(R));
  });
  td::actor::send_closure(block_db_, &BlockDb::get_block_handle, std::move(P));
}

void ArchiveFileMover::got_block_handle2(td::Result<BlockHandle> R) {
  if (R.is_ok()) {
    handle_ = R.move_as_ok();
    got_block_handle();
    return;
  }

  if (R.error().code() != ErrorCode::notready) {
    abort_query(R.move_as_error());
    return;
  }

  finish_query();
}

void ArchiveFileMover::got_block_handle() {
  if (!handle_->is_applied()) {
    finish_query();
    return;
  }
  if (handle_->id().seqno() == 0) {
    processed_all_children();
    return;
  }

  CHECK(handle_->inited_prev());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveFileMover::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveFileMover::processed_child);
    }
  });

  td::actor::create_actor<ArchiveFileMover>("mover", handle_->one_prev(left_), mode_, block_db_, file_db_,
                                            old_archive_db_, old_archive_manager_, archive_manager_, std::move(P))
      .release();
}

void ArchiveFileMover::processed_child() {
  if (!left_ || !handle_->merge_before()) {
    processed_all_children();
    return;
  }
  left_ = false;
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveFileMover::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveFileMover::processed_child);
    }
  });

  td::actor::create_actor<ArchiveFileMover>("mover", handle_->one_prev(left_), mode_, block_db_, file_db_,
                                            old_archive_db_, old_archive_manager_, archive_manager_, std::move(P))
      .release();
}

void ArchiveFileMover::processed_all_children() {
  if (!handle_->received()) {
    got_block_data(td::BufferSlice{});
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_data, std::move(R));
    });

    if (handle_->moved_to_archive()) {
      CHECK(handle_->inited_unix_time());
      td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read, handle_->unix_time(),
                              handle_->is_key_block(), FileDb::RefId{fileref::Block{handle_->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle_->moved_to_storage() ? old_archive_db_ : file_db_, &FileDb::load_file,
                              FileDb::RefId{fileref::Block{handle_->id()}}, std::move(P));
    }
  }
}

void ArchiveFileMover::got_block_data(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    if (R.error().code() != ErrorCode::notready) {
      abort_query(R.move_as_error());
      return;
    }
  } else {
    data_ = R.move_as_ok();
  }
  if (!handle_->inited_proof()) {
    got_block_proof(td::BufferSlice{});
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_proof, std::move(R));
    });

    if (handle_->moved_to_archive()) {
      CHECK(handle_->inited_unix_time());
      td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read, handle_->unix_time(),
                              handle_->is_key_block(), FileDb::RefId{fileref::Proof{handle_->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle_->moved_to_storage() ? old_archive_db_ : file_db_, &FileDb::load_file,
                              FileDb::RefId{fileref::Proof{handle_->id()}}, std::move(P));
    }
  }
}

void ArchiveFileMover::got_block_proof(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    if (R.error().code() != ErrorCode::notready) {
      abort_query(R.move_as_error());
      return;
    }
  } else {
    proof_ = R.move_as_ok();
  }
  if (!handle_->inited_proof_link()) {
    got_block_proof_link(td::BufferSlice{});
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      td::actor::send_closure(SelfId, &ArchiveFileMover::got_block_proof_link, std::move(R));
    });

    if (handle_->moved_to_archive()) {
      CHECK(handle_->inited_unix_time());
      td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read, handle_->unix_time(),
                              handle_->is_key_block(), FileDb::RefId{fileref::ProofLink{handle_->id()}}, std::move(P));
    } else {
      td::actor::send_closure(handle_->moved_to_storage() ? old_archive_db_ : file_db_, &FileDb::load_file,
                              FileDb::RefId{fileref::ProofLink{handle_->id()}}, std::move(P));
    }
  }
}

void ArchiveFileMover::got_block_proof_link(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    if (R.error().code() != ErrorCode::notready) {
      abort_query(R.move_as_error());
      return;
    }
  } else {
    proof_link_ = R.move_as_ok();
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveFileMover::written_data);
  });

  if (data_.size() > 0) {
    td::actor::send_closure(archive_manager_, &ArchiveManager::add_file, handle_, fileref::Block{block_id_},
                            std::move(data_), ig.get_promise());
  }
  if (proof_.size() > 0) {
    td::actor::send_closure(archive_manager_, &ArchiveManager::add_file, handle_, fileref::Proof{block_id_},
                            std::move(proof_), ig.get_promise());
  }
  if (proof_link_.size() > 0) {
    td::actor::send_closure(archive_manager_, &ArchiveManager::add_file, handle_, fileref::ProofLink{block_id_},
                            std::move(proof_link_), ig.get_promise());
  }
}

void ArchiveFileMover::written_data() {
  handle_->set_moved_to_archive();
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveFileMover::written_handle);
  });
  td::actor::send_closure(archive_manager_, &ArchiveManager::add_handle, handle_, std::move(P));
}

void ArchiveFileMover::written_handle() {
  CHECK(handle_->handle_moved_to_archive());
  finish_query();
}

void ArchiveFileMover::abort_query(td::Status error) {
  if (promise_) {
    promise_.set_error(std::move(error));
  }
  stop();
}

void ArchiveFileMover::finish_query() {
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void ArchiveKeyBlockMover::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::skip_block_proof, R.move_as_ok());
    } else {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::failed_to_get_proof0);
    }
  });
  if (proof_link_) {
    td::actor::send_closure(archive_manager_, &ArchiveManager::get_file_short, fileref::ProofLink{block_id_},
                            std::move(P));
  } else {
    td::actor::send_closure(archive_manager_, &ArchiveManager::get_file_short, fileref::Proof{block_id_}, std::move(P));
  }
}

void ArchiveKeyBlockMover::failed_to_get_proof0() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::got_block_proof, R.move_as_ok());
    } else {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::failed_to_get_proof1);
    }
  });
  if (proof_link_) {
    td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read, fileref::ProofLink{block_id_},
                            std::move(P));
  } else {
    td::actor::send_closure(old_archive_manager_, &OldArchiveManager::read, fileref::Proof{block_id_}, std::move(P));
  }
}

void ArchiveKeyBlockMover::failed_to_get_proof1() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::got_block_proof, R.move_as_ok());
    } else {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::failed_to_get_proof2);
    }
  });
  if (proof_link_) {
    td::actor::send_closure(old_archive_db_, &FileDb::load_file, fileref::ProofLink{block_id_}, std::move(P));
  } else {
    td::actor::send_closure(old_archive_db_, &FileDb::load_file, fileref::Proof{block_id_}, std::move(P));
  }
}

void ArchiveKeyBlockMover::failed_to_get_proof2() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::got_block_proof, R.move_as_ok());
    } else {
      td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::failed_to_get_proof3);
    }
  });
  if (proof_link_) {
    td::actor::send_closure(file_db_, &FileDb::load_file, fileref::ProofLink{block_id_}, std::move(P));
  } else {
    td::actor::send_closure(file_db_, &FileDb::load_file, fileref::Proof{block_id_}, std::move(P));
  }
}

void ArchiveKeyBlockMover::failed_to_get_proof3() {
  if (proof_link_) {
    written_data();
  } else {
    proof_link_ = true;
    start_up();
  }
}

void ArchiveKeyBlockMover::got_block_proof(td::BufferSlice data) {
  data_ = std::move(data);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveKeyBlockMover::written_data);
  });

  if (proof_link_) {
    auto p = create_proof_link(block_id_, data_.clone()).move_as_ok();
    auto h = p->get_basic_header_info().move_as_ok();
    td::actor::send_closure(archive_manager_, &ArchiveManager::add_key_block_proof, h.utime,
                            fileref::ProofLink{block_id_}, std::move(data_), std::move(P));
  } else {
    auto p = create_proof(block_id_, data_.clone()).move_as_ok();
    auto h = p->get_basic_header_info().move_as_ok();
    td::actor::send_closure(archive_manager_, &ArchiveManager::add_key_block_proof, h.utime, fileref::Proof{block_id_},
                            std::move(data_), std::move(P));
  }
}

void ArchiveKeyBlockMover::skip_block_proof(td::BufferSlice data) {
  data_ = std::move(data);
  written_data();
}

void ArchiveKeyBlockMover::written_data() {
  td::Ref<ProofLink> proof_link;
  if (proof_link_) {
    auto p = create_proof_link(block_id_, data_.clone()).move_as_ok();
    proof_link = std::move(p);
  } else {
    auto p = create_proof(block_id_, data_.clone()).move_as_ok();
    proof_link = std::move(p);
  }
  auto ts = proof_link->get_basic_header_info().move_as_ok().utime;
  auto te = ValidatorManager::persistent_state_ttl(ts);
  if (te < td::Clocks::system()) {
    finish_query();
    return;
  }
}

void ArchiveKeyBlockMover::abort_query(td::Status error) {
  if (promise_) {
    promise_.set_error(std::move(error));
  }
  stop();
}

void ArchiveKeyBlockMover::finish_query() {
  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void ArchiveMover::start_up() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveMover::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveMover::moved_blocks);
    }
  });
  td::actor::create_actor<ArchiveFileMover>("fmover", masterchain_block_id_, block_db_.get(), file_db_.get(),
                                            old_archive_db_.get(), old_archive_manager_.get(), archive_manager_.get(),
                                            std::move(P))
      .release();
}

void ArchiveMover::moved_blocks() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ArchiveMover::got_handle, R.move_as_ok());
  });
  td::actor::send_closure(archive_manager_, &ArchiveManager::get_handle, masterchain_block_id_, std::move(P));
}

void ArchiveMover::got_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  CHECK(handle_->is_applied());
  CHECK(handle_->inited_state_boc());
  CHECK(!handle_->deleted_state_boc());
  auto P = td::PromiseCreator::lambda(
      [handle = handle_, SelfId = actor_id(this)](td::Result<td::Ref<vm::DataCell>> R) mutable {
        R.ensure();
        auto S = create_shard_state(handle->id(), R.move_as_ok());
        S.ensure();
        td::actor::send_closure(SelfId, &ArchiveMover::got_state, td::Ref<MasterchainState>{S.move_as_ok()});
      });
  td::actor::send_closure(cell_db_, &CellDb::load_cell, handle_->state(), std::move(P));
}

void ArchiveMover::got_state(td::Ref<MasterchainState> state) {
  state_ = std::move(state);

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveMover::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveMover::moved_key_blocks);
    }
  });

  auto k = state_->prev_key_block_id(std::numeric_limits<BlockSeqno>::max());
  while (k.is_valid() && k.seqno() > 0) {
    td::actor::create_actor<ArchiveKeyBlockMover>("keymover", k, block_db_.get(), file_db_.get(), old_archive_db_.get(),
                                                  old_archive_manager_.get(), archive_manager_.get(), ig.get_promise())
        .release();
    k = state_->prev_key_block_id(k.seqno());
  }
}

void ArchiveMover::moved_key_blocks() {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveMover::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ArchiveMover::moved_key_blocks);
    }
  });

  auto k = state_->prev_key_block_id(std::numeric_limits<BlockSeqno>::max());
  while (k.is_valid() && k.seqno() > 0) {
    td::actor::create_actor<ArchiveKeyBlockMover>("keymover", k, block_db_.get(), file_db_.get(), old_archive_db_.get(),
                                                  old_archive_manager_.get(), archive_manager_.get(), ig.get_promise())
        .release();
    k = state_->prev_key_block_id(k.seqno());
  }
}

void ArchiveMover::run() {
  if (to_move_.empty() && to_check_.empty()) {
    completed();
    return;
  }
  if (!to_check_.empty()) {
    auto B = to_check_.back();
    CHECK(to_check_set_.count(B) == 1);

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      td::actor::send_closure(SelfId, &ArchiveMover::got_to_check_handle, std::move(R));
    });
    td::actor::send_closure(block_db_, &BlockDb::get_block_handle, B, std::move(P));
    return;
  }
  CHECK(!to_move_.empty());
}

void ArchiveMover::got_to_check_handle(td::Result<BlockHandle> R) {
  if (R.is_error()) {
    CHECK(R.error().code() == ErrorCode::notready);
    run();
    return;
  }
  auto handle = R.move_as_ok();
}

}  // namespace validator

}  // namespace ton
