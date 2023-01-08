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
#include "archive-manager.hpp"
#include "td/actor/MultiPromise.h"
#include "td/utils/overloaded.h"
#include "files-async.hpp"
#include "td/db/RocksDb.h"
#include "common/delay.h"

namespace ton {

namespace validator {

std::string PackageId::path() const {
  if (temp) {
    return "/files/packages/";
  } else if (key) {
    char s[24];
    sprintf(s, "key%03d", id / 1000000);
    return PSTRING() << "/archive/packages/" << s << "/";
  } else {
    char s[20];
    sprintf(s, "arch%04d", id / 100000);
    return PSTRING() << "/archive/packages/" << s << "/";
  }
}

std::string PackageId::name() const {
  if (temp) {
    return PSTRING() << "temp.archive." << id;
  } else if (key) {
    char s[20];
    sprintf(s, "%06d", id);
    return PSTRING() << "key.archive." << s;
  } else {
    char s[10];
    sprintf(s, "%05d", id);
    return PSTRING() << "archive." << s;
  }
}

ArchiveManager::ArchiveManager(td::actor::ActorId<RootDb> root, std::string db_root) : db_root_(db_root) {
}

void ArchiveManager::add_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  if (handle->handle_moved_to_archive()) {
    update_handle(std::move(handle), std::move(promise));
    return;
  }
  auto p = handle->id().is_masterchain()
               ? get_package_id_force(handle->masterchain_ref_block(), handle->id().shard_full(), handle->id().seqno(),
                                      handle->unix_time(), handle->logical_time(),
                                      handle->inited_is_key_block() && handle->is_key_block())
               : get_package_id(handle->masterchain_ref_block());
  auto f = get_file_desc(handle->id().shard_full(), p, handle->id().seqno(), handle->unix_time(),
                         handle->logical_time(), true);
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::add_handle, std::move(handle), std::move(promise));
}

void ArchiveManager::update_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  FileDescription *f;
  if (handle->handle_moved_to_archive()) {
    CHECK(handle->inited_unix_time());
    if (!handle->need_flush()) {
      promise.set_value(td::Unit());
      return;
    }
    f = get_file_desc(handle->id().shard_full(), get_package_id(handle->masterchain_ref_block()), handle->id().seqno(),
                      handle->unix_time(), handle->logical_time(), true);
    if (!f) {
      handle->flushed_upto(handle->version());
      promise.set_value(td::Unit());
      return;
    }
  } else {
    f = get_file_desc(handle->id().shard_full(), get_temp_package_id(), 0, 0, 0, true);
    CHECK(f);
  }
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::update_handle, std::move(handle), std::move(promise));
}

void ArchiveManager::add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data,
                              td::Promise<td::Unit> promise) {
  bool copy_to_key = false;
  if (handle->inited_is_key_block() && handle->is_key_block() && handle->inited_unix_time() &&
      handle->inited_logical_time() && handle->inited_masterchain_ref_block()) {
    copy_to_key = (ref_id.ref().get_offset() == ref_id.ref().offset<fileref::Proof>()) ||
                  (ref_id.ref().get_offset() == ref_id.ref().offset<fileref::ProofLink>());
  }
  if (!handle->handle_moved_to_archive()) {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise(std::move(promise));
    auto f1 = get_file_desc(handle->id().shard_full(), get_temp_package_id(), 0, 0, 0, true);
    td::actor::send_closure(f1->file_actor_id(), &ArchiveSlice::add_file, nullptr, std::move(ref_id), data.clone(),
                            ig.get_promise());
    if (copy_to_key) {
      auto f2 = get_file_desc(handle->id().shard_full(), get_key_package_id(handle->masterchain_ref_block()),
                              handle->id().seqno(), handle->unix_time(), handle->logical_time(), true);
      td::actor::send_closure(f2->file_actor_id(), &ArchiveSlice::add_file, nullptr, ref_id, std::move(data),
                              ig.get_promise());
    }
    return;
  }

  CHECK(handle->inited_is_key_block());

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  auto f1 = get_file_desc(handle->id().shard_full(), get_package_id(handle->masterchain_ref_block()),
                          handle->id().seqno(), handle->unix_time(), handle->logical_time(), true);
  td::actor::send_closure(f1->file_actor_id(), &ArchiveSlice::add_file, handle, ref_id, data.clone(), ig.get_promise());
  if (copy_to_key) {
    auto f2 = get_file_desc(handle->id().shard_full(), get_key_package_id(handle->masterchain_ref_block()),
                            handle->id().seqno(), handle->unix_time(), handle->logical_time(), true);
    td::actor::send_closure(f2->file_actor_id(), &ArchiveSlice::add_file, handle, ref_id, std::move(data),
                            ig.get_promise());
  }
}

void ArchiveManager::add_key_block_proof(UnixTime ts, BlockSeqno seqno, LogicalTime lt, FileReference ref_id,
                                         td::BufferSlice data, td::Promise<td::Unit> promise) {
  auto f = get_file_desc(ShardIdFull{masterchainId}, get_key_package_id(seqno), seqno, ts, lt, true);
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::add_file, nullptr, std::move(ref_id), std::move(data),
                          std::move(promise));
}

