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
    snprintf(s, sizeof(s), "key%03d", id / 1000000);
    return PSTRING() << "/archive/packages/" << s << "/";
  } else {
    char s[20];
    snprintf(s, sizeof(s), "arch%04d", id / 100000);
    return PSTRING() << "/archive/packages/" << s << "/";
  }
}

std::string PackageId::name() const {
  if (temp) {
    return PSTRING() << "temp.archive." << id;
  } else if (key) {
    char s[20];
    snprintf(s, sizeof(s), "%06d", id);
    return PSTRING() << "key.archive." << s;
  } else {
    char s[10];
    snprintf(s, sizeof(s), "%05d", id);
    return PSTRING() << "archive." << s;
  }
}

ArchiveManager::ArchiveManager(td::actor::ActorId<RootDb> root, std::string db_root,
                               td::Ref<ValidatorManagerOptions> opts)
    : db_root_(db_root), opts_(opts) {
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
  const FileDescription *f;
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

void ArchiveManager::register_perm_state(FileReferenceShort id) {
  BlockSeqno masterchain_seqno = 0;
  id.ref().visit(td::overloaded(
      [&](const fileref::PersistentStateShort &x) { masterchain_seqno = x.masterchain_seqno; }, [&](const auto &) {}));
  td::uint64 size;
  auto r_stat = td::stat(db_root_ + "/archive/states/" + id.filename_short());
  if (r_stat.is_error()) {
    LOG(WARNING) << "Cannot stat persistent state file " << id.filename_short() << " : " << r_stat.move_as_error();
    size = 0;
  } else {
    size = r_stat.ok().size_;
  }
  perm_states_[{masterchain_seqno, id.hash()}] = {.id = id, .size = size};
}

void ArchiveManager::add_zero_state(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find({0, hash}) != perm_states_.end()) {
    promise.set_value(td::Unit());
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id = id.shortref(), promise = std::move(promise)](td::Result<std::string> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::register_perm_state, id);
          promise.set_value(td::Unit());
        }
      });
  td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/", path, std::move(data), std::move(P))
      .release();
}

void ArchiveManager::add_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice data,
                                          td::Promise<td::Unit> promise) {
  auto create_writer = [&](std::string path, td::Promise<std::string> P) {
    td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/", std::move(path), std::move(data),
                                           std::move(P))
        .release();
  };
  add_persistent_state_impl(block_id, masterchain_block_id, std::move(promise), std::move(create_writer));
}

void ArchiveManager::add_persistent_state_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                              std::function<td::Status(td::FileFd &)> write_state,
                                              td::Promise<td::Unit> promise) {
  auto create_writer = [&](std::string path, td::Promise<std::string> P) {
    td::actor::create_actor<db::WriteFile>("writefile", db_root_ + "/archive/tmp/", std::move(path),
                                           std::move(write_state), std::move(P))
        .release();
  };
  add_persistent_state_impl(block_id, masterchain_block_id, std::move(promise), std::move(create_writer));
}

void ArchiveManager::add_persistent_state_impl(
    BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise,
    std::function<void(std::string, td::Promise<std::string>)> create_writer) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  BlockSeqno masterchain_seqno = masterchain_block_id.seqno();
  auto hash = id.hash();
  if (perm_states_.find({masterchain_seqno, hash}) != perm_states_.end()) {
    promise.set_value(td::Unit());
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id = id.shortref(), promise = std::move(promise)](td::Result<std::string> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ArchiveManager::register_perm_state, id);
          promise.set_value(td::Unit());
        }
      });
  create_writer(std::move(path), std::move(P));
}

void ArchiveManager::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find({0, hash}) == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "zerostate not in db"));
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  td::actor::create_actor<db::ReadFile>("readfile", path, 0, -1, 0, std::move(promise)).release();
}

void ArchiveManager::check_zero_state(BlockIdExt block_id, td::Promise<bool> promise) {
  auto id = FileReference{fileref::ZeroState{block_id}};
  auto hash = id.hash();
  if (perm_states_.find({0, hash}) == perm_states_.end()) {
    promise.set_result(false);
    return;
  }
  promise.set_result(true);
}

