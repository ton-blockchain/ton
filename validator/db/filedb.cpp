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
#include "filedb.hpp"
#include "rootdb.hpp"
#include "td/utils/port/path.h"
#include "files-async.hpp"
#include "adnl/utils.hpp"
#include "tl-utils/tl-utils.hpp"
#include "td/utils/overloaded.h"

#include "td/db/RocksDb.h"

namespace ton {

namespace validator {

std::string FileDb::get_file_name(const RefId& ref_id, bool create_dirs) {
  auto ref_id_hash = get_ref_id_hash(ref_id);

  auto s = ref_id_hash.to_hex();
  std::string path = root_path_ + "/files/";
  for (td::uint32 i = 0; i < depth_; i++) {
    path = path + s[2 * i] + s[2 * i + 1] + "/";
    if (create_dirs) {
      td::mkdir(path).ensure();
    }
  }
  return path + s;
}

void FileDb::store_file(RefId ref_id, td::BufferSlice data, td::Promise<FileHash> promise) {
  auto ref_id_hash = get_ref_id_hash(ref_id);
  auto R = get_block(ref_id_hash);
  if (R.is_ok()) {
    auto val = R.move_as_ok();
    promise.set_result(val.file_hash);
    return;
  }

  auto file_hash = sha256_bits256(data.as_slice());
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), file_hash, ref_id = std::move(ref_id),
                                       promise = std::move(promise)](td::Result<std::string> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &FileDb::store_file_continue, std::move(ref_id), file_hash, R.move_as_ok(),
                              std::move(promise));
    }
  });
  td::actor::create_actor<db::WriteFile>("writefile", root_path_ + "/tmp/", "", std::move(data), std::move(P))
      .release();
}

void FileDb::store_file_continue(RefId ref_id, FileHash file_hash, std::string res_path,
                                 td::Promise<FileHash> promise) {
  auto ref_id_hash = get_ref_id_hash(ref_id);
  auto R = get_block(ref_id_hash);
  if (R.is_ok()) {
    td::unlink(res_path).ignore();
    auto val = R.move_as_ok();
    promise.set_result(val.file_hash);
    return;
  }

  auto path = get_file_name(ref_id, true);
  td::rename(res_path, path).ensure();

  auto empty = get_empty_ref_id_hash();
  auto ER = get_block(empty);
  ER.ensure();
  auto E = ER.move_as_ok();

  auto PR = get_block(E.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  CHECK(P.next == empty);

  DbEntry D;
  D.key = std::move(ref_id);
  D.prev = E.prev;
  D.next = empty;
  D.file_hash = file_hash;

  E.prev = ref_id_hash;
  P.next = ref_id_hash;

  if (P.is_empty()) {
    E.next = ref_id_hash;
    P.prev = ref_id_hash;
  }

  kv_->begin_transaction().ensure();
  set_block(empty, std::move(E));
  set_block(D.prev, std::move(P));
  set_block(ref_id_hash, std::move(D));
  kv_->commit_transaction().ensure();

  promise.set_value(std::move(file_hash));
}

void FileDb::load_file(RefId ref_id, td::Promise<td::BufferSlice> promise) {
  auto ref_id_hash = get_ref_id_hash(ref_id);
  auto R = get_block(ref_id_hash);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  auto v = R.move_as_ok();

  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), file_hash = v.file_hash](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto data = R.move_as_ok();
          if (file_hash != sha256_bits256(data.as_slice())) {
            promise.set_error(td::Status::Error(ErrorCode::protoviolation, PSTRING() << "db error: bad file hash"));
          } else {
            promise.set_value(std::move(data));
          }
        }
      });

  td::actor::create_actor<db::ReadFile>("readfile", get_file_name(ref_id, false), 0, -1, 0, std::move(P)).release();
}

void FileDb::load_file_slice(RefId ref_id, td::int64 offset, td::int64 max_size, td::Promise<td::BufferSlice> promise) {
  auto ref_id_hash = get_ref_id_hash(ref_id);
  auto R = get_block(ref_id_hash);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  auto v = R.move_as_ok();

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  td::actor::create_actor<db::ReadFile>("readfile", get_file_name(ref_id, false), offset, max_size, 0, std::move(P))
      .release();
}