void ArchiveManager::add_temp_file_short(FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise) {
  auto f = get_file_desc(ref_id.shard(), get_temp_package_id(), 0, 0, 0, true);
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::add_file, nullptr, std::move(ref_id), std::move(data),
                          std::move(promise));
}

void ArchiveManager::get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  auto f = get_file_desc_by_seqno(block_id.shard_full(), block_id.seqno(), false);
  if (f) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id, idx = get_max_temp_file_desc_idx(),
                                         promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
      if (R.is_ok()) {
        promise.set_value(R.move_as_ok());
      } else {
        td::actor::send_closure(SelfId, &ArchiveManager::get_handle_cont, block_id, idx, std::move(promise));
      }
    });
    td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_handle, block_id, std::move(P));
  } else {
    get_handle_cont(block_id, get_max_temp_file_desc_idx(), std::move(promise));
  }
}

void ArchiveManager::get_handle_cont(BlockIdExt block_id, PackageId idx, td::Promise<BlockHandle> promise) {
  if (idx.is_empty()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "block handle not in db"));
    return;
  }
  auto f = get_temp_file_desc_by_idx(idx);
  if (!f) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "block handle not in db"));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id, idx = get_prev_temp_file_desc_idx(idx),
                                       promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ArchiveManager::get_handle_finish, R.move_as_ok(), std::move(promise));
    } else {
      td::actor::send_closure(SelfId, &ArchiveManager::get_handle_cont, block_id, idx, std::move(promise));
    }
  });
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_handle, block_id, std::move(P));
}

void ArchiveManager::get_handle_finish(BlockHandle handle, td::Promise<BlockHandle> promise) {
  auto f = get_file_desc_by_seqno(handle->id().shard_full(), handle->id().seqno(), false);
  if (!f) {
    promise.set_value(std::move(handle));
    return;
  }
  auto P = td::PromiseCreator::lambda([handle, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_ok()) {
      promise.set_value(R.move_as_ok());
    } else {
      promise.set_value(std::move(handle));
    }
  });
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_handle, handle->id(), std::move(P));
}

void ArchiveManager::get_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise) {
  bool search_in_key = false;
  BlockIdExt block_id;
  ref_id.ref().visit(td::overloaded(
      [&](const fileref::Proof &p) {
        search_in_key = p.block_id.is_masterchain();
        block_id = p.block_id;
      },
      [&](const fileref::ProofLink &p) {
        search_in_key = p.block_id.is_masterchain();
        block_id = p.block_id;
      },
      [&](const auto &p) {}));
  if (search_in_key) {
    auto f = get_file_desc_by_seqno(block_id.shard_full(), block_id.seqno(), true);
    if (f) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), ref_id,
                                           promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_ok()) {
          promise.set_value(R.move_as_ok());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::get_temp_file_short, std::move(ref_id), std::move(promise));
        }
      });
      td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_file, nullptr, ref_id, std::move(P));
      return;
    }
  }
  get_temp_file_short(std::move(ref_id), std::move(promise));
}

void ArchiveManager::get_key_block_proof(FileReference ref_id, td::Promise<td::BufferSlice> promise) {
  bool search_in_key = false;
  BlockIdExt block_id;
  ref_id.ref().visit(td::overloaded(
      [&](const fileref::Proof &p) {
        search_in_key = p.block_id.is_masterchain();
        block_id = p.block_id;
      },
      [&](const fileref::ProofLink &p) {
        search_in_key = p.block_id.is_masterchain();
        block_id = p.block_id;
      },
      [&](const auto &p) {}));
  if (search_in_key) {
    auto f = get_file_desc_by_seqno(block_id.shard_full(), block_id.seqno(), true);
    if (f) {
      td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_file, nullptr, ref_id, std::move(promise));
    } else {
      promise.set_error(td::Status::Error(ErrorCode::notready, "key proof not in db"));
    }
  } else {
    promise.set_error(
        td::Status::Error(ErrorCode::protoviolation, "only proof/prooflink supported in get_key_block_proof"));
  }
}

void ArchiveManager::get_temp_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise) {
  get_file_short_cont(std::move(ref_id), get_max_temp_file_desc_idx(), std::move(promise));
}

void ArchiveManager::get_file_short_cont(FileReference ref_id, PackageId idx, td::Promise<td::BufferSlice> promise) {
  auto f = get_temp_file_desc_by_idx(idx);
  if (!f) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "file not in db"));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), ref_id, idx = get_prev_temp_file_desc_idx(idx),
                                       promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_ok()) {
      promise.set_value(R.move_as_ok());
    } else {
      td::actor::send_closure(SelfId, &ArchiveManager::get_file_short_cont, std::move(ref_id), idx, std::move(promise));
    }
  });
  td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_file, nullptr, std::move(ref_id), std::move(P));
}