void ArchiveManager::get_previous_persistent_state_files(
    BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) {
  auto it = perm_states_.lower_bound({cur_mc_seqno, FileHash::zero()});
  if (it == perm_states_.begin()) {
    promise.set_value({});
    return;
  }
  --it;
  BlockSeqno mc_seqno = it->first.first;
  std::vector<std::pair<std::string, ShardIdFull>> files;
  while (it->first.first == mc_seqno) {
    files.emplace_back(db_root_ + "/archive/states/" + it->second.id.filename_short(), it->second.id.shard());
    if (it == perm_states_.begin()) {
      break;
    }
    --it;
  }
  promise.set_value(std::move(files));
}

void ArchiveManager::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                          td::Promise<td::BufferSlice> promise) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  if (perm_states_.find({masterchain_block_id.seqno(), hash}) == perm_states_.end()) {
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
  if (perm_states_.find({masterchain_block_id.seqno(), hash}) == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "state file not in db"));
    return;
  }

  auto path = db_root_ + "/archive/states/" + id.filename_short();
  td::actor::create_actor<db::ReadFile>("readfile", path, offset, max_size, 0, std::move(promise)).release();
}

void ArchiveManager::get_persistent_state_file_size(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                    td::Promise<td::uint64> promise) {
  auto id = FileReference{fileref::PersistentState{block_id, masterchain_block_id}};
  auto hash = id.hash();
  auto it = perm_states_.find({masterchain_block_id.seqno(), hash});
  if (it == perm_states_.end()) {
    promise.set_error(td::Status::Error(ErrorCode::notready));
    return;
  }
  promise.set_result(it->second.size);
}

void ArchiveManager::get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts,
                                            td::Promise<ConstBlockHandle> promise) {
  auto f1 = get_file_desc_by_unix_time(account_id, ts, false);
  auto f2 = get_next_file_desc(f1, account_id, false);
  if (!f1) {
    std::swap(f1, f2);
  }
  if (f1) {
    td::actor::ActorId<ArchiveSlice> aid;
    if (f2) {
      aid = f2->file_actor_id();
    }
    auto P = td::PromiseCreator::lambda(
        [aid, account_id, ts, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          if (R.is_ok() || R.error().code() != ErrorCode::notready || aid.empty()) {
            promise.set_result(std::move(R));
          } else {
            td::actor::send_closure(aid, &ArchiveSlice::get_block_by_unix_time, account_id, ts, std::move(promise));
          }
        });
    td::actor::send_closure(f1->file_actor_id(), &ArchiveSlice::get_block_by_unix_time, account_id, ts, std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "ts not in db"));
  }
}