void FileDb::check_file(RefId ref_id, td::Promise<bool> promise) {
  auto ref_id_hash = get_ref_id_hash(ref_id);
  auto R = get_block(ref_id_hash);
  if (R.is_error()) {
    promise.set_result(false);
    return;
  }
  promise.set_result(true);
}

td::Slice FileDb::get_key(const RefIdHash& ref) {
  return ref.as_slice();
}

td::Result<FileDb::DbEntry> FileDb::get_block(const RefIdHash& ref) {
  std::string value;
  auto R = kv_->get(get_key(ref), value);
  R.ensure();
  if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error(ErrorCode::notready, "not in db");
  }
  auto val = fetch_tl_object<ton_api::db_filedb_value>(td::BufferSlice{value}, true);
  val.ensure();
  return DbEntry{val.move_as_ok()};
}

void FileDb::set_block(const RefIdHash& ref, DbEntry entry) {
  DCHECK(ref == get_ref_id_hash(entry.key));
  kv_->set(get_key(ref), entry.release()).ensure();
}

FileDb::FileDb(td::actor::ActorId<RootDb> root_db, std::string root_path, td::uint32 depth, bool is_archive)
    : root_db_(root_db), root_path_(root_path), depth_(depth), is_archive_(is_archive) {
}

void FileDb::start_up() {
  td::mkdir(root_path_).ensure();
  db_path_ = root_path_ + "/db/";

  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_path_).move_as_ok());
  td::mkdir(root_path_ + "/files/").ensure();
  td::mkdir(root_path_ + "/tmp/").ensure();

  last_gc_ = get_empty_ref_id_hash();
  alarm_timestamp() = td::Timestamp::in(0.01);

  auto R = get_block(last_gc_);
  if (R.is_error()) {
    DbEntry e{get_empty_ref_id(), last_gc_, last_gc_, FileHash::zero()};
    kv_->begin_transaction().ensure();
    set_block(last_gc_, std::move(e));
    kv_->commit_transaction().ensure();
  }
}

void FileDb::alarm() {
  auto R = get_block(last_gc_);
  R.ensure();
  auto N = R.move_as_ok();
  if (N.is_empty()) {
    last_gc_ = N.next;
    alarm_timestamp() = td::Timestamp::in(0.01);
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<bool> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &FileDb::skip_gc);
    } else {
      auto value = R.move_as_ok();
      if (!value) {
        td::actor::send_closure(SelfId, &FileDb::skip_gc);
      } else {
        td::actor::send_closure(SelfId, &FileDb::gc);
      }
    }
  });
  td::actor::send_closure(root_db_, &RootDb::allow_gc, std::move(N.key), is_archive_, std::move(P));
}