void ArchiveManager::get_file(ConstBlockHandle handle, FileReference ref_id, td::Promise<td::BufferSlice> promise) {
  if (handle->moved_to_archive()) {
    auto f = get_file_desc(handle->id().shard_full(), get_package_id(handle->masterchain_ref_block()), 0, 0, 0, false);
    if (f) {
      td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_file, std::move(handle), std::move(ref_id),
                              std::move(promise));
      return;
    }
  }
  if (handle->handle_moved_to_archive()) {
    auto f = get_file_desc(handle->id().shard_full(), get_package_id(handle->masterchain_ref_block()), 0, 0, 0, false);
    if (f) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), ref_id, idx = get_max_temp_file_desc_idx(),
                                           promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_ok()) {
          promise.set_value(R.move_as_ok());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::get_file_short_cont, ref_id, idx, std::move(promise));
        }
      });
      td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_file, std::move(handle), std::move(ref_id),
                              std::move(P));
      return;
    }
  }
  get_file_short_cont(std::move(ref_id), get_max_temp_file_desc_idx(), std::move(promise));
}

void ArchiveManager::written_perm_state(FileReferenceShort id) {
  perm_states_.emplace(id.hash(), id);
}

void ArchiveManager::add_zero_state(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) != perm_states_.end()) {
    promise.set_value(td::Unit());
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id = id.shortref(), promise = std::move(promise)](td::Result<std::string> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::written_perm_state, id);
          promise.set_value(td::Unit());
        }
      });
  td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/", path, std::move(data), std::move(P))
      .release();
}

void ArchiveManager::add_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice data,
                                          td::Promise<td::Unit> promise) {
  auto create_writer = [&](std::string path, td::Promise<std::string> P) {
    td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/",
                                           std::move(path), std::move(data), std::move(P))
        .release();
  };
  add_persistent_state_impl(block_id, masterchain_block_id, std::move(promise), std::move(create_writer));
}

void ArchiveManager::add_persistent_state_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                              std::function<td::Status(td::FileFd&)> write_state,
                                              td::Promise<td::Unit> promise) {
  auto create_writer = [&](std::string path, td::Promise<std::string> P) {
    td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/",
                                           std::move(path), std::move(write_state), std::move(P))
        .release();
  };
  add_persistent_state_impl(block_id, masterchain_block_id, std::move(promise), std::move(create_writer));
}

void ArchiveManager::add_persistent_state_impl(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                               td::Promise<td::Unit> promise,
                                               std::function<void(std::string, td::Promise<std::string>)> create_writer) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) != perm_states_.end()) {
    promise.set_value(td::Unit());
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id = id.shortref(), promise = std::move(promise)](td::Result<std::string> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::written_perm_state, id);
          promise.set_value(td::Unit());
        }
      });
  create_writer(std::move(path), std::move(P));
}

void ArchiveManager::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "zerostate not in db"));
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  td::actor::create_actor<db::ReadFile>("readfile", path, 0, -1, 0, std::move(promise)).release();
}

void ArchiveManager::check_zero_state(BlockIdExt block_id, td::Promise<bool> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) == perm_states_.end()) {
    promise.set_result(false);
    return;
  }
  promise.set_result(true);
}

void ArchiveManager::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                          td::Promise<td::BufferSlice> promise) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "state file not in db"));
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  td::actor::create_actor<db::ReadFile>("readfile", path, 0, -1, 0, std::move(promise)).release();
}

void ArchiveManager::get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                                td::int64 max_size, td::Promise<td::BufferSlice> promise) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "state file not in db"));
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  td::actor::create_actor<db::ReadFile>("readfile", path, offset, max_size, 0, std::move(promise)).release();
}

void ArchiveManager::check_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                            td::Promise<bool> promise) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  if (perm_states_.find(hash) == perm_states_.end()) {
    promise.set_result(false);
    return;
  }
  promise.set_result(true);
}

void ArchiveManager::get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts,
                                            td::Promise<ConstBlockHandle> promise) {
  auto f = get_file_desc_by_unix_time(account_id, ts, false);
  if (f) {
    auto n = f;
    do {
      n = get_next_file_desc(n);
    } while (n != nullptr && !n->has_account_prefix(account_id));
    td::actor::ActorId<ArchiveSlice> aid;
    if (n) {
      aid = n->file_actor_id();
    }
    auto P = td::PromiseCreator::lambda(
        [aid, account_id, ts, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          if (R.is_ok() || R.error().code() != ErrorCode::notready || aid.empty()) {
            promise.set_result(std::move(R));
          } else {
            td::actor::send_closure(aid, &ArchiveSlice::get_block_by_unix_time, account_id, ts, std::move(promise));
          }
        });
    td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_block_by_unix_time, account_id, ts, std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "ts not in db"));
  }
}