void ArchiveManager::get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt,
                                     td::Promise<ConstBlockHandle> promise) {
  auto f1 = get_file_desc_by_lt(account_id, lt, false);
  auto f2 = get_next_file_desc(f1, account_id, false);
  if (!f1) {
    std::swap(f1, f2);
  }
  if (f1) {
    td::actor::ActorId<ArchiveSlice> aid;
    if (f2) {
      aid = f2->file_actor_id();
    }
    auto P = td::PromiseCreator::lambda(
        [aid, account_id, lt, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          if (R.is_ok() || R.error().code() != ErrorCode::notready || aid.empty()) {
            promise.set_result(std::move(R));
          } else {
            td::actor::send_closure(aid, &ArchiveSlice::get_block_by_lt, account_id, lt, std::move(promise));
          }
        });
    td::actor::send_closure(f1->file_actor_id(), &ArchiveSlice::get_block_by_lt, account_id, lt, std::move(P));
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
  it->second.file.reset();
  promise.set_value(td::Unit());
}

void ArchiveManager::load_package(PackageId id) {
  auto &m = get_file_map(id);
  if (m.count(id)) {
    LOG(WARNING) << "Duplicate id " << id.name();
    return;
  }
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

  desc.file = td::actor::create_actor<ArchiveSlice>("slice", id.id, id.key, id.temp, false, 0, db_root_,
                                                    archive_lru_.get(), statistics_);

  m.emplace(id, std::move(desc));
  update_permanent_slices();
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno,
                                                                     UnixTime ts, LogicalTime lt, bool force) {
  auto &f = get_file_map(id);
  auto it = f.find(id);
  if (it != f.end()) {
    if (it->second.deleted) {
      return nullptr;
    }
    if (force && !id.temp) {
      update_desc(f, it->second, shard, seqno, ts, lt);
    }
    return &it->second;
  }
  if (!force) {
    return nullptr;
  }

  return add_file_desc(shard, id, seqno, ts, lt);
}

const ArchiveManager::FileDescription *ArchiveManager::add_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno,
                                                                     UnixTime ts, LogicalTime lt) {
  auto &f = get_file_map(id);
  CHECK(f.count(id) == 0);

  FileDescription new_desc{id, false};
  td::mkdir(db_root_ + id.path()).ensure();
  std::string prefix = PSTRING() << db_root_ << id.path() << id.name();
  new_desc.file = td::actor::create_actor<ArchiveSlice>("slice", id.id, id.key, id.temp, false,
                                                        id.key || id.temp ? 0 : cur_shard_split_depth_, db_root_,
                                                        archive_lru_.get(), statistics_);
  const FileDescription &desc = f.emplace(id, std::move(new_desc));
  if (!id.temp) {
    update_desc(f, desc, shard, seqno, ts, lt);
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
  update_permanent_slices();
  return &desc;
}

void ArchiveManager::update_desc(FileMap &f, const FileDescription &desc, ShardIdFull shard, BlockSeqno seqno,
                                 UnixTime ts, LogicalTime lt) {
  auto it = desc.first_blocks.find(shard);
  if (it != desc.first_blocks.end() && it->second.seqno <= seqno) {
    return;
  }
  f.set_shard_first_block(desc, shard, FileDescription::Desc{seqno, ts, lt});
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

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_seqno(ShardIdFull shard, BlockSeqno seqno,
                                                                              bool key_block) {
  return get_file_map(PackageId{0, key_block, false}).get_file_desc_by_seqno(shard, seqno);
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_unix_time(ShardIdFull shard, UnixTime ts,
                                                                                  bool key_block) {
  return get_file_map(PackageId{0, key_block, false}).get_file_desc_by_unix_time(shard, ts);
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_lt(ShardIdFull shard, LogicalTime lt,
                                                                           bool key_block) {
  return get_file_map(PackageId{0, key_block, false}).get_file_desc_by_lt(shard, lt);
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_seqno(AccountIdPrefixFull account,
                                                                              BlockSeqno seqno, bool key_block) {
  if (account.is_masterchain()) {
    return get_file_desc_by_seqno(ShardIdFull{masterchainId}, seqno, key_block);
  }
  auto &f = get_file_map(PackageId{0, key_block, false});
  const FileDescription *result = nullptr;
  for (int i = 0; i <= 60; i++) {
    const FileDescription *desc = f.get_file_desc_by_seqno(shard_prefix(account, i), seqno);
    if (desc && (!result || result->id < desc->id)) {
      result = desc;
    } else if (result && (!desc || desc->id < result->id)) {
      break;
    }
  }
  return result;
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_unix_time(AccountIdPrefixFull account,
                                                                                  UnixTime ts, bool key_block) {
  if (account.is_masterchain()) {
    return get_file_desc_by_unix_time(ShardIdFull{masterchainId}, ts, key_block);
  }
  auto &f = get_file_map(PackageId{0, key_block, false});
  const FileDescription *result = nullptr;
  for (int i = 0; i <= 60; i++) {
    const FileDescription *desc = f.get_file_desc_by_unix_time(shard_prefix(account, i), ts);
    if (desc && (!result || result->id < desc->id)) {
      result = desc;
    } else if (result && (!desc || desc->id < result->id)) {
      break;
    }
  }
  return result;
}

const ArchiveManager::FileDescription *ArchiveManager::get_file_desc_by_lt(AccountIdPrefixFull account, LogicalTime lt,
                                                                           bool key_block) {
  if (account.is_masterchain()) {
    return get_file_desc_by_lt(ShardIdFull{masterchainId}, lt, key_block);
  }
  auto &f = get_file_map(PackageId{0, key_block, false});
  const FileDescription *result = nullptr;
  for (int i = 0; i <= 60; i++) {
    const FileDescription *desc = f.get_file_desc_by_lt(shard_prefix(account, i), lt);
    if (desc && (!result || result->id < desc->id)) {
      result = desc;
    } else if (result && (!desc || desc->id < result->id)) {
      break;
    }
  }
  return result;
}

const ArchiveManager::FileDescription *ArchiveManager::get_next_file_desc(const FileDescription *f,
                                                                          AccountIdPrefixFull shard, bool key_block) {
  auto &m = get_file_map(PackageId{0, key_block, false});
  const FileDescription *result = nullptr;
  for (int i = 0; i <= 60; i++) {
    const FileDescription *desc = m.get_next_file_desc(shard_prefix(shard, i), f);
    if (desc && (!result || desc->id < result->id)) {
      result = desc;
    } else if (result && (!desc || result->id < desc->id)) {
      break;
    }
  }
  return result;
}

const ArchiveManager::FileDescription *ArchiveManager::get_temp_file_desc_by_idx(PackageId idx) {
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
  if (opts_->get_max_open_archive_files() > 0) {
    archive_lru_ = td::actor::create_actor<ArchiveLru>("archive_lru", opts_->get_max_open_archive_files());
  }
  if (!opts_->get_disable_rocksdb_stats()) {
    statistics_.init();
  }
  td::RocksDbOptions db_options;
  db_options.statistics = statistics_.rocksdb_statistics;
  index_ = std::make_shared<td::RocksDb>(
      td::RocksDb::open(db_root_ + "/files/globalindex", std::move(db_options)).move_as_ok());
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
      auto pos = fname.rfind(TD_DIR_SLASH);
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
      register_perm_state(R.move_as_ok());
    }
  }).ensure();

  persistent_state_gc({0, FileHash::zero()});

  double open_since = td::Clocks::system() - opts_->get_archive_preload_period();
  for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
    if (it->second.file_actor_id().empty()) {
      continue;
    }
    td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::open_files);
    bool stop = true;
    for (const auto &first_block : it->second.first_blocks) {
      if ((double)first_block.second.ts >= open_since) {
        stop = false;
        break;
      }
    }
    if (stop) {
      break;
    }
  }

  if (!opts_->get_disable_rocksdb_stats()) {
    alarm_timestamp() = td::Timestamp::in(60.0);
  }
}

void ArchiveManager::alarm() {
  alarm_timestamp() = td::Timestamp::in(60.0);
  auto stats = statistics_.to_string_and_reset();
  auto to_file_r =
      td::FileFd::open(db_root_ + "/db_stats.txt", td::FileFd::Truncate | td::FileFd::Create | td::FileFd::Write, 0644);
  if (to_file_r.is_error()) {
    LOG(ERROR) << "Failed to open db_stats.txt: " << to_file_r.move_as_error();
    return;
  }
  auto to_file = to_file_r.move_as_ok();
  auto res = to_file.write(stats);
  to_file.close();
  if (res.is_error()) {
    LOG(ERROR) << "Failed to write to db_stats.txt: " << res.move_as_error();
    return;
  }
}

void ArchiveManager::run_gc(UnixTime mc_ts, UnixTime gc_ts, double archive_ttl) {
  auto p = get_temp_package_id_by_unixtime((double)mc_ts - TEMP_PACKAGES_TTL);
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
      if ((double)it->second.ts < (double)gc_ts - archive_ttl) {
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

void ArchiveManager::persistent_state_gc(std::pair<BlockSeqno, FileHash> last) {
  if (perm_states_.empty()) {
    delay_action(
        [SelfId = actor_id(this)]() {
          td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc,
                                  std::pair<BlockSeqno, FileHash>{0, FileHash::zero()});
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

  auto key = it->first;
  auto &F = it->second;

  int res = 0;
  BlockSeqno seqno = 0;
  F.id.ref().visit(td::overloaded([&](const fileref::ZeroStateShort &) { res = 1; },
                                  [&](const fileref::PersistentStateShort &x) {
                                    res = 0;
                                    seqno = x.masterchain_seqno;
                                  },
                                  [&](const auto &obj) { res = -1; }));

  if (res == -1) {
    td::unlink(db_root_ + "/archive/states/" + F.id.filename_short()).ignore();
    perm_states_.erase(it);
  }
  if (res != 0) {
    delay_action([key, SelfId = actor_id(
                           this)]() { td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, key); },
                 td::Timestamp::in(1.0));
    return;
  }
  CHECK(seqno == key.first);

  // Do not delete the most recent fully serialized state
  bool allow_delete = false;
  auto it2 = perm_states_.lower_bound({seqno + 1, FileHash::zero()});
  if (it2 != perm_states_.end()) {
    it2 = perm_states_.lower_bound({it2->first.first + 1, FileHash::zero()});
    if (it2 != perm_states_.end()) {
      allow_delete = true;
    }
  }
  if (!allow_delete) {
    delay_action([key, SelfId = actor_id(
                           this)]() { td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, key); },
                 td::Timestamp::in(1.0));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), key](td::Result<ConstBlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ArchiveManager::got_gc_masterchain_handle, nullptr, key);
    } else {
      td::actor::send_closure(SelfId, &ArchiveManager::got_gc_masterchain_handle, R.move_as_ok(), key);
    }
  });

  get_block_by_seqno(AccountIdPrefixFull{masterchainId, 0}, seqno, std::move(P));
}

