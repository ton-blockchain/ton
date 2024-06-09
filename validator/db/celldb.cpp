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
#include "celldb.hpp"
#include "rootdb.hpp"

#include "td/db/RocksDb.h"
#include "td/utils/filesystem.h"

#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "common/delay.h"

namespace ton {

namespace validator {

class CellDbAsyncExecutor : public vm::DynamicBagOfCellsDb::AsyncExecutor {
 public:
  explicit CellDbAsyncExecutor(td::actor::ActorId<CellDbBase> cell_db) : cell_db_(std::move(cell_db)) {
  }

  void execute_async(std::function<void()> f) override {
    class Runner : public td::actor::Actor {
     public:
      explicit Runner(std::function<void()> f) : f_(std::move(f)) {}
      void start_up() override {
        f_();
        stop();
      }
     private:
      std::function<void()> f_;
    };
    td::actor::create_actor<Runner>("executeasync", std::move(f)).release();
  }

  void execute_sync(std::function<void()> f) override {
    td::actor::send_closure(cell_db_, &CellDbBase::execute_sync, std::move(f));
  }
 private:
  td::actor::ActorId<CellDbBase> cell_db_;
};

void CellDbBase::start_up() {
  async_executor = std::make_shared<CellDbAsyncExecutor>(actor_id(this));
}

void CellDbBase::execute_sync(std::function<void()> f) {
  f();
}

CellDbIn::CellDbIn(td::actor::ActorId<RootDb> root_db, td::actor::ActorId<CellDb> parent, std::string path,
                   td::Ref<ValidatorManagerOptions> opts)
    : root_db_(root_db), parent_(parent), path_(std::move(path)), opts_(opts) {
}

void CellDbIn::start_up() {
  on_load_callback_ = [actor = std::make_shared<td::actor::ActorOwn<MigrationProxy>>(
                           td::actor::create_actor<MigrationProxy>("celldbmigration", actor_id(this))),
                       compress_depth = opts_->get_celldb_compress_depth()](const vm::CellLoader::LoadResult& res) {
    if (res.cell_.is_null()) {
      return;
    }
    bool expected_stored_boc = res.cell_->get_depth() == compress_depth && compress_depth != 0;
    if (expected_stored_boc != res.stored_boc_) {
      td::actor::send_closure(*actor, &CellDbIn::MigrationProxy::migrate_cell,
                              td::Bits256{res.cell_->get_hash().bits()});
    }
  };

  CellDbBase::start_up();
  if (!opts_->get_disable_rocksdb_stats()) {
    statistics_ = td::RocksDb::create_statistics();
    statistics_flush_at_ = td::Timestamp::in(60.0);
  }
  td::RocksDbOptions db_options;
  db_options.statistics = statistics_;
  if (opts_->get_celldb_cache_size()) {
    db_options.block_cache = td::RocksDb::create_cache(opts_->get_celldb_cache_size().value());
    LOG(WARNING) << "Set CellDb block cache size to " << td::format::as_size(opts_->get_celldb_cache_size().value());
  }
  db_options.use_direct_reads = opts_->get_celldb_direct_io();
  cell_db_ = std::make_shared<td::RocksDb>(td::RocksDb::open(path_, std::move(db_options)).move_as_ok());


  boc_ = vm::DynamicBagOfCellsDb::create();
  boc_->set_celldb_compress_depth(opts_->get_celldb_compress_depth());
  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  alarm_timestamp() = td::Timestamp::in(10.0);

  auto empty = get_empty_key_hash();
  if (get_block(empty).is_error()) {
    DbEntry e{get_empty_key(), empty, empty, RootHash::zero()};
    cell_db_->begin_write_batch().ensure();
    set_block(empty, std::move(e));
    cell_db_->commit_write_batch().ensure();
  }

  if (opts_->get_celldb_preload_all()) {
    // Iterate whole DB in a separate thread
    delay_action([snapshot = cell_db_->snapshot()]() {
      LOG(WARNING) << "CellDb: pre-loading all keys";
      td::uint64 total = 0;
      td::Timer timer;
      auto S = snapshot->for_each([&](td::Slice, td::Slice) {
        ++total;
        if (total % 1000000 == 0) {
          LOG(INFO) << "CellDb: iterated " << total << " keys";
        }
        return td::Status::OK();
      });
      if (S.is_error()) {
        LOG(ERROR) << "CellDb: pre-load failed: " << S.move_as_error();
      } else {
      LOG(WARNING) << "CellDb: iterated " << total << " keys in " << timer.elapsed() << "s";
      }
    }, td::Timestamp::now());
  }
}

void CellDbIn::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  boc_->load_cell_async(hash.as_slice(), async_executor, std::move(promise));
}

void CellDbIn::store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise) {
  td::PerfWarningTimer timer{"storecell", 0.1};
  auto key_hash = get_key_hash(block_id);
  auto R = get_block(key_hash);
  // duplicate
  if (R.is_ok()) {
    promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
    return;
  }

  auto empty = get_empty_key_hash();
  auto ER = get_block(empty);
  ER.ensure();
  auto E = ER.move_as_ok();

  auto PR = get_block(E.prev);
  PR.ensure();
  auto P = PR.move_as_ok();
  CHECK(P.next == empty);

  DbEntry D{block_id, E.prev, empty, cell->get_hash().bits()};

  E.prev = key_hash;
  P.next = key_hash;

  if (P.is_empty()) {
    E.next = key_hash;
    P.prev = key_hash;
  }

  boc_->inc(cell);
  boc_->prepare_commit().ensure();
  vm::CellStorer stor{*cell_db_.get()};
  cell_db_->begin_write_batch().ensure();
  boc_->commit(stor).ensure();
  set_block(empty, std::move(E));
  set_block(D.prev, std::move(P));
  set_block(key_hash, std::move(D));
  cell_db_->commit_write_batch().ensure();

  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
  if (!opts_->get_disable_rocksdb_stats()) {
    cell_db_statistics_.store_cell_time_.insert(timer.elapsed() * 1e6);
  }
}