void ArchiveManager::get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt,
                                     td::Promise<ConstBlockHandle> promise) {
  auto f = get_file_desc_by_lt(account_id, lt, false);
  if (f) {
    auto n = f;
    do {
      n = get_next_file_desc(n);
    } while (n != nullptr && !n->has_account_prefix(account_id));
    td::actor::ActorId<ArchiveSlice> aid;
    if (n) {
      aid = n->file_actor_id();
    }
    auto P = td::PromiseCreator::lambda(
        [aid, account_id, lt, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          if (R.is_ok() || R.error().code() != ErrorCode::notready || aid.empty()) {
            promise.set_result(std::move(R));
          } else {
            td::actor::send_closure(aid, &ArchiveSlice::get_block_by_lt, account_id, lt, std::move(promise));
          }
        });
    td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_block_by_lt, account_id, lt, std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "lt not in db"));
  }
}

void ArchiveManager::get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno,
                                        td::Promise<ConstBlockHandle> promise) {
  auto f = get_file_desc_by_seqno(account_id, seqno, false);
  if (f) {
    td::actor::send_closure(f->file_actor_id(), &ArchiveSlice::get_block_by_seqno, account_id, seqno,
                            std::move(promise));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "seqno not in db"));
  }
}

void ArchiveManager::delete_package(PackageId id, td::Promise<td::Unit> promise) {
  auto key = create_serialize_tl_object<ton_api::db_files_package_key>(id.id, id.key, id.temp);

  std::string value;
  auto v = index_->get(key.as_slice(), value);
  v.ensure();
  CHECK(v.move_as_ok() == td::KeyValue::GetStatus::Ok);

  auto R = fetch_tl_object<ton_api::db_files_package_value>(value, true);
  R.ensure();
  auto x = R.move_as_ok();

  if (x->deleted_) {
    promise.set_value(td::Unit());
    return;
  }

  auto &m = get_file_map(id);
  auto it = m.find(id);
  if (it == m.end() || it->second.deleted) {
    promise.set_value(td::Unit());
    return;
  }

  it->second.deleted = true;
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        R.ensure();
        td::actor::send_closure(SelfId, &ArchiveManager::deleted_package, id, std::move(promise));
      });
  td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::destroy, std::move(P));
}

void ArchiveManager::deleted_package(PackageId id, td::Promise<td::Unit> promise) {
  auto key = create_serialize_tl_object<ton_api::db_files_package_key>(id.id, id.key, id.temp);

  std::string value;
  auto v = index_->get(key.as_slice(), value);
  v.ensure();
  CHECK(v.move_as_ok() == td::KeyValue::GetStatus::Ok);

  auto R = fetch_tl_object<ton_api::db_files_package_value>(value, true);
  R.ensure();
  auto x = R.move_as_ok();

  if (x->deleted_) {
    promise.set_value(td::Unit());
    return;
  }
  x->deleted_ = true;
  index_->begin_transaction().ensure();
  index_->set(key, serialize_tl_object(x, true)).ensure();
  index_->commit_transaction().ensure();

  auto &m = get_file_map(id);
  auto it = m.find(id);
  CHECK(it != m.end());
  CHECK(it->second.deleted);
  it->second.clear_actor_id();
  promise.set_value(td::Unit());
}

void ArchiveManager::load_package(PackageId id) {
  auto key = create_serialize_tl_object<ton_api::db_files_package_key>(id.id, id.key, id.temp);

  std::string value;
  auto v = index_->get(key.as_slice(), value);
  v.ensure();
  CHECK(v.move_as_ok() == td::KeyValue::GetStatus::Ok);

  auto R = fetch_tl_object<ton_api::db_files_package_value>(value, true);
  R.ensure();
  auto x = R.move_as_ok();

  if (x->deleted_) {
    return;
  }

  std::string prefix = PSTRING() << db_root_ << id.path() << id.name();
  auto f = td::FileFd::open(prefix + ".pack", td::FileFd::Read);
  if (f.is_error()) {
    x->deleted_ = true;
    return;
  }

  FileDescription desc{id, false};
  if (!id.temp) {
    for (auto &e : x->firstblocks_) {
      desc.first_blocks[ShardIdFull{e->workchain_, static_cast<ShardId>(e->shard_)}] = FileDescription::Desc{
          static_cast<BlockSeqno>(e->seqno_), static_cast<UnixTime>(e->unixtime_), static_cast<LogicalTime>(e->lt_)};
    }
  }

  desc.file = td::actor::create_actor<ArchiveSlice>("slice", id.id, id.key, id.temp, false, db_root_);

  get_file_map(id).emplace(id, std::move(desc));
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno,
                                                               UnixTime ts, LogicalTime lt, bool force) {
  auto &f = get_file_map(id);
  auto it = f.find(id);
  if (it != f.end()) {
    if (it->second.deleted) {
      return nullptr;
    }
    if (force && !id.temp) {
      update_desc(it->second, shard, seqno, ts, lt);
    }
    return &it->second;
  }
  if (!force) {
    return nullptr;
  }

  return add_file_desc(shard, id, seqno, ts, lt);
}

