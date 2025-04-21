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

#include "files-async.hpp"
#include "rootdb.hpp"

#include "td/db/RocksDb.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "common/delay.h"

#include <block-auto.h>
#include <rocksdb/merge_operator.h>

namespace ton {

namespace validator {
class CellDbAsyncExecutor : public vm::DynamicBagOfCellsDb::AsyncExecutor {
 public:
  explicit CellDbAsyncExecutor(td::actor::ActorId<CellDbBase> cell_db) : cell_db_(std::move(cell_db)) {
  }

  void execute_async(std::function<void()> f) override {
    class Runner : public td::actor::Actor {
     public:
      explicit Runner(std::function<void()> f) : f_(std::move(f)) {
      }
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

struct MergeOperatorAddCellRefcnt : public rocksdb::MergeOperator {
  const char* Name() const override {
    return "MergeOperatorAddCellRefcnt";
  }
  static auto to_td(rocksdb::Slice value) -> td::Slice {
    return td::Slice(value.data(), value.size());
  }
  bool FullMergeV2(const MergeOperationInput& merge_in, MergeOperationOutput* merge_out) const override {
    CHECK(merge_in.existing_value);
    auto& value = *merge_in.existing_value;
    CHECK(merge_in.operand_list.size() >= 1);
    td::Slice diff;
    std::string diff_buf;
    if (merge_in.operand_list.size() == 1) {
      diff = to_td(merge_in.operand_list[0]);
    } else {
      diff_buf = merge_in.operand_list[0].ToString();
      for (size_t i = 1; i < merge_in.operand_list.size(); ++i) {
        vm::CellStorer::merge_refcnt_diffs(diff_buf, to_td(merge_in.operand_list[i]));
      }
      diff = diff_buf;
    }

    merge_out->new_value = value.ToString();
    vm::CellStorer::merge_value_and_refcnt_diff(merge_out->new_value, diff);
    return true;
  }
  bool PartialMerge(const rocksdb::Slice& /*key*/, const rocksdb::Slice& left, const rocksdb::Slice& right,
                    std::string* new_value, rocksdb::Logger* logger) const override {
    *new_value = left.ToString();
    vm::CellStorer::merge_refcnt_diffs(*new_value, to_td(right));
    return true;
  }
};

void CellDbIn::validate_meta() {
  LOG(INFO) << "Validating metadata\n";
  size_t max_meta_keys_loaded = opts_->get_celldb_in_memory() ? std::numeric_limits<std::size_t>::max() : 10000;
  auto meta = boc_->meta_get_all(max_meta_keys_loaded).move_as_ok();
  bool partial_check = meta.size() == max_meta_keys_loaded;
  if (partial_check) {
    LOG(ERROR) << "Too much metadata in the database, do only partial check";
  }
  size_t missing_roots = 0;
  size_t unknown_roots = 0;
  std::set<vm::CellHash> root_hashes;
  for (auto [k, v] : meta) {
    if (k == "desczero") {
      continue;
    }
    auto obj = fetch_tl_object<ton_api::db_celldb_value>(td::BufferSlice{v}, true);
    obj.ensure();
    auto entry = DbEntry{obj.move_as_ok()};
    root_hashes.insert(vm::CellHash::from_slice(entry.root_hash.as_slice()));
    auto cell = boc_->load_cell(entry.root_hash.as_slice());
    missing_roots += cell.is_error();
    LOG_IF(ERROR, cell.is_error()) << "Cannot load root from meta: " << entry.block_id.to_str() << " " << cell.error();
  }

  // load_known_roots is only supported by InMemory database, so it is ok to check all known roots here
  auto known_roots = boc_->load_known_roots().move_as_ok();
  for (auto& root : known_roots) {
    block::gen::ShardStateUnsplit::Record info;
    block::gen::OutMsgQueueInfo::Record qinfo;
    block::ShardId shard;
    if (!(tlb::unpack_cell(root, info) && shard.deserialize(info.shard_id.write()) &&
          tlb::unpack_cell(info.out_msg_queue_info, qinfo))) {
      LOG(FATAL) << "cannot create ShardDescr from a root in celldb";
    }
    if (!partial_check && !root_hashes.contains(root->get_hash())) {
      unknown_roots++;
      LOG(ERROR) << "Unknown root" << ShardIdFull(shard).to_str() << ":" << info.seq_no;
      constexpr bool delete_unknown_roots = false;
      if (delete_unknown_roots) {
        vm::CellStorer stor{*cell_db_};
        cell_db_->begin_write_batch().ensure();
        boc_->dec(root);
        boc_->commit(stor).ensure();
        cell_db_->commit_write_batch().ensure();
        if (!opts_->get_celldb_in_memory()) {
          boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
        }
        LOG(ERROR) << "Unknown root" << ShardIdFull(shard).to_str() << ":" << info.seq_no << " REMOVED";
      }
    }
  }

  LOG_IF(ERROR, missing_roots != 0) << "Missing root hashes: " << missing_roots;
  LOG_IF(ERROR, unknown_roots != 0) << "Unknown roots: " << unknown_roots;

  LOG_IF(FATAL, missing_roots != 0) << "Missing root hashes: " << missing_roots;
  LOG_IF(FATAL, unknown_roots != 0) << "Unknown roots: " << unknown_roots;
  LOG(INFO) << "Validating metadata: OK\n";
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
  td::RocksDbOptions db_options;
  if (!opts_->get_disable_rocksdb_stats()) {
    statistics_ = td::RocksDb::create_statistics();
    statistics_flush_at_ = td::Timestamp::in(60.0);
    snapshot_statistics_ = std::make_shared<td::RocksDbSnapshotStatistics>();
    db_options.snapshot_statistics = snapshot_statistics_;
  }
  db_options.statistics = statistics_;
  auto o_celldb_cache_size = opts_->get_celldb_cache_size();

  std::optional<vm::DynamicBagOfCellsDb::CreateInMemoryOptions> boc_in_memory_options;
  std::optional<vm::DynamicBagOfCellsDb::CreateV1Options> boc_v1_options;
  std::optional<vm::DynamicBagOfCellsDb::CreateV2Options> boc_v2_options;

  if (opts_->get_celldb_v2()) {
    boc_v2_options = vm::DynamicBagOfCellsDb::CreateV2Options{
        .extra_threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u),
        .executor = {},
        .cache_ttl_max = 2000,
        .cache_size_max = 1000000};
    size_t min_rocksdb_cache = std::max(size_t{1} << 30, boc_v2_options->cache_size_max * 5000);
    if (!o_celldb_cache_size || o_celldb_cache_size.value() < min_rocksdb_cache) {
      LOG(WARNING) << "Increase CellDb block cache size to " << td::format::as_size(min_rocksdb_cache) << " from "
                   << td::format::as_size(o_celldb_cache_size.value());
      o_celldb_cache_size = min_rocksdb_cache;
    }
    LOG(WARNING) << "Using V2 DynamicBagOfCells with options " << *boc_v2_options;
  } else if (opts_->get_celldb_in_memory()) {
    // default options
    boc_in_memory_options = vm::DynamicBagOfCellsDb::CreateInMemoryOptions{
        .extra_threads = std::thread::hardware_concurrency(),
        .verbose = true,
        .use_arena = false,
        .use_less_memory_during_creation = true,
    };
    LOG(WARNING) << "Using InMemory DynamicBagOfCells with options " << *boc_v2_options;
  } else {
    boc_v1_options = vm::DynamicBagOfCellsDb::CreateV1Options{};
    LOG(WARNING) << "Using V1 DynamicBagOfCells with options " << *boc_v1_options;
  }

  db_options.enable_bloom_filter = !opts_->get_celldb_disable_bloom_filter();
  db_options.two_level_index_and_filter = db_options.enable_bloom_filter 
                                && opts_->state_ttl() >= 60 * 60 * 24 * 30; // 30 days
  if (db_options.two_level_index_and_filter && !opts_->get_celldb_in_memory()) {
    o_celldb_cache_size = std::max<td::uint64>(o_celldb_cache_size ? o_celldb_cache_size.value() : 0UL, 16UL << 30);
  }

  if (o_celldb_cache_size) {
    db_options.block_cache = td::RocksDb::create_cache(o_celldb_cache_size.value());
    LOG(WARNING) << "Set CellDb block cache size to " << td::format::as_size(o_celldb_cache_size.value());
  }
  db_options.use_direct_reads = opts_->get_celldb_direct_io();

  // NB: from now on we MUST use this merge operator
  // Only V2 and InMemory BoC actually use them, but it still should be kept for V1,
  // to handle updates written by V2 or InMemory BoCs
  db_options.merge_operator = std::make_shared<MergeOperatorAddCellRefcnt>();

  if (opts_->get_celldb_in_memory()) {
    td::RocksDbOptions read_db_options;
    read_db_options.use_direct_reads = true;
    read_db_options.no_block_cache = true;
    read_db_options.block_cache = {};
    read_db_options.merge_operator = std::make_shared<MergeOperatorAddCellRefcnt>();
    LOG(WARNING) << "Loading all cells in memory (because of --celldb-in-memory)";
    td::Timer timer;
    auto read_cell_db =
        std::make_shared<td::RocksDb>(td::RocksDb::open(path_, std::move(read_db_options)).move_as_ok());
    boc_ = vm::DynamicBagOfCellsDb::create_in_memory(read_cell_db.get(), *boc_in_memory_options);
    in_memory_load_time_ = timer.elapsed();

    // no reads will be allowed from rocksdb, only writes
    db_options.no_reads = true;
  }

  auto rocks_db = std::make_shared<td::RocksDb>(td::RocksDb::open(path_, std::move(db_options)).move_as_ok());
  rocks_db_ = rocks_db->raw_db();
  cell_db_ = std::move(rocks_db);
  if (!opts_->get_celldb_in_memory()) {
    if (opts_->get_celldb_v2()) {
      boc_ = vm::DynamicBagOfCellsDb::create_v2(*boc_v2_options);
    } else {
      boc_ = vm::DynamicBagOfCellsDb::create(*boc_v1_options);
    }
    boc_->set_celldb_compress_depth(opts_->get_celldb_compress_depth());
    boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
  }

  validate_meta();

  alarm_timestamp() = td::Timestamp::in(10.0);

  auto empty = get_empty_key_hash();
  if (get_block(empty).is_error()) {
    DbEntry e{get_empty_key(), empty, empty, RootHash::zero()};
    vm::CellStorer stor{*cell_db_};
    cell_db_->begin_write_batch().ensure();
    set_block(empty, std::move(e));
    boc_->commit(stor);
    cell_db_->commit_write_batch().ensure();
    if (!opts_->get_celldb_in_memory()) {
      boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
    }
  }

  if (opts_->get_celldb_v2() || opts_->get_celldb_in_memory()) {
    send_closure(parent_, &CellDb::set_thread_safe_boc, boc_);
  } else {
    send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());
  }

  if (opts_->get_celldb_preload_all()) {
    // Iterate whole DB in a separate thread
    delay_action(
        [snapshot = cell_db_->snapshot()]() {
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
        },
        td::Timestamp::now());
  }

  {
    std::string key = "stats.last_deleted_mc_seqno", value;
    auto R = boc_->meta_get(td::as_slice(key), value);
    R.ensure();
    if (R.ok() == td::KeyValue::GetStatus::Ok) {
      auto r_value = td::to_integer_safe<BlockSeqno>(value);
      r_value.ensure();
      last_deleted_mc_state_ = r_value.move_as_ok();
    }
  }
}

void CellDbIn::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  if (db_busy_) {
    action_queue_.push([self = this, hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      R.ensure();
      self->load_cell(hash, std::move(promise));
    });
    return;
  }
  if (opts_->get_celldb_in_memory()) {
    auto result = boc_->load_root(hash.as_slice());
    async_apply("load_cell_result", std::move(promise), std::move(result));
    return;
  }
  auto cell = boc_->load_cell(hash.as_slice());
  delay_action(
      [cell = std::move(cell), promise = std::move(promise)]() mutable { promise.set_result(std::move(cell)); },
      td::Timestamp::now());
}

void CellDbIn::store_cell(BlockIdExt block_id, td::Ref<vm::Cell> cell, td::Promise<td::Ref<vm::DataCell>> promise) {
  if (db_busy_) {
    action_queue_.push(
        [self = this, block_id, cell = std::move(cell), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          R.ensure();
          self->store_cell(block_id, std::move(cell), std::move(promise));
        });
    return;
  }
  td::PerfWarningTimer timer{"storecell", 0.1};
  auto key_hash = get_key_hash(block_id);
  auto R = get_block(key_hash);
  // duplicate
  if (R.is_ok()) {
    promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
    return;
  }

  boc_->inc(cell);
  db_busy_ = true;
  boc_->prepare_commit_async(async_executor, [=, this, SelfId = actor_id(this), timer = std::move(timer),
                                              timer_prepare = td::Timer{}, promise = std::move(promise),
                                              cell = std::move(cell)](td::Result<td::Unit> Res) mutable {
    Res.ensure();
    timer_prepare.pause();
    td::actor::send_lambda(
        SelfId, [=, this, timer = std::move(timer), promise = std::move(promise), cell = std::move(cell)]() mutable {
          TD_PERF_COUNTER(celldb_store_cell);
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
          td::Timer timer_write;
          vm::CellStorer stor{*cell_db_};
          cell_db_->begin_write_batch().ensure();
          set_block(get_empty_key_hash(), std::move(E));
          set_block(D.prev, std::move(P));
          set_block(key_hash, std::move(D));
          boc_->commit(stor).ensure();
          cell_db_->commit_write_batch().ensure();
          timer_write.pause();

          if (!opts_->get_celldb_in_memory()) {
            boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
            td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());
          }

          promise.set_result(boc_->load_cell(cell->get_hash().as_slice()));
          if (!opts_->get_disable_rocksdb_stats()) {
            cell_db_statistics_.store_cell_time_.insert(timer.elapsed() * 1e6);
            cell_db_statistics_.store_cell_prepare_time_.insert(timer_prepare.elapsed() * 1e6);
            cell_db_statistics_.store_cell_write_time_.insert(timer_write.elapsed() * 1e6);
          }
          LOG(DEBUG) << "Stored state " << block_id.to_str();
          release_db();
        });
  });
}

void CellDbIn::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  if (db_busy_) {
    action_queue_.push([self = this, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      R.ensure();
      self->get_cell_db_reader(std::move(promise));
    });
    return;
  }
  promise.set_result(boc_->get_cell_db_reader());
}