void CellDbIn::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  promise.set_result(boc_->get_cell_db_reader());
}

void CellDbIn::flush_db_stats() {
  auto stats = td::RocksDb::statistics_to_string(statistics_) + cell_db_statistics_.to_string();
  auto to_file_r =
      td::FileFd::open(path_ + "/db_stats.txt", td::FileFd::Truncate | td::FileFd::Create | td::FileFd::Write, 0644);
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
  td::RocksDb::reset_statistics(statistics_);
  cell_db_statistics_.clear();
}

void CellDbIn::alarm() {
  if (statistics_flush_at_ && statistics_flush_at_.is_in_past()) {
    statistics_flush_at_ = td::Timestamp::in(60.0);
    flush_db_stats();
  }

  if (migrate_after_ && migrate_after_.is_in_past()) {
    migrate_cells();
  }
  if (migration_stats_ && migration_stats_->end_at_.is_in_past()) {
    LOG(INFO) << "CellDb migration, " << migration_stats_->start_.elapsed()
              << "s stats: batches=" << migration_stats_->batches_ << " migrated=" << migration_stats_->migrated_cells_
              << " checked=" << migration_stats_->checked_cells_ << " time=" << migration_stats_->total_time_
              << " queue_size=" << cells_to_migrate_.size();
    migration_stats_ = {};
  }
  auto E = get_block(get_empty_key_hash()).move_as_ok();
  auto N = get_block(E.next).move_as_ok();
  if (N.is_empty()) {
    alarm_timestamp() = td::Timestamp::in(0.1);
    return;
  }

  auto block_id = N.block_id;
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id](td::Result<bool> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &CellDbIn::skip_gc);
    } else {
      auto value = R.move_as_ok();
      if (!value) {
        td::actor::send_closure(SelfId, &CellDbIn::skip_gc);
      } else {
        td::actor::send_closure(SelfId, &CellDbIn::gc, block_id);
      }
    }
  });
  td::actor::send_closure(root_db_, &RootDb::allow_state_gc, block_id, std::move(P));
}

void CellDbIn::gc(BlockIdExt block_id) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &CellDbIn::gc_cont, R.move_as_ok());
  });
  td::actor::send_closure(root_db_, &RootDb::get_block_handle_external, block_id, false, std::move(P));
}

