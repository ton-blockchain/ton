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
#include "td/db/RocksDbSecondary.h"

#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/utilities/transaction.h"

namespace td {
namespace {
class NullLogger final : public rocksdb::Logger {
 public:
  void Logv(const char *, va_list) override {
  }
  void Logv(rocksdb::InfoLogLevel, const char *, va_list) override {
  }
};

static Status from_rocksdb(rocksdb::Status status) {
  if (status.ok()) {
    return Status::OK();
  }
  return Status::Error(status.ToString());
}
static Slice from_rocksdb(rocksdb::Slice slice) {
  return Slice(slice.data(), slice.size());
}
static rocksdb::Slice to_rocksdb(Slice slice) {
  return rocksdb::Slice(slice.data(), slice.size());
}
}  // namespace

Status RocksDbSecondary::destroy(Slice path) {
  return from_rocksdb(rocksdb::DestroyDB(path.str(), {}));
}

RocksDbSecondary::RocksDbSecondary(RocksDbSecondary &&) = default;
RocksDbSecondary &RocksDbSecondary::operator=(RocksDbSecondary &&) = default;

RocksDbSecondary::~RocksDbSecondary() {
  if (!db_) {
    return;
  }
  end_snapshot().ensure();
}

RocksDbSecondary RocksDbSecondary::clone() const {
  return RocksDbSecondary{db_, options_};
}

Result<RocksDbSecondary> RocksDbSecondary::open(std::string path, RocksDbOptions options) {
  rocksdb::DB *db;
  {
    rocksdb::Options db_options;
    db_options.merge_operator = options.merge_operator;
    db_options.compaction_filter = options.compaction_filter;

    if (!options.no_block_cache && options.block_cache == nullptr) {
      // Secondary instances must not share the implicit default cache across different DBs because recent rocksdb version it leads to NotFound error (file not in archive slice). But need to verify this again.
      // options.block_cache = rocksdb::NewLRUCache(1 << 30);
    }

    rocksdb::BlockBasedTableOptions table_options;
    if (options.no_block_cache) {
      table_options.no_block_cache = true;
    } else {
      table_options.block_cache = options.block_cache;
    }
    if (options.enable_bloom_filter) {
      table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
      if (options.two_level_index_and_filter) {
        table_options.index_type = rocksdb::BlockBasedTableOptions::IndexType::kTwoLevelIndexSearch;
        table_options.partition_filters = true;
        table_options.cache_index_and_filter_blocks = true;
        table_options.pin_l0_filter_and_index_blocks_in_cache = true;
      }
    }
    db_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    db_options.wal_recovery_mode = rocksdb::WALRecoveryMode::kTolerateCorruptedTailRecords;
    db_options.use_direct_reads = options.use_direct_reads;
    db_options.manual_wal_flush = true;
    db_options.create_if_missing = true;
    db_options.max_background_compactions = 4;
    db_options.max_background_flushes = 2;
    db_options.bytes_per_sync = 1 << 20;
    db_options.writable_file_max_buffer_size = 2 << 14;
    db_options.statistics = options.statistics;
    // A null info_log creates RocksDB's default file logger. Supply a no-op
    // logger instead: secondary reads need no on-disk state of their own.
    db_options.info_log = std::make_shared<NullLogger>();
    rocksdb::ColumnFamilyOptions cf_options(db_options);
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.push_back(rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, cf_options));
    std::vector<rocksdb::ColumnFamilyHandle *> handles;
    // OpenAsSecondary does not use secondary_path when info_log is supplied.
    TRY_STATUS(from_rocksdb(rocksdb::DB::OpenAsSecondary(db_options, path, "", column_families, &handles, &db)));
    CHECK(handles.size() == 1);
    // i can delete the handle since DBImpl is always holding a reference to
    // default column family
    delete handles[0];
  }
  TRY_STATUS(from_rocksdb(db->TryCatchUpWithPrimary()));
  return RocksDbSecondary(std::shared_ptr<rocksdb::DB>(db), std::move(options));
}

Status RocksDbSecondary::try_catch_up_with_primary() {
  return from_rocksdb(db_->TryCatchUpWithPrimary());
}

std::unique_ptr<KeyValueReader> RocksDbSecondary::snapshot() {
  auto res = std::make_unique<RocksDbSecondary>(clone());
  res->begin_snapshot().ensure();
  return std::move(res);
}

std::string RocksDbSecondary::stats() const {
  std::string out;
  db_->GetProperty("rocksdb.stats", &out);
  //db_->GetProperty("rocksdb.cur-size-all-mem-tables", &out);
  return out;
}

Result<RocksDbSecondary::GetStatus> RocksDbSecondary::get(Slice key, std::string &value) {
  rocksdb::Status status;
  status = db_->Get({}, to_rocksdb(key), &value);
  if (status.ok()) {
    return GetStatus::Ok;
  }
  if (status.code() == rocksdb::Status::kNotFound) {
    return GetStatus::NotFound;
  }
  return from_rocksdb(status);
}