void ArchiveManager::got_gc_masterchain_handle(ConstBlockHandle handle, std::pair<BlockSeqno, FileHash> key) {
  bool to_del = false;
  if (!handle || !handle->inited_unix_time() || !handle->unix_time()) {
    to_del = true;
  } else {
    auto ttl = ValidatorManager::persistent_state_ttl(handle->unix_time());
    to_del = ttl < td::Clocks::system();
  }
  auto it = perm_states_.find(key);
  CHECK(it != perm_states_.end());
  auto &F = it->second;
  if (to_del) {
    td::unlink(db_root_ + "/archive/states/" + F.id.filename_short()).ignore();
    perm_states_.erase(it);
  }
  delay_action(
      [key, SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &ArchiveManager::persistent_state_gc, key); },
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

void ArchiveManager::get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                    td::Promise<td::uint64> promise) {
  auto F = get_file_desc_by_seqno(ShardIdFull{masterchainId}, masterchain_seqno, false);
  if (!F) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "archive not found"));
    return;
  }

  td::actor::send_closure(F->file_actor_id(), &ArchiveSlice::get_archive_id, masterchain_seqno, shard_prefix,
                          std::move(promise));
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

void ArchiveManager::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  std::vector<std::pair<std::string, std::string>> stats;
  {
    std::map<BlockSeqno, td::uint64> states;
    for (auto &[key, file] : perm_states_) {
      BlockSeqno seqno = key.first;
      states[seqno] += file.size;
    }
    td::StringBuilder sb;
    for (auto &[seqno, size] : states) {
      sb << seqno << ":" << td::format::as_size(size) << " ";
    }
    if (!sb.as_cslice().empty()) {
      stats.emplace_back("persistent_states", sb.as_cslice().str());
    }
  }
  promise.set_value(std::move(stats));
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
        if (!it->second.deleted) {
          td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::truncate, masterchain_seqno, handle,
                                  ig.get_promise());
        }
        it++;
      } else {
        auto it2 = it;
        it++;
        if (!it2->second.deleted) {
          td::actor::send_closure(it2->second.file_actor_id(), &ArchiveSlice::destroy, ig.get_promise());
        }
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
        if (!it->second.deleted) {
          td::actor::send_closure(it->second.file_actor_id(), &ArchiveSlice::truncate, masterchain_seqno, handle,
                                  ig.get_promise());
        }
        it++;
      } else {
        auto it2 = it;
        it++;
        if (!it2->second.deleted) {
          td::actor::send_closure(it2->second.file_actor_id(), &ArchiveSlice::destroy, ig.get_promise());
        }
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
      it->second.id.ref().visit(td::overloaded(
          [&](const fileref::ZeroStateShort &x) { res = -1; },
          [&](const fileref::PersistentStateShort &x) { res = x.masterchain_seqno <= masterchain_seqno ? -1 : 1; },
          [&](const auto &obj) { res = 1; }));
      if (res <= 0) {
        it++;
      } else {
        auto it2 = it;
        it++;
        td::unlink(db_root_ + "/archive/states/" + it2->second.id.filename_short()).ignore();
        perm_states_.erase(it2);
      }
    }
  }
  update_permanent_slices();
}