std::vector<std::pair<std::string, std::string>> CellDbIn::prepare_stats() {
  TD_PERF_COUNTER(celldb_prepare_stats);
  auto r_boc_stats = boc_->get_stats();
  if (r_boc_stats.is_ok()) {
    cell_db_statistics_.boc_stats_ = r_boc_stats.move_as_ok();
  }
  cell_db_statistics_.in_memory_load_time_ = in_memory_load_time_;
  auto stats = cell_db_statistics_.prepare_stats();
  auto add_stat = [&](const auto& key, const auto& value) { stats.emplace_back(key, PSTRING() << value); };

  add_stat("started", "true");
  auto r_mem_stat = td::mem_stat();
  auto r_total_mem_stat = td::get_total_mem_stat();
  td::uint64 celldb_size = 0;
  bool ok_celldb_size = rocks_db_->GetIntProperty("rocksdb.total-sst-files-size", &celldb_size);
  if (celldb_size > 0 && r_mem_stat.is_ok() && r_total_mem_stat.is_ok() && ok_celldb_size) {
    auto mem_stat = r_mem_stat.move_as_ok();
    auto total_mem_stat = r_total_mem_stat.move_as_ok();
    add_stat("rss", td::format::as_size(mem_stat.resident_size_));
    add_stat("available_ram", td::format::as_size(total_mem_stat.available_ram));
    add_stat("total_ram", td::format::as_size(total_mem_stat.total_ram));
    add_stat("actual_ram_to_celldb_ratio", double(total_mem_stat.available_ram) / double(celldb_size));
    add_stat("if_restarted_ram_to_celldb_ratio",
             double(total_mem_stat.available_ram + mem_stat.resident_size_ - 10 * (td::uint64(1) << 30)) /
                 double(celldb_size));
    add_stat("max_possible_ram_to_celldb_ratio", double(total_mem_stat.total_ram) / double(celldb_size));
  }
  stats.emplace_back("last_deleted_mc_state", td::to_string(last_deleted_mc_state_));

  return stats;
  // do not clear statistics, it is needed for flush_db_stats
}
void CellDbIn::flush_db_stats() {
  if (opts_->get_disable_rocksdb_stats()) {
    return;
  }
  if (db_busy_) {
    action_queue_.push([self = this](td::Result<td::Unit> R) mutable {
      R.ensure();
      self->flush_db_stats();
    });
    return;
  }

  auto celldb_stats = prepare_stats();
  td::StringBuilder ss;
  for (auto& [key, value] : celldb_stats) {
    ss << "ton.celldb." << key << " " << value << "\n";
  }

  auto stats =
      td::RocksDb::statistics_to_string(statistics_) + snapshot_statistics_->to_string() + ss.as_cslice().str();
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
  if (db_busy_) {
    action_queue_.push([self = this, handle = std::move(handle)](td::Result<td::Unit> R) mutable {
      R.ensure();
      self->gc_cont2(handle);
    });
    return;
  }

  td::PerfWarningTimer timer{"gccell", 0.1};
  td::PerfWarningTimer timer_all{"gccell_all", 0.05};

  td::PerfWarningTimer timer_get_keys{"gccell_get_keys", 0.05};
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
  timer_get_keys.reset();

  td::PerfWarningTimer timer_boc{"gccell_boc", 0.05};
  auto r_cell = boc_->load_cell(F.root_hash.as_slice());
  td::Ref<vm::Cell> cell;
  if (r_cell.is_ok()) {
    cell = r_cell.move_as_ok();
    boc_->dec(cell);
  }

  db_busy_ = true;
  boc_->prepare_commit_async(
      async_executor, [this, SelfId = actor_id(this), timer_boc = std::move(timer_boc), F = std::move(F), key_hash,
                       P = std::move(P), N = std::move(N), cell = std::move(cell), timer = std::move(timer),
                       timer_all = std::move(timer_all), handle](td::Result<td::Unit> R) mutable {
        R.ensure();
        td::actor::send_lambda(SelfId, [this, timer_boc = std::move(timer_boc), F = std::move(F), key_hash,
                                        P = std::move(P), N = std::move(N), cell = std::move(cell),
                                        timer = std::move(timer), timer_all = std::move(timer_all), handle]() mutable {
          TD_PERF_COUNTER(celldb_gc_cell);
          vm::CellStorer stor{*cell_db_};
          timer_boc.reset();

          td::PerfWarningTimer timer_write_batch{"gccell_write_batch", 0.05};
          cell_db_->begin_write_batch().ensure();

          boc_->meta_erase(get_key(key_hash)).ensure();
          set_block(F.prev, std::move(P));
          set_block(F.next, std::move(N));
          if (handle->id().is_masterchain()) {
            last_deleted_mc_state_ = handle->id().seqno();
            std::string key = "stats.last_deleted_mc_seqno", value = td::to_string(last_deleted_mc_state_);
            boc_->meta_set(td::as_slice(key), td::as_slice(value));
          }

          boc_->commit(stor).ensure();
          cell_db_->commit_write_batch().ensure();

          alarm_timestamp() = td::Timestamp::now();
          timer_write_batch.reset();

          td::PerfWarningTimer timer_free_cells{"gccell_free_cells", 0.05};
          auto before = td::ref_get_delete_count();
          cell = {};
          auto after = td::ref_get_delete_count();
          if (timer_free_cells.elapsed() > 0.04) {
            LOG(ERROR) << "deleted " << after - before << " cells";
          }
          timer_free_cells.reset();

          td::PerfWarningTimer timer_finish{"gccell_finish", 0.05};
          if (!opts_->get_celldb_in_memory()) {
            boc_->set_loader(std::make_unique<vm::CellLoader>(cell_db_->snapshot(), on_load_callback_)).ensure();
            td::actor::send_closure(parent_, &CellDb::update_snapshot, cell_db_->snapshot());
          }

          DCHECK(get_block(key_hash).is_error());
          if (!opts_->get_disable_rocksdb_stats()) {
            cell_db_statistics_.gc_cell_time_.insert(timer.elapsed() * 1e6);
          }
          LOG(DEBUG) << "Deleted state " << handle->id().to_str();
          timer_finish.reset();
          timer_all.reset();
          release_db();
        });
      });
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
  auto R = boc_->meta_get(td::as_slice(key), value);
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
  boc_->meta_set(td::as_slice(key), e.release());
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
  if (db_busy_) {
    action_queue_.push([self = this](td::Result<td::Unit> R) mutable {
      R.ensure();
      self->migrate_cells();
    });
    return;
  }
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
  for (auto it = cells_to_migrate_.begin(); it != cells_to_migrate_.end() && checked < 128;) {
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

void CellDb::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  promise.set_value(decltype(prepared_stats_)(prepared_stats_));
}

void CellDb::update_stats(td::Result<std::vector<std::pair<std::string, std::string>>> r_stats) {
  if (r_stats.is_error()) {
    LOG(ERROR) << "error updating stats: " << r_stats.error();
  } else {
    prepared_stats_ = r_stats.move_as_ok();
  }
  alarm_timestamp() = td::Timestamp::in(2.0);
}

void CellDb::alarm() {
  send_closure(cell_db_, &CellDbIn::prepare_stats, td::promise_send_closure(actor_id(this), &CellDb::update_stats));
}

void CellDb::load_cell(RootHash hash, td::Promise<td::Ref<vm::DataCell>> promise) {
  if (thread_safe_boc_) {
    auto result = thread_safe_boc_->load_root_thread_safe(hash.as_slice());
    if (result.is_ok()) {
      return async_apply("load_cell_result", std::move(promise), std::move(result));
    } else {
      send_closure(cell_db_, &CellDbIn::load_cell, hash, std::move(promise));
      return;
    }
  }
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

std::vector<std::pair<std::string, std::string>> CellDbIn::CellDbStatistics::prepare_stats() {
  std::vector<std::pair<std::string, std::string>> stats;
  stats.emplace_back("store_cell.micros", PSTRING() << store_cell_time_.to_string());
  stats.emplace_back("store_cell.prepare.micros", PSTRING() << store_cell_prepare_time_.to_string());
  stats.emplace_back("store_cell.write.micros", PSTRING() << store_cell_write_time_.to_string());
  stats.emplace_back("gc_cell.micros", PSTRING() << gc_cell_time_.to_string());
  stats.emplace_back("total_time.micros", PSTRING() << (td::Timestamp::now().at() - stats_start_time_.at()) * 1e6);
  stats.emplace_back("in_memory", PSTRING() << bool(in_memory_load_time_));
  if (in_memory_load_time_) {
    stats.emplace_back("in_memory_load_time", PSTRING() << in_memory_load_time_.value());
  }
  if (boc_stats_) {
    stats.emplace_back("cells_count", PSTRING() << boc_stats_->cells_total_count);
    stats.emplace_back("cells_size", PSTRING() << boc_stats_->cells_total_size);
    stats.emplace_back("roots_count", PSTRING() << boc_stats_->roots_total_count);
    for (auto& [key, value] : boc_stats_->custom_stats) {
      stats.emplace_back(key, value);
    }

    for (auto& [key, value] : boc_stats_->named_stats.stats_str) {
      stats.emplace_back(key, value);
    }
    for (auto& [key, value] : boc_stats_->named_stats.stats_int) {
      stats.emplace_back(key, td::to_string(value));
    }
  }
  return stats;
}

}  // namespace validator

}  // namespace ton
