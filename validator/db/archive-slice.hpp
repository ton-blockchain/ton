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
#pragma once

#include "validator/interfaces/db.h"
#include "package.hpp"
#include "fileref.hpp"

namespace ton {

namespace validator {

class PackageWriter : public td::actor::Actor {
 public:
  PackageWriter(std::shared_ptr<Package> package) : package_(std::move(package)) {
  }

  void append(std::string filename, td::BufferSlice data, td::Promise<std::pair<td::uint64, td::uint64>> promise);
  void set_async_mode(bool mode, td::Promise<td::Unit> promise) {
    async_mode_ = mode;
    if (!async_mode_) {
      package_->sync();
    }
    promise.set_value(td::Unit());
  }

 private:
  std::shared_ptr<Package> package_;
  bool async_mode_ = false;
};

class ArchiveSlice : public td::actor::Actor {
 public:
  ArchiveSlice(td::uint32 archive_id, bool key_blocks_only, bool temp, std::string prefix);

  void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise) {
    promise.set_result(archive_id_);
  }

  void add_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void update_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);
  void get_temp_handle(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise);
  void get_file(FileReference ref_id, td::Promise<td::BufferSlice> promise);

  /* from LTDB */
  void get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<ConstBlockHandle> promise);
  void get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<ConstBlockHandle> promise);
  void get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise);
  void get_block_common(AccountIdPrefixFull account_id,
                        std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                        std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                        td::Promise<ConstBlockHandle> promise);

  void get_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit, td::Promise<td::BufferSlice> promise);

  void start_up() override;
  void destroy(td::Promise<td::Unit> promise);

  void begin_transaction();
  void commit_transaction();
  void set_async_mode(bool mode, td::Promise<td::Unit> promise);

 private:
  void written_data(BlockHandle handle, td::Promise<td::Unit> promise);
  void add_file_cont(FileReference ref_id, td::uint64 offset, td::uint64 size, td::Promise<td::Unit> promise);

  /* ltdb */
  td::BufferSlice get_db_key_lt_desc(ShardIdFull shard);
  td::BufferSlice get_db_key_lt_el(ShardIdFull shard, td::uint32 idx);
  td::BufferSlice get_db_key_block_info(BlockIdExt block_id);
  td::BufferSlice get_lt_from_db(ShardIdFull shard, td::uint32 idx);

  td::uint32 archive_id_;

  bool key_blocks_only_;
  bool temp_;

  bool destroyed_ = false;
  bool async_mode_ = false;
  bool huge_transaction_started_ = false;
  td::uint32 huge_transaction_size_ = 0;

  std::string prefix_;
  std::shared_ptr<Package> package_;
  std::shared_ptr<td::KeyValue> kv_;
  td::actor::ActorOwn<PackageWriter> writer_;
};

}  // namespace validator

}  // namespace ton