void ArchiveManager::FileMap::shard_index_add(const FileDescription &desc) {
  for (const auto &p : desc.first_blocks) {
    ShardIndex &s = shards_[p.first];
    s.seqno_index_[p.second.seqno] = &desc;
    s.lt_index_[p.second.lt] = &desc;
    s.unix_time_index_[p.second.ts] = &desc;
    s.packages_index_[desc.id] = &desc;
  }
}

void ArchiveManager::FileMap::shard_index_del(const FileDescription &desc) {
  for (const auto &p : desc.first_blocks) {
    ShardIndex &s = shards_[p.first];
    s.seqno_index_.erase(p.second.seqno);
    s.lt_index_.erase(p.second.lt);
    s.unix_time_index_.erase(p.second.ts);
    s.packages_index_.erase(desc.id);
  }
}

void ArchiveManager::FileMap::set_shard_first_block(const FileDescription &desc, ShardIdFull shard,
                                                    FileDescription::Desc v) {
  ShardIndex &s = shards_[shard];
  auto &d = const_cast<FileDescription &>(desc);
  auto it = d.first_blocks.find(shard);
  if (it != d.first_blocks.end()) {
    s.seqno_index_.erase(it->second.seqno);
    s.lt_index_.erase(it->second.lt);
    s.unix_time_index_.erase(it->second.ts);
  }
  d.first_blocks[shard] = v;
  s.seqno_index_[v.seqno] = &d;
  s.lt_index_[v.lt] = &d;
  s.unix_time_index_[v.ts] = &d;
  s.packages_index_[d.id] = &d;
}