Result<std::vector<RocksDbSecondary::GetStatus>> RocksDbSecondary::get_multi(td::Span<Slice> keys, std::vector<std::string> *values) {
  std::vector<rocksdb::Status> statuses(keys.size());
  std::vector<rocksdb::Slice> keys_rocksdb;
  keys_rocksdb.reserve(keys.size());
  for (auto &key : keys) {
    keys_rocksdb.push_back(to_rocksdb(key));
  }
  std::vector<rocksdb::PinnableSlice> values_rocksdb(keys.size());
  rocksdb::ReadOptions options;
  db_->MultiGet(options, db_->DefaultColumnFamily(), keys_rocksdb.size(), keys_rocksdb.data(), values_rocksdb.data(),
                statuses.data());
  std::vector<GetStatus> res(statuses.size());
  values->resize(statuses.size());
  for (size_t i = 0; i < statuses.size(); i++) {
    auto &status = statuses[i];
    if (status.ok()) {
      res[i] = GetStatus::Ok;
      values->at(i) = values_rocksdb[i].ToString();
    } else if (status.code() == rocksdb::Status::kNotFound) {
      res[i] = GetStatus::NotFound;
      values->at(i) = "";
    } else {
      return from_rocksdb(status);
    }
  }
  return res;
}

Status RocksDbSecondary::set(Slice key, Slice value) {
  CHECK(false)
}

Status RocksDbSecondary::merge(Slice key, Slice value) {
  CHECK(false)
}

Status RocksDbSecondary::erase(Slice key) {
  CHECK(false)
}

Result<size_t> RocksDbSecondary::count(Slice prefix) {
  rocksdb::ReadOptions options;
  std::unique_ptr<rocksdb::Iterator> iterator;

  iterator.reset(db_->NewIterator(options));

  size_t res = 0;
  for (iterator->Seek(to_rocksdb(prefix)); iterator->Valid(); iterator->Next()) {
    if (from_rocksdb(iterator->key()).truncate(prefix.size()) != prefix) {
      break;
    }
    res++;
  }
  if (!iterator->status().ok()) {
    return from_rocksdb(iterator->status());
  }
  return res;
}

Status RocksDbSecondary::for_each(std::function<Status(Slice, Slice)> f) {
  rocksdb::ReadOptions options;
  options.auto_prefix_mode = true;
  std::unique_ptr<rocksdb::Iterator> iterator;
  iterator.reset(db_->NewIterator(options));

  iterator->SeekToFirst();
  for (; iterator->Valid(); iterator->Next()) {
    auto key = from_rocksdb(iterator->key());
    auto value = from_rocksdb(iterator->value());
    TRY_STATUS(f(key, value));
  }
  if (!iterator->status().ok()) {
    return from_rocksdb(iterator->status());
  }
  return Status::OK();
}

Status RocksDbSecondary::for_each_in_range(Slice begin, Slice end, std::function<Status(Slice, Slice)> f) {
  rocksdb::ReadOptions options;
  options.auto_prefix_mode = true;
  std::unique_ptr<rocksdb::Iterator> iterator;
  iterator.reset(db_->NewIterator(options));

  auto comparator = rocksdb::BytewiseComparator();
  iterator->Seek(to_rocksdb(begin));
  for (; iterator->Valid(); iterator->Next()) {
    auto key = from_rocksdb(iterator->key());
    if (comparator->Compare(to_rocksdb(key), to_rocksdb(end)) >= 0) {
      break;
    }
    auto value = from_rocksdb(iterator->value());
    TRY_STATUS(f(key, value));
  }
  if (!iterator->status().ok()) {
    return from_rocksdb(iterator->status());
  }
  return td::Status::OK();
}

Status RocksDbSecondary::begin_write_batch() {
  CHECK(false)
}

Status RocksDbSecondary::begin_transaction() {
  CHECK(false)
}

Status RocksDbSecondary::commit_write_batch() {
  CHECK(false)
}

Status RocksDbSecondary::commit_transaction() {
  CHECK(false)
}

Status RocksDbSecondary::abort_write_batch() {
  CHECK(false)
}

Status RocksDbSecondary::abort_transaction() {
  CHECK(false)
}

Status RocksDbSecondary::flush() {
  CHECK(false)
}

Status RocksDbSecondary::begin_snapshot() {
  return td::Status::OK();
}

Status RocksDbSecondary::end_snapshot() {
  return td::Status::OK();
}

RocksDbSecondary::RocksDbSecondary(std::shared_ptr<rocksdb::DB> db, RocksDbOptions options)
    : db_(std::move(db)), options_(std::move(options)) {
}
}  // namespace td
