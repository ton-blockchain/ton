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
#include "td/db/RocksDb.h"
#include <map>

namespace rocksdb {
class Statistics;
}

namespace ton {

namespace validator {

struct PackageId {
  td::uint32 id;
  bool key;
  bool temp;

  explicit PackageId(td::uint32 id, bool key, bool temp) : id(id), key(key), temp(temp) {
  }

  bool operator<(const PackageId &with) const {
    return id < with.id;
  }
  bool operator==(const PackageId &with) const {
    return id == with.id;
  }

  std::string path() const;
  std::string name() const;

  bool is_empty() const {
    return id == std::numeric_limits<td::uint32>::max();
  }
  static PackageId empty(bool key, bool temp) {
    return PackageId(std::numeric_limits<td::uint32>::max(), key, temp);
  }
};

class PackageStatistics;

struct DbStatistics {
  void init();
  std::string to_string_and_reset();

  std::shared_ptr<PackageStatistics> pack_statistics;
  std::shared_ptr<rocksdb::Statistics> rocksdb_statistics;
};

class PackageWriter : public td::actor::Actor {
 public:
  PackageWriter(std::weak_ptr<Package> package, bool async_mode = false, std::shared_ptr<PackageStatistics> statistics = nullptr)
      : package_(std::move(package)), async_mode_(async_mode), statistics_(std::move(statistics)) {
  }

  void append(std::string filename, td::BufferSlice data, td::Promise<std::pair<td::uint64, td::uint64>> promise);
  void set_async_mode(bool mode, td::Promise<td::Unit> promise) {
    async_mode_ = mode;
    if (!async_mode_) {
      auto p = package_.lock();
      if (p) {
        p->sync();
      }
    }
    promise.set_value(td::Unit());
  }

 private:
  std::weak_ptr<Package> package_;
  bool async_mode_ = false;
  std::shared_ptr<PackageStatistics> statistics_;
};

class ArchiveLru;

class ArchiveSlice : public td::actor::Actor {
 public:
  ArchiveSlice(td::uint32 archive_id, bool key_blocks_only, bool temp, bool finalized, td::uint32 shard_split_depth,
               std::string db_root, td::actor::ActorId<ArchiveLru> archive_lru, DbStatistics statistics = {});

  void get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, td::Promise<td::uint64> promise);

  void add_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void update_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);
  void get_temp_handle(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise);
  void get_file(ConstBlockHandle handle, FileReference ref_id, td::Promise<td::BufferSlice> promise);

  /* from LTDB */
  void get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<ConstBlockHandle> promise);
  void get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<ConstBlockHandle> promise);
  void get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise);
  void get_block_common(AccountIdPrefixFull account_id,
                        std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                        std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                        td::Promise<ConstBlockHandle> promise);

  void get_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit, td::Promise<td::BufferSlice> promise);

  void destroy(td::Promise<td::Unit> promise);
  void truncate(BlockSeqno masterchain_seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise);

  void set_async_mode(bool mode, td::Promise<td::Unit> promise);

  void open_files();
  void close_files();

 private:
  void before_query();
  void do_close();
  template<typename T>
  td::Promise<T> begin_async_query(td::Promise<T> promise);
  void end_async_query();

  void begin_transaction();
  void commit_transaction();

  void add_file_cont(size_t idx, FileReference ref_id, td::uint64 offset, td::uint64 size,
                     td::Promise<td::Unit> promise);

  /* ltdb */
  td::BufferSlice get_db_key_lt_desc(ShardIdFull shard);
  td::BufferSlice get_db_key_lt_el(ShardIdFull shard, td::uint32 idx);
  td::BufferSlice get_db_key_block_info(BlockIdExt block_id);

  td::uint32 archive_id_;

  bool key_blocks_only_;
  bool temp_;
  bool finalized_;
  PackageId p_id_;
  std::string db_path_;

  bool destroyed_ = false;
  bool async_mode_ = false;
  bool huge_transaction_started_ = false;
  bool sliced_mode_{false};
  td::uint32 huge_transaction_size_ = 0;
  td::uint32 slice_size_{100};
  td::uint32 shard_split_depth_ = 0;

  enum Status {
    st_closed, st_open, st_want_close
  } status_ = st_closed;
  size_t active_queries_ = 0;

  std::string db_root_;
  td::actor::ActorId<ArchiveLru> archive_lru_;
  DbStatistics statistics_;
  std::unique_ptr<td::KeyValue> kv_;

  struct PackageInfo {
    PackageInfo(std::shared_ptr<Package> package, td::actor::ActorOwn<PackageWriter> writer, BlockSeqno seqno, ShardIdFull shard_prefix,
                std::string path, td::uint32 idx, td::uint32 version)
        : package(std::move(package))
        , writer(std ::move(writer))
        , seqno(seqno)
        , shard_prefix(shard_prefix)
        , path(std::move(path))
        , idx(idx)
        , version(version) {
    }
    std::shared_ptr<Package> package;
    td::actor::ActorOwn<PackageWriter> writer;
    BlockSeqno seqno;
    ShardIdFull shard_prefix;
    std::string path;
    td::uint32 idx;
    td::uint32 version;
  };
  std::vector<PackageInfo> packages_;
  std::map<std::pair<BlockSeqno, ShardIdFull>, td::uint32> id_to_package_;

  td::Result<PackageInfo *> choose_package(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, bool force);
  void add_package(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, td::uint64 size, td::uint32 version);
  void truncate_shard(BlockSeqno masterchain_seqno, ShardIdFull shard, td::uint32 cutoff_seqno, Package *pack);
  bool truncate_block(BlockSeqno masterchain_seqno, BlockIdExt block_id, td::uint32 cutoff_seqno, Package *pack);

  void delete_handle(ConstBlockHandle handle);
  void delete_file(FileReference ref_id);
  void move_handle(ConstBlockHandle handle, Package *old_pack, Package *pack);
  void move_file(FileReference ref_id, Package *old_pack, Package *pack);

  BlockSeqno max_masterchain_seqno();

  static constexpr td::uint32 default_package_version() {
    return 1;
  }

  static const size_t ESTIMATED_DB_OPEN_FILES = 5;
};

class ArchiveLru : public td::actor::Actor {
 public:
  explicit ArchiveLru(size_t max_total_files) : max_total_files_(max_total_files) {
    CHECK(max_total_files_ > 0);
  }
  void on_query(td::actor::ActorId<ArchiveSlice> slice, PackageId id, size_t files_count);
  void set_permanent_slices(std::vector<PackageId> ids);
 private:
  size_t current_idx_ = 1;
  struct SliceInfo {
    td::actor::ActorId<ArchiveSlice> actor;
    size_t files_count = 0;
    size_t opened_idx = 0;  // 0 - not opened
    bool is_permanent = false;
  };
  std::map<std::tuple<td::uint32, bool, bool>, SliceInfo> slices_;
  std::map<size_t, PackageId> lru_;
  size_t total_files_ = 0;
  size_t max_total_files_ = 0;
  std::vector<PackageId> permanent_slices_;

  void enforce_limit();
};

}  // namespace validator

}  // namespace ton