const ArchiveManager::FileDescription *ArchiveManager::FileMap::get_file_desc_by_seqno(ShardIdFull shard,
                                                                                       BlockSeqno seqno) const {
  auto it = shards_.find(shard);
  if (it == shards_.end()) {
    return nullptr;
  }
  const auto &map = it->second.seqno_index_;
  auto it2 = map.upper_bound(seqno);
  if (it2 == map.begin()) {
    return nullptr;
  }
  --it2;
  return it2->second->deleted ? nullptr : it2->second;
}

const ArchiveManager::FileDescription *ArchiveManager::FileMap::get_file_desc_by_lt(ShardIdFull shard,
                                                                                    LogicalTime lt) const {
  auto it = shards_.find(shard);
  if (it == shards_.end()) {
    return nullptr;
  }
  const auto &map = it->second.lt_index_;
  auto it2 = map.upper_bound(lt);
  if (it2 == map.begin()) {
    return nullptr;
  }
  --it2;
  return it2->second->deleted ? nullptr : it2->second;
}

const ArchiveManager::FileDescription *ArchiveManager::FileMap::get_file_desc_by_unix_time(ShardIdFull shard,
                                                                                           UnixTime ts) const {
  auto it = shards_.find(shard);
  if (it == shards_.end()) {
    return nullptr;
  }
  const auto &map = it->second.unix_time_index_;
  auto it2 = map.upper_bound(ts);
  if (it2 == map.begin()) {
    return nullptr;
  }
  --it2;
  return it2->second->deleted ? nullptr : it2->second;
}

const ArchiveManager::FileDescription *ArchiveManager::FileMap::get_next_file_desc(ShardIdFull shard,
                                                                                   const FileDescription *desc) const {
  auto it = shards_.find(shard);
  if (it == shards_.end()) {
    return nullptr;
  }
  const auto &map = it->second.packages_index_;
  auto it2 = desc ? map.upper_bound(desc->id) : map.begin();
  if (it2 == map.end()) {
    return nullptr;
  }
  return it2->second->deleted ? nullptr : it2->second;
}

void ArchiveManager::update_permanent_slices() {
  if (archive_lru_.empty()) {
    return;
  }
  std::vector<PackageId> ids;
  if (!files_.empty()) {
    ids.push_back(files_.rbegin()->first);
  }
  if (!key_files_.empty()) {
    ids.push_back(key_files_.rbegin()->first);
  }
  if (!temp_files_.empty()) {
    ids.push_back(temp_files_.rbegin()->first);
  }
  td::actor::send_closure(archive_lru_, &ArchiveLru::set_permanent_slices, std::move(ids));
}

}  // namespace validator

}  // namespace ton
