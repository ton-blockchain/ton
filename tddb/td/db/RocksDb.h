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

#if !TDDB_USE_ROCKSDB
#error "RocksDb is not supported"
#endif

#include "td/db/KeyValue.h"
#include "td/utils/Span.h"
#include "td/utils/Status.h"
#include "td/utils/optional.h"

#include "td/utils/Time.h"

#include <map>
#include <mutex>
#include <set>

#include <functional>

namespace rocksdb {
class DB;
class Comparator;
class Cache;
class OptimisticTransactionDB;
class Transaction;
class WriteBatch;
class Snapshot;
class Statistics;
class MergeOperator;
class CompactionFilter;
}  // namespace rocksdb

namespace td {
struct RocksDbSnapshotStatistics {
  void begin_snapshot(const rocksdb::Snapshot *snapshot);
  void end_snapshot(const rocksdb::Snapshot *snapshot);
  td::Timestamp oldest_snapshot_timestamp() const;
  std::string to_string() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::uintptr_t, double> id_to_ts_;
  std::set<std::pair<double, std::uintptr_t>> by_ts_;
};

struct RocksDbOptions {
  std::shared_ptr<rocksdb::Statistics> statistics = nullptr;
  std::shared_ptr<rocksdb::Cache> block_cache;  // Default - one 1G cache for all RocksDb
  std::shared_ptr<RocksDbSnapshotStatistics> snapshot_statistics = nullptr;

  std::shared_ptr<rocksdb::MergeOperator> merge_operator = nullptr;
  const rocksdb::CompactionFilter *compaction_filter = nullptr;

  bool experimental = false;
  bool no_reads = false;
  bool no_transactions = false;

  bool use_direct_reads = false;
  bool no_block_cache = false;
  bool enable_bloom_filter = false;
  bool two_level_index_and_filter = false;
};

class RocksDb : public KeyValue {
 public:
  static Status destroy(Slice path);
  RocksDb clone() const;
  static Result<RocksDb> open(std::string path, RocksDbOptions options = {});

  Result<GetStatus> get(Slice key, std::string &value) override;
  Result<std::vector<RocksDb::GetStatus>> get_multi(td::Span<Slice> keys, std::vector<std::string> *values) override;
  Status set(Slice key, Slice value) override;
  Status merge(Slice key, Slice value) override;
  Status erase(Slice key) override;
  Status run_gc() override;
  Result<size_t> count(Slice prefix) override;
  Status for_each(std::function<Status(Slice, Slice)> f) override;
  Status for_each_in_range(Slice begin, Slice end, std::function<Status(Slice, Slice)> f) override;

  Status begin_write_batch() override;
  Status commit_write_batch() override;
  Status abort_write_batch() override;

  Status begin_transaction() override;
  Status commit_transaction() override;
  Status abort_transaction() override;
  Status flush() override;

  Status begin_snapshot();
  Status end_snapshot();

  std::unique_ptr<KeyValueReader> snapshot() override;
  std::string stats() const override;

  static std::shared_ptr<rocksdb::Statistics> create_statistics();
  static std::string statistics_to_string(const std::shared_ptr<rocksdb::Statistics> &statistics);
  static void reset_statistics(const std::shared_ptr<rocksdb::Statistics> &statistics);

  static std::shared_ptr<rocksdb::Cache> create_cache(size_t capacity);

  RocksDb(RocksDb &&);
  RocksDb &operator=(RocksDb &&);
  ~RocksDb();

  std::shared_ptr<rocksdb::DB> raw_db() const {
    return db_;
  };

 private:
  std::shared_ptr<rocksdb::OptimisticTransactionDB> transaction_db_;
  std::shared_ptr<rocksdb::DB> db_;
  RocksDbOptions options_;

  std::unique_ptr<rocksdb::Transaction> transaction_;
  std::unique_ptr<rocksdb::WriteBatch> write_batch_;
  class UnreachableDeleter {
   public:
    template <class T>
    void operator()(T *) {
      UNREACHABLE();
    }
  };
  std::unique_ptr<const rocksdb::Snapshot, UnreachableDeleter> snapshot_;

  explicit RocksDb(std::shared_ptr<rocksdb::OptimisticTransactionDB> db, RocksDbOptions options);
  explicit RocksDb(std::shared_ptr<rocksdb::DB> db, RocksDbOptions options);
};
}  // namespace td