ArchiveManager::FileDescription *ArchiveManager::add_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno,
                                                               UnixTime ts, LogicalTime lt) {
  auto &f = get_file_map(id);
  CHECK(f.count(id) == 0);

  FileDescription desc{id, false};
  td::mkdir(db_root_ + id.path()).ensure();
  std::string prefix = PSTRING() << db_root_ << id.path() << id.name();
  desc.file = td::actor::create_actor<ArchiveSlice>("slice", id.id, id.key, id.temp, false, db_root_);
  if (!id.temp) {
    update_desc(desc, shard, seqno, ts, lt);
  }

  std::vector<tl_object_ptr<ton_api::db_files_package_firstBlock>> vec;
  for (auto &e : desc.first_blocks) {
    vec.push_back(create_tl_object<ton_api::db_files_package_firstBlock>(e.first.workchain, e.first.shard,
                                                                         e.second.seqno, e.second.ts, e.second.lt));
  }

  index_->begin_transaction().ensure();
  // add package info to list of packages
  {
    std::vector<td::int32> t;
    std::vector<td::int32> tk;
    std::vector<td::int32> tt;
    for (auto &e : files_) {
      t.push_back(e.first.id);
    }
    for (auto &e : key_files_) {
      tk.push_back(e.first.id);
    }
    for (auto &e : temp_files_) {
      tt.push_back(e.first.id);
    }
    (id.temp ? tt : (id.key ? tk : t)).push_back(id.id);
    index_
        ->set(create_serialize_tl_object<ton_api::db_files_index_key>().as_slice(),
              create_serialize_tl_object<ton_api::db_files_index_value>(std::move(t), std::move(tk), std::move(tt))
                  .as_slice())
        .ensure();
  }
  // add package info key
  {
    index_
        ->set(create_serialize_tl_object<ton_api::db_files_package_key>(id.id, id.key, id.temp).as_slice(),
              create_serialize_tl_object<ton_api::db_files_package_value>(id.id, id.key, id.temp, std::move(vec), false)
                  .as_slice())
        .ensure();
  }
  index_->commit_transaction().ensure();

  return &f.emplace(id, std::move(desc)).first->second;
}