void CellDbIn::gc_cont(BlockHandle handle) {
  if (!handle->inited_state_boc()) {
    LOG(WARNING) << "inited_state_boc=false, but state in db. blockid=" << handle->id();
  }
  handle->set_deleted_state_boc();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &CellDbIn::gc_cont2, handle);
  });

  td::actor::send_closure(root_db_, &RootDb::store_block_handle, handle, std::move(P));
}

void CellDbIn::gc_cont2(BlockHandle handle) {
  td::PerfWarningTimer timer{"gccell", 0.1};

  auto key_hash = get_key_hash(handle->id());
  auto FR = get_block(key_hash);
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

  auto cell = boc_->load_cell(F.root_hash.as_slice()).move_as_ok();

  boc_->dec(cell);
  boc_->prepare_commit().ensure();
  vm::CellStorer stor{*cell_db_};
  cell_db_->begin_write_batch().ensure();
  boc_->commit(stor).ensure();
  cell_db_->erase(get_key(key_hash)).ensure();
  set_block(F.prev, std::move(P));
  set_block(F.next, std::move(N));
  cell_db_->commit_write_batch().ensure();
  alarm_timestamp() = td::Timestamp::now();

  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  DCHECK(get_block(key_hash).is_error());
  if (!opts_->get_disable_rocksdb_stats()) {
    cell_db_statistics_.gc_cell_time_.insert(timer.elapsed() * 1e6);
  }
}

void CellDbIn::skip_gc() {
  alarm_timestamp() = td::Timestamp::in(1.0);
}

std::string CellDbIn::get_key(KeyHash key_hash) {
  if (!key_hash.is_zero()) {
    return PSTRING() << "desc" << key_hash;
  } else {
    return "desczero";
  }
}

CellDbIn::KeyHash CellDbIn::get_key_hash(BlockIdExt block_id) {
  if (block_id.is_valid()) {
    return get_tl_object_sha_bits256(create_tl_block_id(block_id));
  } else {
    return KeyHash::zero();
  }
}

BlockIdExt CellDbIn::get_empty_key() {
  return BlockIdExt{workchainInvalid, 0, 0, RootHash::zero(), FileHash::zero()};
}

CellDbIn::KeyHash CellDbIn::get_empty_key_hash() {
  return KeyHash::zero();
}

td::Result<CellDbIn::DbEntry> CellDbIn::get_block(KeyHash key_hash) {
  const auto key = get_key(key_hash);
  std::string value;
  auto R = cell_db_->get(td::as_slice(key), value);
  R.ensure();
  auto S = R.move_as_ok();
  if (S == td::KeyValue::GetStatus::NotFound) {
    return td::Status::Error(ErrorCode::notready, "not in db");
  }
  auto obj = fetch_tl_object<ton_api::db_celldb_value>(td::BufferSlice{value}, true);
  obj.ensure();
  return DbEntry{obj.move_as_ok()};
}

void CellDbIn::set_block(KeyHash key_hash, DbEntry e) {
  const auto key = get_key(key_hash);
  cell_db_->set(td::as_slice(key), e.release()).ensure();
}

void CellDbIn::migrate_cell(td::Bits256 hash) {
  cells_to_migrate_.insert(hash);
  if (!migration_active_) {
    migration_active_ = true;
    migrate_after_ = td::Timestamp::in(10.0);
  }
}