void FileDb::gc() {
  auto FR = get_block(last_gc_);
  FR.ensure();
  auto F = FR.move_as_ok();

  auto PR = get_block(F.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  auto NR = get_block(F.next);
  NR.ensure();
  auto N = NR.move_as_ok();

  P.next = F.next;
  N.prev = F.prev;
  if (P.is_empty() && N.is_empty()) {
    P.prev = P.next;
    N.next = N.prev;
  }

  auto name = get_file_name(F.key, false);
  auto S = td::unlink(name);
  if (S.is_error()) {
    LOG(WARNING) << "failed to delete " << name;
  }

  kv_->begin_transaction().ensure();
  kv_->erase(last_gc_.as_slice()).ensure();
  set_block(F.prev, std::move(P));
  set_block(F.next, std::move(N));
  kv_->commit_transaction().ensure();
  alarm_timestamp() = td::Timestamp::now();

  DCHECK(get_block(last_gc_).is_error());
  last_gc_ = F.next;
}

void FileDb::skip_gc() {
  auto FR = get_block(last_gc_);
  FR.ensure();
  auto F = FR.move_as_ok();
  last_gc_ = F.next;
  alarm_timestamp() = td::Timestamp::in(0.01);
}

td::BufferSlice FileDb::DbEntry::release() {
  return create_serialize_tl_object<ton_api::db_filedb_value>(get_ref_id_tl(key), prev, next, file_hash);
}

bool FileDb::DbEntry::is_empty() const {
  return (key.get_offset() == key.offset<fileref::Empty>());
}

FileDb::RefIdHash FileDb::get_ref_id_hash(const RefId& ref) {
  FileHash x;
  ref.visit([&](const auto& obj) { x = obj.hash(); });
  return x;
}

tl_object_ptr<ton_api::db_filedb_Key> FileDb::get_ref_id_tl(const RefId& ref) {
  tl_object_ptr<ton_api::db_filedb_Key> x;
  ref.visit([&](const auto& obj) { x = obj.tl(); });
  return x;
}

FileDb::RefId FileDb::get_ref_from_tl(const ton_api::db_filedb_Key& from) {
  RefId ref_id{fileref::Empty{}};
  ton_api::downcast_call(
      const_cast<ton_api::db_filedb_Key&>(from),
      td::overloaded(
          [&](const ton_api::db_filedb_key_empty& key) { ref_id = fileref::Empty{}; },
          [&](const ton_api::db_filedb_key_blockFile& key) { ref_id = fileref::Block{create_block_id(key.block_id_)}; },
          [&](const ton_api::db_filedb_key_zeroStateFile& key) {
            ref_id = fileref::ZeroState{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_persistentStateFile& key) {
            ref_id =
                fileref::PersistentState{create_block_id(key.block_id_), create_block_id(key.masterchain_block_id_)};
          },
          [&](const ton_api::db_filedb_key_proof& key) { ref_id = fileref::Proof{create_block_id(key.block_id_)}; },
          [&](const ton_api::db_filedb_key_proofLink& key) {
            ref_id = fileref::ProofLink{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_signatures& key) {
            ref_id = fileref::Signatures{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_candidate& key) {
            ref_id = fileref::Candidate{PublicKey{key.id_->source_}, create_block_id(key.id_->id_),
                                        key.id_->collated_data_file_hash_};
          },
          [&](const ton_api::db_filedb_key_blockInfo& key) {
            ref_id = fileref::BlockInfo{create_block_id(key.block_id_)};
          }));
  return ref_id;
}

FileDb::RefId FileDb::get_empty_ref_id() {
  return fileref::Empty();
}

FileDb::RefIdHash FileDb::get_empty_ref_id_hash() {
  if (empty_.is_zero()) {
    empty_ = get_ref_id_hash(get_empty_ref_id());
  }
  return empty_;
}

FileDb::DbEntry::DbEntry(tl_object_ptr<ton_api::db_filedb_value> entry)
    : key(FileDb::get_ref_from_tl(*entry->key_.get()))
    , prev(entry->prev_)
    , next(entry->next_)
    , file_hash(entry->file_hash_) {
}

void FileDb::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  std::vector<std::pair<std::string, std::string>> rocksdb_stats;
  auto stats = kv_->stats();
  if (stats.size() == 0) {
    promise.set_value(std::move(rocksdb_stats));
    return;
  }
  size_t pos = 0;
  while (pos < stats.size()) {
    while (pos < stats.size() &&
           (stats[pos] == ' ' || stats[pos] == '\n' || stats[pos] == '\r' || stats[pos] == '\t')) {
      pos++;
    }
    auto p = pos;
    if (pos == stats.size()) {
      break;
    }
    while (stats[pos] != '\n' && stats[pos] != '\r' && stats[pos] != ' ' && stats[pos] != '\t' && pos < stats.size()) {
      pos++;
    }
    auto name = stats.substr(p, pos - p);
    if (stats[pos] == '\n' || pos == stats.size()) {
      rocksdb_stats.emplace_back(name, "");
      continue;
    }
    while (pos < stats.size() &&
           (stats[pos] == ' ' || stats[pos] == '\n' || stats[pos] == '\r' || stats[pos] == '\t')) {
      pos++;
    }
    p = pos;
    while (stats[pos] != '\n' && stats[pos] != '\r' && pos < stats.size()) {
      pos++;
    }
    auto value = stats.substr(p, pos - p);
    rocksdb_stats.emplace_back(name, value);
  }
  promise.set_value(std::move(rocksdb_stats));
}

}  // namespace validator

}  // namespace ton