void ArchiveManager::update_desc(FileDescription &desc, ShardIdFull shard, BlockSeqno seqno, UnixTime ts,
                                 LogicalTime lt) {
  auto it = desc.first_blocks.find(shard);
  if (it != desc.first_blocks.end() && it->second.seqno <= seqno) {
    return;
  }
  desc.first_blocks[shard] = FileDescription::Desc{seqno, ts, lt};
  std::vector<tl_object_ptr<ton_api::db_files_package_firstBlock>> vec;
  for (auto &e : desc.first_blocks) {
    vec.push_back(create_tl_object<ton_api::db_files_package_firstBlock>(e.first.workchain, e.first.shard,
                                                                         e.second.seqno, e.second.ts, e.second.lt));
  }
  index_->begin_transaction().ensure();
  index_
      ->set(create_serialize_tl_object<ton_api::db_files_package_key>(desc.id.id, desc.id.key, desc.id.temp).as_slice(),
            create_serialize_tl_object<ton_api::db_files_package_value>(desc.id.id, desc.id.key, desc.id.temp,
                                                                        std::move(vec), false)
                .as_slice())
      .ensure();
  index_->commit_transaction().ensure();
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_seqno(ShardIdFull shard, BlockSeqno seqno,
                                                                        bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    auto i = it->second.first_blocks.find(shard);
    if (i != it->second.first_blocks.end() && i->second.seqno <= seqno) {
      if (it->second.deleted) {
        return nullptr;
      } else {
        return &it->second;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_unix_time(ShardIdFull shard, UnixTime ts,
                                                                            bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    auto i = it->second.first_blocks.find(shard);
    if (i != it->second.first_blocks.end() && i->second.ts <= ts) {
      if (it->second.deleted) {
        return nullptr;
      } else {
        return &it->second;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_lt(ShardIdFull shard, LogicalTime lt,
                                                                     bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    auto i = it->second.first_blocks.find(shard);
    if (i != it->second.first_blocks.end() && i->second.lt <= lt) {
      if (it->second.deleted) {
        return nullptr;
      } else {
        return &it->second;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_seqno(AccountIdPrefixFull account, BlockSeqno seqno,
                                                                        bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  if (account.is_masterchain()) {
    return get_file_desc_by_seqno(ShardIdFull{masterchainId}, seqno, key_block);
  }
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    bool found = false;
    for (int i = 0; i < 60; i++) {
      auto shard = shard_prefix(account, i);
      auto it2 = it->second.first_blocks.find(shard);
      if (it2 != it->second.first_blocks.end()) {
        if (it2->second.seqno <= seqno) {
          return &it->second;
        }
        found = true;
      } else if (found) {
        break;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_unix_time(AccountIdPrefixFull account, UnixTime ts,
                                                                            bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  if (account.is_masterchain()) {
    return get_file_desc_by_unix_time(ShardIdFull{masterchainId}, ts, key_block);
  }
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    bool found = false;
    for (int i = 0; i < 60; i++) {
      auto shard = shard_prefix(account, i);
      auto it2 = it->second.first_blocks.find(shard);
      if (it2 != it->second.first_blocks.end()) {
        if (it2->second.ts <= ts) {
          return &it->second;
        }
        found = true;
      } else if (found) {
        break;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_lt(AccountIdPrefixFull account, LogicalTime lt,
                                                                     bool key_block) {
  auto &f = get_file_map(PackageId{0, key_block, false});
  if (account.is_masterchain()) {
    return get_file_desc_by_lt(ShardIdFull{masterchainId}, lt, key_block);
  }
  for (auto it = f.rbegin(); it != f.rend(); it++) {
    bool found = false;
    for (int i = 0; i < 60; i++) {
      auto shard = shard_prefix(account, i);
      auto it2 = it->second.first_blocks.find(shard);
      if (it2 != it->second.first_blocks.end()) {
        if (it2->second.lt <= lt) {
          return &it->second;
        }
        found = true;
      } else if (found) {
        break;
      }
    }
  }
  return nullptr;
}

ArchiveManager::FileDescription *ArchiveManager::get_next_file_desc(FileDescription *f) {
  auto &m = get_file_map(f->id);
  auto it = m.find(f->id);
  CHECK(it != m.end());
  it++;
  if (it == m.end()) {
    return nullptr;
  } else {
    return &it->second;
  }
}

ArchiveManager::FileDescription *ArchiveManager::get_temp_file_desc_by_idx(PackageId idx) {
  auto it = temp_files_.find(idx);
  if (it != temp_files_.end()) {
    if (it->second.deleted) {
      return nullptr;
    } else {
      return &it->second;
    }
  } else {
    return nullptr;
  }
}

PackageId ArchiveManager::get_max_temp_file_desc_idx() {
  if (temp_files_.size() == 0) {
    return PackageId::empty(false, true);
  } else {
    return temp_files_.rbegin()->first;
  }
}

PackageId ArchiveManager::get_prev_temp_file_desc_idx(PackageId idx) {
  auto it = temp_files_.lower_bound(idx);
  if (it == temp_files_.begin()) {
    return PackageId::empty(false, true);
  }
  it--;
  return it->first;
}

void ArchiveManager::start_up() {
  td::mkdir(db_root_).ensure();
  td::mkdir(db_root_ + "/archive/").ensure();
  td::mkdir(db_root_ + "/archive/tmp/").ensure();
  td::mkdir(db_root_ + "/archive/packages/").ensure();
  td::mkdir(db_root_ + "/archive/states/").ensure();
  td::mkdir(db_root_ + "/files/").ensure();
  td::mkdir(db_root_ + "/files/packages/").ensure();
  index_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_root_ + "/files/globalindex").move_as_ok());
  std::string value;
  auto v = index_->get(create_serialize_tl_object<ton_api::db_files_index_key>().as_slice(), value);
  v.ensure();
  if (v.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto R = fetch_tl_object<ton_api::db_files_index_value>(value, true);
    R.ensure();
    auto x = R.move_as_ok();

    for (auto &d : x->packages_) {
      load_package(PackageId{static_cast<td::uint32>(d), false, false});
    }
    for (auto &d : x->key_packages_) {
      load_package(PackageId{static_cast<td::uint32>(d), true, false});
    }
    for (auto &d : x->temp_packages_) {
      load_package(PackageId{static_cast<td::uint32>(d), false, true});
    }
  }

  v = index_->get("finalizedupto", value);
  v.ensure();
  if (v.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto R = td::to_integer_safe<td::uint32>(value);
    R.ensure();
    finalized_up_to_ = R.move_as_ok();
  }

  td::WalkPath::run(db_root_ + "/archive/states/", [&](td::CSlice fname, td::WalkPath::Type t) -> void {
    if (t == td::WalkPath::Type::NotDir) {
      LOG(ERROR) << "checking file " << fname;
      auto pos = fname.rfind('/');
      if (pos != td::Slice::npos) {
        fname.remove_prefix(pos + 1);
      }
      auto R = FileReferenceShort::create(fname.str());
      if (R.is_error()) {
        auto R2 = FileReference::create(fname.str());
        if (R2.is_error()) {
          LOG(ERROR) << "deleting bad state file '" << fname << "': " << R.move_as_error() << R2.move_as_error();
          td::unlink(db_root_ + "/archive/states/" + fname.str()).ignore();
          return;
        }
        auto d = R2.move_as_ok();
        auto newfname = d.filename_short();
        td::rename(db_root_ + "/archive/states/" + fname.str(), db_root_ + "/archive/states/" + newfname).ensure();
        R = FileReferenceShort::create(newfname);
        R.ensure();
      }
      auto f = R.move_as_ok();
      auto hash = f.hash();
      perm_states_[hash] = std::move(f);
    }
  }).ensure();

  persistent_state_gc(FileHash::zero());
}

void ArchiveManager::run_gc(UnixTime ts, UnixTime archive_ttl) {
  auto p = get_temp_package_id_by_unixtime(ts);
  std::vector<PackageId> vec;
  for (auto &x : temp_files_) {
    if (x.first < p) {
      vec.push_back(x.first);
    } else {
      break;
    }
  }
  if (vec.size() > 1) {
    vec.resize(vec.size() - 1, PackageId::empty(false, true));

    for (auto &x : vec) {
      delete_package(x, [](td::Unit) {});
    }
  }
  vec.clear();

  if (archive_ttl > 0) {
    for (auto &f : files_) {
      auto &desc = f.second;
      if (desc.deleted) {
        continue;
      }
      auto it = desc.first_blocks.find(ShardIdFull{masterchainId});
      if (it == desc.first_blocks.end()) {
        continue;
      }
      if (it->second.ts < ts - archive_ttl) {
        vec.push_back(f.first);
      }
    }
    if (vec.size() > 1) {
      vec.resize(vec.size() - 1, PackageId::empty(false, true));

      for (auto &x : vec) {
        LOG(ERROR) << "WARNING: deleting package " << x.id;
        delete_package(x, [](td::Unit) {});
      }
    }
  }
}

void ArchiveManager::persistent_state_gc(FileHash last) {
  if (perm_states_.size() == 0) {
    delay_action(
        [hash = FileHash::zero(), SelfId = actor_id(this)]() {
          td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, hash);
        },
        td::Timestamp::in(1.0));
    return;
  }
  auto it = perm_states_.lower_bound(last);
  if (it != perm_states_.end() && it->first == last) {
    it++;
  }
  if (it == perm_states_.end()) {
    it = perm_states_.begin();
  }

  auto &F = it->second;
  auto hash = F.hash();

  int res = 0;
  BlockSeqno seqno = 0;
  F.ref().visit(td::overloaded([&](const fileref::ZeroStateShort &x) { res = 1; },
                               [&](const fileref::PersistentStateShort &x) {
                                 res = 0;
                                 seqno = x.masterchain_seqno;
                               },
                               [&](const auto &obj) { res = -1; }));

  if (res == -1) {
    td::unlink(db_root_ + "/archive/states/" + F.filename_short()).ignore();
    perm_states_.erase(it);
  }
  if (res != 0) {
    delay_action([hash, SelfId = actor_id(
                            this)]() { td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, hash); },
                 td::Timestamp::in(1.0));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), hash](td::Result<ConstBlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveManager::got_gc_masterchain_handle, nullptr, hash);
    } else {
      td::actor::send_closure(SelfId, &ArchiveManager::got_gc_masterchain_handle, R.move_as_ok(), hash);
    }
  });

  get_block_by_seqno(AccountIdPrefixFull{masterchainId, 0}, seqno, std::move(P));
}

void ArchiveManager::got_gc_masterchain_handle(ConstBlockHandle handle, FileHash hash) {
  bool to_del = false;
  if (!handle || !handle->inited_unix_time() || !handle->unix_time()) {
    to_del = true;
  } else {
    auto ttl = ValidatorManager::persistent_state_ttl(handle->unix_time());
    to_del = ttl < td::Clocks::system();
  }
  auto it = perm_states_.find(hash);
  CHECK(it != perm_states_.end());
  auto &F = it->second;
  if (to_del) {
    td::unlink(db_root_ + "/archive/states/" + F.filename_short()).ignore();
    perm_states_.erase(it);
  }
  delay_action([hash, SelfId = actor_id(
                          this)]() { td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, hash); },
               td::Timestamp::in(1.0));
}

PackageId ArchiveManager::get_temp_package_id() const {
  return get_temp_package_id_by_unixtime(static_cast<UnixTime>(td::Clocks::system()));
}

PackageId ArchiveManager::get_temp_package_id_by_unixtime(UnixTime ts) const {
  return PackageId{ts - (ts % 3600), false, true};
}

PackageId ArchiveManager::get_key_package_id(BlockSeqno seqno) const {
  return PackageId{seqno - seqno % key_archive_size(), true, false};
}

PackageId ArchiveManager::get_package_id(BlockSeqno seqno) const {
  auto it = files_.upper_bound(PackageId{seqno, false, false});
  CHECK(it != files_.begin());
  it--;
  return it->first;
}

PackageId ArchiveManager::get_package_id_force(BlockSeqno masterchain_seqno, ShardIdFull shard, BlockSeqno seqno,
                                               UnixTime ts, LogicalTime lt, bool is_key) {
  PackageId p = PackageId::empty(false, false);
  if (!is_key) {
    auto it = files_.upper_bound(PackageId{masterchain_seqno, false, false});
    p = PackageId{masterchain_seqno - (masterchain_seqno % archive_size()), false, false};
    if (it != files_.begin()) {
      it--;
      if (p < it->first) {
        p = it->first;
      }
    }
  } else {
    p = PackageId{masterchain_seqno, false, false};
  }
  auto it = files_.find(p);
  if (it != files_.end()) {
    return it->first;
  }
  add_file_desc(shard, p, seqno, ts, lt);
  it = files_.find(p);
  CHECK(it != files_.end());
  return it->first;
}

void ArchiveManager::get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) {
  auto F = get_file_desc_by_seqno(ShardIdFull{masterchainId}, masterchain_seqno, false);
  if (!F) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "archive not found"));
    return;
  }

  td::actor::send_closure(F->file_actor_id(), &ArchiveSlice::get_archive_id, masterchain_seqno, std::move(promise));
}

void ArchiveManager::get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                                       td::Promise<td::BufferSlice> promise) {
  auto arch = static_cast<BlockSeqno>(archive_id);
  auto F = get_file_desc(ShardIdFull{masterchainId}, PackageId{arch, false, false}, 0, 0, 0, false);
  if (!F) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "archive not found"));
    return;
  }

  td::actor::send_closure(F->file_actor_id(), &ArchiveSlice::get_slice, archive_id, offset, limit, std::move(promise));
}

