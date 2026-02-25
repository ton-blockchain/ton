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

#include "td/utils/port/path.h"

#include "fabric.h"
#include "temp-archive.hpp"

namespace ton::validator {

static td::BufferSlice db_key_block_handle(const BlockIdExt& block_id) {
  return create_serialize_tl_object<ton_api::db_temp_key_blockHandle>(create_tl_block_id(block_id));
}

static td::BufferSlice db_key_file(const FileReference& file_ref) {
  return create_serialize_tl_object<ton_api::db_temp_key_file>(file_ref.hash());
}

static td::BufferSlice db_key_gc_list(UnixTime ts, td::Slice key) {
  td::BufferSlice buf{8 + key.size()};
  td::MutableSlice s = buf.as_slice();
  s.copy_from(td::Slice{reinterpret_cast<const char*>(&ton_api::db_temp_key_gcList::ID), 4});
  s.remove_prefix(4);
  td::bitstring::bits_store_long(td::BitPtr{(unsigned char*)s.data()}, ts, 32);  // big endian
  s.remove_prefix(4);
  s.copy_from(key);
  return buf;
}

void TempArchive::start_up() {
  td::mkpath(db_path_ + "/").ensure();
  td::RocksDbOptions db_options;
  db_options.statistics = db_statistics_.rocksdb_statistics;
  auto kv = std::make_unique<td::RocksDb>(td::RocksDb::open(db_path_, std::move(db_options)).ensure().move_as_ok());
  db_snapshot_ = kv->snapshot();
  db_writer_ = td::actor::create_actor<DbWriter>("temp-archive.writer", actor_id(this), std::move(kv));
}

td::Result<td::BufferSlice> TempArchive::get_file(FileReference file_ref) {
  auto value = db_get(db_key_file(file_ref));
  if (!value) {
    return td::Status::Error(notready, PSTRING() << "file " << file_ref.filename() << " not in temp archive");
  }
  return std::move(value.value());
}

td::Result<BlockHandle> TempArchive::get_handle(BlockIdExt block_id) {
  auto value = db_get(db_key_block_handle(block_id));
  if (!value) {
    return td::Status::Error(notready, "handle not in temp archive");
  }
  return create_block_handle(std::move(value.value())).ensure().move_as_ok();
}

td::actor::Task<> TempArchive::add_file(FileReference file_ref, td::BufferSlice data, bool sync) {
  auto key = db_key_file(file_ref);
  if (!db_get(key.clone())) {
    db_set(db_key_gc_list((UnixTime)td::Clocks::system(), key), {});
    db_set(std::move(key), std::move(data));
  }
  if (sync) {
    co_await db_sync();
  }
  co_return {};
}

void TempArchive::update_handle(BlockHandle handle) {
  if (!handle->need_flush()) {
    return;
  }
  td::BufferSlice key = db_key_block_handle(handle->id());
  if (!db_get(key.clone())) {
    db_set(db_key_gc_list((UnixTime)td::Clocks::system(), key), {});
  }
  do {
    auto version = handle->version();
    db_set(key.clone(), handle->serialize());
    handle->flushed_upto(version);
  } while (handle->need_flush());
}

void TempArchive::iterate_block_handles(std::function<void(const BlockHandleInterface&)> f) {
  td::uint32 range_start = ton_api::db_temp_key_blockHandle::ID;
  td::uint32 range_end = ton_api::db_temp_key_blockHandle::ID + 1;
  db_for_each_in_range(td::BufferSlice{(char*)&range_start, 4}, td::BufferSlice{(char*)&range_end, 4},
                       [&](td::Slice key, td::Slice value) {
                         auto r_key = fetch_tl_object<ton_api::db_blockdb_key_value>(key, true);
                         if (r_key.is_error()) {
                           return true;
                         }
                         auto r_handle = create_block_handle(value);
                         if (r_handle.is_ok()) {
                           f(*r_handle.ok());
                         }
                         return true;
                       });
}

void TempArchive::gc(UnixTime gc_ts) {
  std::vector<td::BufferSlice> to_erase;
  bool complete = true;
  db_for_each_in_range(db_key_gc_list(0, {}), db_key_gc_list(gc_ts, {}), [&](td::Slice key, td::Slice) {
    if (to_erase.size() >= 1000) {
      complete = false;
      return false;
    }
    to_erase.emplace_back(key.substr(8));
    to_erase.emplace_back(key);
    return true;
  });
  for (auto& key : to_erase) {
    db_erase(std::move(key));
  }
  if (!complete) {
    td::actor::send_closure(actor_id(this), &TempArchive::gc, gc_ts);
  }
}

td::optional<td::BufferSlice> TempArchive::db_get(td::BufferSlice key) {
  auto it = db_pending_writes_.find(key);
  if (it != db_pending_writes_.end()) {
    if (it->second.first) {
      return it->second.first.value().clone();
    }
    return {};
  }
  std::string value;
  auto result = db_snapshot_->get(key, value).ensure().move_as_ok();
  if (result == td::KeyValue::GetStatus::NotFound) {
    return {};
  }
  return td::BufferSlice{value};
}

void TempArchive::db_set(td::BufferSlice key, td::BufferSlice value) {
  db_pending_writes_[key.clone()] = {value.clone(), db_write_idx_};
  td::actor::send_closure(db_writer_, &DbWriter::set, std::move(key), std::move(value), db_write_idx_++);
}

void TempArchive::db_erase(td::BufferSlice key) {
  db_pending_writes_[key.clone()] = {td::optional<td::BufferSlice>{}, db_write_idx_};
  td::actor::send_closure(db_writer_, &DbWriter::erase, std::move(key), db_write_idx_++);
}

td::actor::Task<> TempArchive::db_sync() {
  co_return co_await td::actor::ask(db_writer_, &DbWriter::sync);
}

void TempArchive::db_for_each_in_range(td::BufferSlice range_begin, td::BufferSlice range_end,
                                       std::function<bool(td::Slice, td::Slice)> f) {
  auto it = db_pending_writes_.lower_bound(range_begin);
  bool interrupted = false;
  auto S = db_snapshot_->for_each_in_range(range_begin, range_end, [&](td::Slice key, td::Slice value) -> td::Status {
    while (it != db_pending_writes_.end() && it->first < range_end) {
      const td::BufferSlice& pending_key = it->first;
      if (key < pending_key) {
        break;
      }
      if (it->second.first) {
        if (!f(it->first, it->second.first.value())) {
          interrupted = true;
          return td::Status::Error("interrupted");
        }
      }
      ++it;
      if (key == pending_key) {
        return td::Status::OK();
      }
    }
    if (!f(key, value)) {
      interrupted = true;
      return td::Status::Error("interrupted");
    }
    return td::Status::OK();
  });
  if (interrupted) {
    return;
  }
  S.ensure();
  while (it != db_pending_writes_.end() && it->first < range_end) {
    if (it->second.first) {
      if (!f(it->first, it->second.first.value())) {
        return;
      }
    }
    ++it;
  }
}

void TempArchive::DbWriter::set(td::BufferSlice key, td::BufferSlice value, td::uint64 write_idx) {
  begin_transaction();
  kv_->set(key, value).ensure();
  last_write_idx_ = write_idx;
}

void TempArchive::DbWriter::erase(td::BufferSlice key, td::uint64 write_idx) {
  begin_transaction();
  kv_->erase(key).ensure();
  last_write_idx_ = write_idx;
}

void TempArchive::DbWriter::sync() {
  commit_transaction();
}

void TempArchive::DbWriter::alarm() {
  commit_transaction();
}

void TempArchive::DbWriter::begin_transaction() {
  if (transaction_active_) {
    return;
  }
  transaction_active_ = true;
  kv_->begin_transaction().ensure();
  alarm_timestamp() = td::Timestamp::in(SYNC_IN);
}

void TempArchive::DbWriter::commit_transaction() {
  if (!transaction_active_) {
    return;
  }
  transaction_active_ = false;
  kv_->commit_transaction().ensure();
  td::actor::send_closure(parent_, &TempArchive::db_update_snapshot, kv_->snapshot(), last_write_idx_);
}

void TempArchive::db_update_snapshot(std::unique_ptr<td::KeyValueReader> snapshot, td::uint64 last_write_idx) {
  db_snapshot_ = std::move(snapshot);
  for (auto it = db_pending_writes_.begin(); it != db_pending_writes_.end();) {
    if (it->second.second <= last_write_idx) {
      it = db_pending_writes_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace ton::validator
