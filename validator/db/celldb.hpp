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

#include "td/actor/actor.h"
#include "crypto/vm/db/DynamicBagOfCellsDb.h"
#include "crypto/vm/db/CellStorage.h"
#include "td/db/KeyValue.h"
#include "ton/ton-types.h"
#include "interfaces/block-handle.h"
#include "auto/tl/ton_api.h"
#include "validator.h"
#include "db-utils.h"
#include "td/db/RocksDb.h"

#include <optional>
#include <queue>

namespace rocksdb {
class Statistics;
class DB;
}  // namespace rocksdb

namespace ton {

namespace validator {

class RootDb;

class CellDb;
class CellDbAsyncExecutor;

class CellDbBase : public td::actor::Actor {
 public:
  void start_up() override;

 protected:
  std::shared_ptr<vm::DynamicBagOfCellsDb::AsyncExecutor> async_executor;

 private:
  void execute_sync(std::function<void()> f);
  friend CellDbAsyncExecutor;
};

class CellDbIn : public CellDbBase {
 public:
  using KeyHash = td::Bits256;

  std::vector<std::pair<std::string, std::string>> prepare_stats();
  void load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise);
  void store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise);
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise);

  void migrate_cell(td::Bits256 hash);

  void flush_db_stats();

  CellDbIn(td::actor::ActorId<RootDb> root_db, td::actor::ActorId<CellDb> parent, std::string path,
           td::Ref<ValidatorManagerOptions> opts);

  void validate_meta();
  void start_up() override;
  void alarm() override;

 private:
  struct DbEntry {
    BlockIdExt block_id;
    KeyHash prev;
    KeyHash next;
    RootHash root_hash;

    DbEntry(tl_object_ptr<ton_api::db_celldb_value> entry);
    DbEntry() = default;
    DbEntry(BlockIdExt block_id, KeyHash prev, KeyHash next, RootHash root_hash)
        : block_id(block_id), prev(prev), next(next), root_hash(root_hash) {
    }
    td::BufferSlice release();
    bool is_empty() const {
      return !block_id.is_valid();
    }
  };
  td::Result<DbEntry> get_block(KeyHash key);
  void set_block(KeyHash key, DbEntry e);

  static std::string get_key(KeyHash key);
  static KeyHash get_key_hash(BlockIdExt block_id);
  static BlockIdExt get_empty_key();
  KeyHash get_empty_key_hash();

  void gc(BlockIdExt block_id);
  void gc_cont(BlockHandle handle);
  void gc_cont2(BlockHandle handle);
  void skip_gc();

  void migrate_cells();

  td::actor::ActorId<RootDb> root_db_;
  td::actor::ActorId<CellDb> parent_;

  std::string path_;
  td::Ref<ValidatorManagerOptions> opts_;

  std::shared_ptr<vm::DynamicBagOfCellsDb> boc_;
  std::shared_ptr<vm::KeyValue> cell_db_;
  std::shared_ptr<rocksdb::DB> rocks_db_;

  std::function<void(const vm::CellLoader::LoadResult&)> on_load_callback_;
  std::set<td::Bits256> cells_to_migrate_;
  td::Timestamp migrate_after_ = td::Timestamp::never();
  bool migration_active_ = false;
  std::optional<double> in_memory_load_time_;

  struct MigrationStats {
    td::Timer start_;
    td::Timestamp end_at_ = td::Timestamp::in(60.0);
    size_t batches_ = 0;
    size_t migrated_cells_ = 0;
    size_t checked_cells_ = 0;
    double total_time_ = 0.0;
  };
  std::unique_ptr<MigrationStats> migration_stats_;

  struct CellDbStatistics {
    PercentileStats store_cell_time_;
    PercentileStats store_cell_prepare_time_;
    PercentileStats store_cell_write_time_;
    PercentileStats gc_cell_time_;
    td::Timestamp stats_start_time_ = td::Timestamp::now();
    std::optional<double> in_memory_load_time_;
    std::optional<vm::DynamicBagOfCellsDb::Stats> boc_stats_;

    std::vector<std::pair<std::string, std::string>> prepare_stats();
    void clear() {
      *this = CellDbStatistics{};
    }
  };

  std::shared_ptr<rocksdb::Statistics> statistics_;
  std::shared_ptr<td::RocksDbSnapshotStatistics> snapshot_statistics_;
  CellDbStatistics cell_db_statistics_;
  td::Timestamp statistics_flush_at_ = td::Timestamp::never();
  BlockSeqno last_deleted_mc_state_ = 0;

  bool db_busy_ = false;
  std::queue<td::Promise<td::Unit>> action_queue_;

  void release_db() {
    db_busy_ = false;
    while (!db_busy_ && !action_queue_.empty()) {
      auto action = std::move(action_queue_.front());
      action_queue_.pop();
      action.set_value(td::Unit());
    }
  }

 public:
  class MigrationProxy : public td::actor::Actor {
   public:
    explicit MigrationProxy(td::actor::ActorId<CellDbIn> cell_db) : cell_db_(cell_db) {
    }
    void migrate_cell(td::Bits256 hash) {
      td::actor::send_closure(cell_db_, &CellDbIn::migrate_cell, hash);
    }

   private:
    td::actor::ActorId<CellDbIn> cell_db_;
  };
};

class CellDb : public CellDbBase {
 public:
  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise);
  void load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise);
  void store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise);
  void update_snapshot(std::unique_ptr<td::KeyValueReader> snapshot) {
    CHECK(!opts_->get_celldb_in_memory());
    if (!started_) {
      alarm();
    }
    started_ = true;
    boc_->set_loader(std::make_unique<vm::CellLoader>(std::move(snapshot), on_load_callback_)).ensure();
  }
  void set_thread_safe_boc(std::shared_ptr<const vm::DynamicBagOfCellsDb> thread_safe_boc) {
    CHECK(opts_->get_celldb_in_memory() || opts_->get_celldb_v2());
    if (!started_) {
      alarm();
    }
    started_ = true;
    thread_safe_boc_ = std::move(thread_safe_boc);
  }
  void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise);

  CellDb(td::actor::ActorId<RootDb> root_db, std::string path, td::Ref<ValidatorManagerOptions> opts)
      : root_db_(root_db), path_(path), opts_(opts) {
  }

  void start_up() override;

 private:
  td::actor::ActorId<RootDb> root_db_;
  std::string path_;
  td::Ref<ValidatorManagerOptions> opts_;

  td::actor::ActorOwn<CellDbIn> cell_db_;

  std::unique_ptr<vm::DynamicBagOfCellsDb> boc_;
  std::shared_ptr<const vm::DynamicBagOfCellsDb> thread_safe_boc_;
  bool started_ = false;
  std::vector<std::pair<std::string, std::string>> prepared_stats_{{"started", "false"}};

  std::function<void(const vm::CellLoader::LoadResult&)> on_load_callback_;

  void update_stats(td::Result<std::vector<std::pair<std::string, std::string>>> stats);
  void alarm() override;
};

}  // namespace validator

}  // namespace ton