void ArchiveManager::commit_transaction() {
  if (!async_mode_ || huge_transaction_size_++ >= 100) {
    index_->commit_transaction().ensure();
    if (async_mode_) {
      huge_transaction_size_ = 0;
      huge_transaction_started_ = false;
    }
  }
}

void ArchiveManager::set_async_mode(bool mode, td::Promise<td::Unit> promise) {
  async_mode_ = mode;
  if (!async_mode_ && huge_transaction_started_) {
    index_->commit_transaction().ensure();
    huge_transaction_size_ = 0;
    huge_transaction_started_ = false;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &x : key_files_) {
    if (!x.second.deleted) {
      td::actor::send_closure(x.second.file_actor_id(), &ArchiveSlice::set_async_mode, mode, ig.get_promise());
    }
  }
  for (auto &x : temp_files_) {
    if (!x.second.deleted) {
      td::actor::send_closure(x.second.file_actor_id(), &ArchiveSlice::set_async_mode, mode, ig.get_promise());
    }
  }
  for (auto &x : files_) {
    if (!x.second.deleted) {
      td::actor::send_closure(x.second.file_actor_id(), &ArchiveSlice::set_async_mode, mode, ig.get_promise());
    }
  }
}

void ArchiveManager::truncate(BlockSeqno masterchain_seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  index_->begin_transaction().ensure();
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  for (auto &x : temp_files_) {
    if (!x.second.deleted) {
      td::actor::send_closure(x.second.file_actor_id(), &ArchiveSlice::destroy, ig.get_promise());
      x.second.file.release();
    }
  }
  temp_files_.clear();

  {
    auto it = key_files_.begin();
    while (it != key_files_.end()) {
      if (it->first.id <= masterchain_seqno) {
        td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::truncate, masterchain_seqno, handle,
                                ig.get_promise());
        it++;
      } else {
        auto it2 = it;
        it++;
        td::actor::send_closure(it2->second.file_actor_id(), &ArchiveSlice::destroy, ig.get_promise());
        it2->second.file.release();
        index_
            ->erase(create_serialize_tl_object<ton_api::db_files_package_key>(it2->second.id.id, it2->second.id.key,
                                                                              it2->second.id.temp)
                        .as_slice())
            .ensure();
        key_files_.erase(it2);
      }
    }
  }
  {
    auto it = files_.begin();
    while (it != files_.end()) {
      if (it->first.id <= masterchain_seqno) {
        td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::truncate, masterchain_seqno, handle,
                                ig.get_promise());
        it++;
      } else {
        auto it2 = it;
        it++;
        td::actor::send_closure(it2->second.file_actor_id(), &ArchiveSlice::destroy, ig.get_promise());
        it2->second.file.release();
        index_
            ->erase(create_serialize_tl_object<ton_api::db_files_package_key>(it2->second.id.id, it2->second.id.key,
                                                                              it2->second.id.temp)
                        .as_slice())
            .ensure();
        files_.erase(it2);
      }
    }
  }
  {
    std::vector<td::int32> t;
    std::vector<td::int32> tk;
    std::vector<td::int32> tt;
    for (auto &e : files_) {
      t.push_back(e.first.id);
    }
    for (auto &e : key_files_) {
      tk.push_back(e.first.id);
    }
    for (auto &e : temp_files_) {
      tt.push_back(e.first.id);
    }
    index_
        ->set(create_serialize_tl_object<ton_api::db_files_index_key>().as_slice(),
              create_serialize_tl_object<ton_api::db_files_index_value>(std::move(t), std::move(tk), std::move(tt))
                  .as_slice())
        .ensure();
  }
  index_->commit_transaction().ensure();

  {
    auto it = perm_states_.begin();
    while (it != perm_states_.end()) {
      int res = 0;
      it->second.ref().visit(td::overloaded(
          [&](const fileref::ZeroStateShort &x) { res = -1; },
          [&](const fileref::PersistentStateShort &x) { res = x.masterchain_seqno <= masterchain_seqno ? -1 : 1; },
          [&](const auto &obj) { res = 1; }));
      if (res <= 0) {
        it++;
      } else {
        auto it2 = it;
        it++;
        td::unlink(db_root_ + "/archive/states/" + it2->second.filename_short()).ignore();
        perm_states_.erase(it2);
      }
    }
  }
}

bool ArchiveManager::FileDescription::has_account_prefix(AccountIdPrefixFull account_id) const {
  for (int i = 0; i < 60; i++) {
    auto shard = shard_prefix(account_id, i);
    if (first_blocks.count(shard)) {
      return true;
    }
  }
  return false;
}

}  // namespace validator

}  // namespace ton