void CellDbIn::migrate_cells() {
  migrate_after_ = td::Timestamp::never();
  if (cells_to_migrate_.empty()) {
    migration_active_ = false;
    return;
  }
  td::Timer timer;
  if (!migration_stats_) {
    migration_stats_ = std::make_unique<MigrationStats>();
  }
  vm::CellStorer stor{*cell_db_};
  auto loader = std::make_unique<vm::CellLoader>(cell_db_->snapshot());
  boc_->set_loader(std::make_unique<vm::CellLoader>(*loader)).ensure();
  cell_db_->begin_write_batch().ensure();
  td::uint32 checked = 0, migrated = 0;
  for (auto it = cells_to_migrate_.begin(); it != cells_to_migrate_.end() && checked < 128; ) {
    ++checked;
    td::Bits256 hash = *it;
    it = cells_to_migrate_.erase(it);
    auto R = loader->load(hash.as_slice(), true, boc_->as_ext_cell_creator());
    if (R.is_error()) {
      continue;
    }
    if (R.ok().status == vm::CellLoader::LoadResult::NotFound) {
      continue;
    }
    bool expected_stored_boc =
        R.ok().cell_->get_depth() == opts_->get_celldb_compress_depth() && opts_->get_celldb_compress_depth() != 0;
    if (expected_stored_boc != R.ok().stored_boc_) {
      ++migrated;
      stor.set(R.ok().refcnt(), R.ok().cell_, expected_stored_boc).ensure();
    }
  }
  cell_db_->commit_write_batch().ensure();
  boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
  td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());

  double time = timer.elapsed();
  LOG(DEBUG) << "CellDb migration: migrated=" << migrated << " checked=" << checked << " time=" << time;
  ++migration_stats_->batches_;
  migration_stats_->migrated_cells_ += migrated;
  migration_stats_->checked_cells_ += checked;
  migration_stats_->total_time_ += time;

  if (cells_to_migrate_.empty()) {
    migration_active_ = false;
  } else {
    delay_action([SelfId = actor_id(this)] { td::actor::send_closure(SelfId, &CellDbIn::migrate_cells); },
                 td::Timestamp::in(time * 2));
  }
}

void CellDb::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  if (!started_) {
    td::actor::send_closure(cell_db_, &CellDbIn::load_cell, hash, std::move(promise));
  } else {
    auto P = td::PromiseCreator::lambda(
        [cell_db_in = cell_db_.get(), hash, promise = std::move(promise)](td::Result<td::Ref<vm::DataCell>> R) mutable {
          if (R.is_error()) {
            td::actor::send_closure(cell_db_in, &CellDbIn::load_cell, hash, std::move(promise));
          } else {
            promise.set_result(R.move_as_ok());
          }
        });
    boc_->load_cell_async(hash.as_slice(), async_executor, std::move(P));
  }
}

void CellDb::store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise) {
  td::actor::send_closure(cell_db_, &CellDbIn::store_cell, block_id, std::move(cell), std::move(promise));
}

void CellDb::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(cell_db_, &CellDbIn::get_cell_db_reader, std::move(promise));
}

void CellDb::start_up() {
  CellDbBase::start_up();
  boc_ = vm::DynamicBagOfCellsDb::create();
  boc_->set_celldb_compress_depth(opts_->get_celldb_compress_depth());
  cell_db_ = td::actor::create_actor<CellDbIn>("celldbin", root_db_, actor_id(this), path_, opts_);
  on_load_callback_ = [actor = std::make_shared<td::actor::ActorOwn<CellDbIn::MigrationProxy>>(
                           td::actor::create_actor<CellDbIn::MigrationProxy>("celldbmigration", cell_db_.get())),
                       compress_depth = opts_->get_celldb_compress_depth()](const vm::CellLoader::LoadResult& res) {
    if (res.cell_.is_null()) {
      return;
    }
    bool expected_stored_boc = res.cell_->get_depth() == compress_depth && compress_depth != 0;
    if (expected_stored_boc != res.stored_boc_) {
      td::actor::send_closure(*actor, &CellDbIn::MigrationProxy::migrate_cell,
                              td::Bits256{res.cell_->get_hash().bits()});
    }
  };
}

CellDbIn::DbEntry::DbEntry(tl_object_ptr<ton_api::db_celldb_value> entry)
    : block_id(create_block_id(entry->block_id_))
    , prev(entry->prev_)
    , next(entry->next_)
    , root_hash(entry->root_hash_) {
}

td::BufferSlice CellDbIn::DbEntry::release() {
  return create_serialize_tl_object<ton_api::db_celldb_value>(create_tl_block_id(block_id), prev, next, root_hash);
}

std::string CellDbIn::CellDbStatistics::to_string() {
  td::StringBuilder ss;
  ss << "ton.celldb.store_cell.micros " << store_cell_time_.to_string() << "\n";
  ss << "ton.celldb.gc_cell.micros " << gc_cell_time_.to_string() << "\n";
  ss << "ton.celldb.total_time.micros : " << (td::Timestamp::now().at() - stats_start_time_.at()) * 1e6 << "\n";
  return ss.as_cslice().str();
}

}  // namespace validator

}  // namespace ton
