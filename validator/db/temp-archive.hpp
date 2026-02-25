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

#pragma once
#include <string>

#include "interfaces/block-handle.h"
#include "td/actor/common.h"

#include "archive-slice.hpp"
#include "fileref.hpp"

namespace ton::validator {

class TempArchive : public td::actor::Actor {
 public:
  explicit TempArchive(std::string db_path, DbStatistics statistics)
      : db_path_(std::move(db_path)), db_statistics_(std::move(statistics)) {
  }

  void start_up() override;

  td::Result<td::BufferSlice> get_file(FileReference file_ref);
  td::Result<BlockHandle> get_handle(BlockIdExt block_id);
  void add_file(FileReference file_ref, td::BufferSlice data);
  void update_handle(BlockHandle handle);
  void iterate_block_handles(std::function<void(const BlockHandleInterface&)> f);
  void gc(UnixTime gc_ts);
  td::actor::Task<> sync();

  void remove_handle(BlockIdExt block_id);
  void remove_file(FileReference file_ref);

 private:
  std::string db_path_;
  DbStatistics db_statistics_;

  td::optional<td::BufferSlice> db_get(td::BufferSlice key);
  void db_set(td::BufferSlice key, td::BufferSlice value);
  void db_erase(td::BufferSlice key);
  void db_for_each_in_range(td::BufferSlice range_begin, td::BufferSlice range_end,
                            std::function<bool(td::Slice, td::Slice)> f);

  class DbWriter : public td::actor::Actor {
   public:
    DbWriter(td::actor::ActorId<TempArchive> parent, std::unique_ptr<td::KeyValue> kv)
        : parent_(parent), kv_(std::move(kv)) {
    }

    void set(td::BufferSlice key, td::BufferSlice value, td::uint64 write_idx);
    void erase(td::BufferSlice key, td::uint64 write_idx);
    void sync();

    void alarm() override;

   private:
    td::actor::ActorId<TempArchive> parent_;
    std::unique_ptr<td::KeyValue> kv_;
    bool transaction_active_ = false;
    td::uint64 last_write_idx_ = 0;

    void begin_transaction();
    void commit_transaction();

    static constexpr double SYNC_IN = 1.0;
  };
  td::actor::ActorOwn<DbWriter> db_writer_;
  std::unique_ptr<td::KeyValueReader> db_snapshot_;
  std::map<td::BufferSlice, std::pair<td::optional<td::BufferSlice>, td::uint64>> db_pending_writes_;
  td::uint64 db_write_idx_ = 1;

  void db_update_snapshot(std::unique_ptr<td::KeyValueReader> snapshot, td::uint64 last_write_idx);
};

}  // namespace ton::validator
