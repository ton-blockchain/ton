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
#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/db/CellStorage.h"
#include "vm/db/CellHashTable.h"

#include "vm/cells/ExtCell.h"

#include "td/utils/base64.h"
#include "td/utils/format.h"
#include "td/utils/ThreadSafeCounter.h"

#include "vm/cellslice.h"
#include <queue>
#include "td/actor/actor.h"
#include "common/delay.h"

namespace vm {
namespace {

struct DynamicBocExtCellExtra {
  std::shared_ptr<CellDbReader> reader;
};

class DynamicBocCellLoader {
 public:
  static td::Result<Ref<DataCell>> load_data_cell(const Cell &cell, const DynamicBocExtCellExtra &extra) {
    return extra.reader->load_cell(cell.get_hash().as_slice());
  }
};

using DynamicBocExtCell = ExtCell<DynamicBocExtCellExtra, DynamicBocCellLoader>;

struct CellInfo {
  bool sync_with_db{false};
  bool in_db{false};

  bool was_dfs_new_cells{false};
  bool was{false};

  td::int32 db_refcnt{0};
  td::int32 refcnt_diff{0};
  Ref<Cell> cell;
  Cell::Hash key() const {
    return cell->get_hash();
  }
  bool operator<(const CellInfo &other) const {
    return key() < other.key();
  }

  struct Eq {
    using is_transparent = void;  // Pred to use
    bool operator()(const CellInfo &info, const CellInfo &other_info) const { return info.key() == other_info.key();}
    bool operator()(const CellInfo &info, td::Slice hash) const { return info.key().as_slice() == hash;}
    bool operator()(td::Slice hash, const CellInfo &info) const { return info.key().as_slice() == hash;}

  };
  struct Hash {
    using is_transparent = void;  // Pred to use
    using transparent_key_equal = Eq;
    size_t operator()(td::Slice hash) const { return cell_hash_slice_hash(hash); }
    size_t operator()(const CellInfo &info) const { return cell_hash_slice_hash(info.key().as_slice());}
  };
};

bool operator<(const CellInfo &a, td::Slice b) {
  return a.key().as_slice() < b;
}

bool operator<(td::Slice a, const CellInfo &b) {
  return a < b.key().as_slice();
}

class DynamicBagOfCellsDbImpl : public DynamicBagOfCellsDb, private ExtCellCreator {
 public:
  DynamicBagOfCellsDbImpl() {
    get_thread_safe_counter().add(1);
  }
  ~DynamicBagOfCellsDbImpl() {
    get_thread_safe_counter().add(-1);
    reset_cell_db_reader();
  }
  td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
    return get_cell_info_lazy(level_mask, hash, depth).cell;
  }
  td::Result<Ref<DataCell>> load_cell(td::Slice hash) override {
    TRY_RESULT(loaded_cell, get_cell_info_force(hash).cell->load_cell());
    return std::move(loaded_cell.data_cell);
  }
  td::Result<Ref<DataCell>> load_root(td::Slice hash) override {
    return load_cell(hash);
  }
  td::Result<Ref<DataCell>> load_root_thread_safe(td::Slice hash) const override {
    return td::Status::Error("Not implemented");
  }
  void load_cell_async(td::Slice hash, std::shared_ptr<AsyncExecutor> executor,
                       td::Promise<Ref<DataCell>> promise) override {
    auto promise_ptr = std::make_shared<td::Promise<Ref<DataCell>>>(std::move(promise));
    auto info = hash_table_.get_if_exists(hash);
    if (info && info->sync_with_db) {
      executor->execute_async([promise = std::move(promise_ptr), cell = info->cell]() mutable {
        TRY_RESULT_PROMISE((*promise), loaded_cell, cell->load_cell());
        promise->set_result(loaded_cell.data_cell);
      });
      return;
    }
    SimpleExtCellCreator ext_cell_creator(cell_db_reader_);
    executor->execute_async(
        [executor, loader = *loader_, hash = CellHash::from_slice(hash), db = this,
         ext_cell_creator = std::move(ext_cell_creator), promise = std::move(promise_ptr)]() mutable {
          TRY_RESULT_PROMISE((*promise), res, loader.load(hash.as_slice(), true, ext_cell_creator));
          if (res.status != CellLoader::LoadResult::Ok) {
            promise->set_error(td::Status::Error("cell not found"));
            return;
          }
          Ref<Cell> cell = res.cell();
          executor->execute_sync([hash, db, res = std::move(res),
                                  ext_cell_creator = std::move(ext_cell_creator)]() mutable {
            db->hash_table_.apply(hash.as_slice(), [&](CellInfo &info) {
              db->update_cell_info_loaded(info, hash.as_slice(), std::move(res));
            });
            for (auto &ext_cell : ext_cell_creator.get_created_cells()) {
              auto ext_cell_hash = ext_cell->get_hash();
              db->hash_table_.apply(ext_cell_hash.as_slice(), [&](CellInfo &info) {
                db->update_cell_info_created_ext(info, std::move(ext_cell));
              });
            }
          });
          promise->set_result(std::move(cell));
        });
  }
  CellInfo &get_cell_info_force(td::Slice hash) {
    return hash_table_.apply(hash, [&](CellInfo &info) { update_cell_info_force(info, hash); });
  }
  CellInfo &get_cell_info_lazy(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) {
    return hash_table_.apply(hash.substr(hash.size() - Cell::hash_bytes),
                             [&](CellInfo &info) { update_cell_info_lazy(info, level_mask, hash, depth); });
  }
  CellInfo &get_cell_info(const Ref<Cell> &cell) {
    return hash_table_.apply(cell->get_hash().as_slice(), [&](CellInfo &info) { update_cell_info(info, cell); });
  }

  void inc(const Ref<Cell> &cell) override {
    if (cell.is_null()) {
      return;
    }
    if (cell->get_virtualization() != 0) {
      return;
    }
    to_inc_.push_back(cell);
  }
  void dec(const Ref<Cell> &cell) override {
    if (cell.is_null()) {
      return;
    }
    if (cell->get_virtualization() != 0) {
      return;
    }
    to_dec_.push_back(cell);
  }

  bool is_prepared_for_commit() {
    return to_inc_.empty() && to_dec_.empty();
  }

  Stats get_stats_diff() override {
    CHECK(is_prepared_for_commit());
    return stats_diff_;
  }

  td::Status prepare_commit() override {
    if (pca_state_) {
      return td::Status::Error("prepare_commit_async is not finished");
    }
    if (is_prepared_for_commit()) {
      return td::Status::OK();
    }
    for (auto &new_cell : to_inc_) {
      auto &new_cell_info = get_cell_info(new_cell);
      dfs_new_cells_in_db(new_cell_info);
    }
    for (auto &new_cell : to_inc_) {
      auto &new_cell_info = get_cell_info(new_cell);
      dfs_new_cells(new_cell_info);
    }

    for (auto &old_cell : to_dec_) {
      auto &old_cell_info = get_cell_info(old_cell);
      dfs_old_cells(old_cell_info);
    }

    save_diff_prepare();

    to_inc_.clear();
    to_dec_.clear();

    return td::Status::OK();
  }

  td::Status commit(CellStorer &storer) override {
    prepare_commit();
    save_diff(storer);
    // Some elements are erased from hash table, to keep it small.
    // Hash table is no longer represents the difference between the loader and
    // the current bag of cells.
    reset_cell_db_reader();
    return td::Status::OK();
  }

  std::shared_ptr<CellDbReader> get_cell_db_reader() override {
    return cell_db_reader_;
  }

  td::Status set_loader(std::unique_ptr<CellLoader> loader) override {
    reset_cell_db_reader();
    loader_ = std::move(loader);
    //cell_db_reader_ = std::make_shared<CellDbReaderImpl>(this);
    // Temporary(?) fix to make ExtCell thread safe.
    // Downside(?) - loaded cells won't be cached
    cell_db_reader_ = std::make_shared<CellDbReaderImpl>(std::make_unique<CellLoader>(*loader_));
    stats_diff_ = {};
    return td::Status::OK();
  }

  void set_celldb_compress_depth(td::uint32 value) override {
    celldb_compress_depth_ = value;
  }

  vm::ExtCellCreator& as_ext_cell_creator() override {
    return *this;
  }

 private:
  std::unique_ptr<CellLoader> loader_;
  std::vector<Ref<Cell>> to_inc_;
  std::vector<Ref<Cell>> to_dec_;
  CellHashTable<CellInfo> hash_table_;
  std::vector<CellInfo *> visited_;
  Stats stats_diff_;
  td::uint32 celldb_compress_depth_{0};

  static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
    static auto res = td::NamedThreadSafeCounter::get_default().get_counter("DynamicBagOfCellsDb");
    return res;
  }

  class SimpleExtCellCreator : public ExtCellCreator {
   public:
    explicit SimpleExtCellCreator(std::shared_ptr<CellDbReader> cell_db_reader) :
        cell_db_reader_(std::move(cell_db_reader)) {}

    td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
      TRY_RESULT(ext_cell, DynamicBocExtCell::create(PrunnedCellInfo{level_mask, hash, depth},
                                                     DynamicBocExtCellExtra{cell_db_reader_}));
      created_cells_.push_back(ext_cell);
      return std::move(ext_cell);
    }

    std::vector<Ref<Cell>>& get_created_cells() {
      return created_cells_;
    }

   private:
    std::vector<Ref<Cell>> created_cells_;
    std::shared_ptr<CellDbReader> cell_db_reader_;
  };

  class CellDbReaderImpl : public CellDbReader,
                           private ExtCellCreator,
                           public std::enable_shared_from_this<CellDbReaderImpl> {
   public:
    CellDbReaderImpl(std::unique_ptr<CellLoader> cell_loader) : db_(nullptr), cell_loader_(std::move(cell_loader)) {
      if (cell_loader_) {
        get_thread_safe_counter().add(1);
      }
    }
    CellDbReaderImpl(DynamicBagOfCellsDb *db) : db_(db) {
    }
    ~CellDbReaderImpl() {
      if (cell_loader_) {
        get_thread_safe_counter().add(-1);
      }
    }
    void set_loader(std::unique_ptr<CellLoader> cell_loader) {
      if (cell_loader_) {
        // avoid race
        return;
      }
      cell_loader_ = std::move(cell_loader);
      db_ = nullptr;
      if (cell_loader_) {
        get_thread_safe_counter().add(1);
      }
    }

    td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
      CHECK(!db_);
      TRY_RESULT(ext_cell, DynamicBocExtCell::create(PrunnedCellInfo{level_mask, hash, depth},
                                                     DynamicBocExtCellExtra{shared_from_this()}));
      return std::move(ext_cell);
    }

    td::Result<Ref<DataCell>> load_cell(td::Slice hash) override {
      if (db_) {
        return db_->load_cell(hash);
      }
      TRY_RESULT(load_result, cell_loader_->load(hash, true, *this));
      if (load_result.status != CellLoader::LoadResult::Ok) {
        return td::Status::Error("cell not found");
      }
      return std::move(load_result.cell());
    }

   private:
    static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
      static auto res = td::NamedThreadSafeCounter::get_default().get_counter("DynamicBagOfCellsDbLoader");
      return res;
    }
    DynamicBagOfCellsDb *db_;
    std::unique_ptr<CellLoader> cell_loader_;
  };

  std::shared_ptr<CellDbReaderImpl> cell_db_reader_;

  void reset_cell_db_reader() {
    if (!cell_db_reader_) {
      return;
    }
    cell_db_reader_->set_loader(std::move(loader_));
    cell_db_reader_.reset();
    //EXPERIMENTAL: clear cache to drop all references to old reader.
    hash_table_ = {};
  }

  bool is_in_db(CellInfo &info) {
    if (info.in_db) {
      return true;
    }
    load_cell(info);
    return info.in_db;
  }
  bool is_loaded(CellInfo &info) {
    return info.sync_with_db;
  }

  void load_cell(CellInfo &info) {
    if (is_loaded(info)) {
      return;
    }
    do_load_cell(info);
  }

  bool dfs_new_cells_in_db(CellInfo &info) {
    if (info.sync_with_db) {
      return is_in_db(info);
    }
    if (info.in_db) {
      return true;
    }

    bool not_in_db = false;
    for_each(
        info, [&not_in_db, this](auto &child_info) { not_in_db |= !dfs_new_cells_in_db(child_info); }, false);

    if (not_in_db) {
      CHECK(!info.in_db);
      info.sync_with_db = true;
    }
    return is_in_db(info);
  }

  void dfs_new_cells(CellInfo &info) {
    info.refcnt_diff++;
    if (!info.was) {
      info.was = true;
      visited_.push_back(&info);
    }

    if (info.was_dfs_new_cells) {
      return;
    }
    info.was_dfs_new_cells = true;

    if (is_in_db(info)) {
      return;
    }

    CHECK(is_loaded(info));
    for_each(info, [this](auto &child_info) { dfs_new_cells(child_info); });
  }

  void dfs_old_cells(CellInfo &info) {
    info.refcnt_diff--;
    if (!info.was) {
      info.was = true;
      visited_.push_back(&info);
    }

    load_cell(info);

    auto new_refcnt = info.refcnt_diff + info.db_refcnt;
    CHECK(new_refcnt >= 0);
    if (new_refcnt != 0) {
      return;
    }

    for_each(info, [this](auto &child_info) { dfs_old_cells(child_info); });
  }

  void save_diff_prepare() {
    stats_diff_ = {};
    for (auto info_ptr : visited_) {
      save_cell_prepare(*info_ptr);
    }
  }

  void save_diff(CellStorer &storer) {
    for (auto info_ptr : visited_) {
      save_cell(*info_ptr, storer);
    }
    visited_.clear();
  }

  void save_cell_prepare(CellInfo &info) {
    if (info.refcnt_diff == 0) {
      return;
    }
    load_cell(info);

    auto loaded_cell = info.cell->load_cell().move_as_ok();
    if (info.db_refcnt + info.refcnt_diff == 0) {
      CHECK(info.in_db);
      // erase
      stats_diff_.cells_total_count--;
      stats_diff_.cells_total_size -= loaded_cell.data_cell->get_serialized_size(true);
    } else {
      //save
      if (info.in_db == false) {
        stats_diff_.cells_total_count++;
        stats_diff_.cells_total_size += loaded_cell.data_cell->get_serialized_size(true);
      }
    }
  }

  void save_cell(CellInfo &info, CellStorer &storer) {
    auto guard = td::ScopeExit{} + [&] {
      info.was_dfs_new_cells = false;
      info.was = false;
    };
    if (info.refcnt_diff == 0) {
      //CellSlice(info.cell, nullptr).print_rec(std::cout);
      return;
    }
    CHECK(info.sync_with_db);

    info.db_refcnt += info.refcnt_diff;
    info.refcnt_diff = 0;

    if (info.db_refcnt == 0) {
      CHECK(info.in_db);
      storer.erase(info.cell->get_hash().as_slice());
      info.in_db = false;
      hash_table_.erase(info.cell->get_hash().as_slice());
      guard.dismiss();
    } else {
      auto loaded_cell = info.cell->load_cell().move_as_ok();
      storer.set(info.db_refcnt, loaded_cell.data_cell,
                 loaded_cell.data_cell->get_depth() == celldb_compress_depth_ && celldb_compress_depth_ != 0);
      info.in_db = true;
    }
  }

  template <class F>
  void for_each(CellInfo &info, F &&f, bool force = true) {
    auto cell = info.cell;

    if (!cell->is_loaded()) {
      if (!force) {
        return;
      }
      load_cell(info);
      cell = info.cell;
    }
    if (!cell->is_loaded()) {
      cell->load_cell().ensure();
    }
    CHECK(cell->is_loaded());
    vm::CellSlice cs(vm::NoVm{}, cell);  // FIXME
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      f(get_cell_info(cs.prefetch_ref(i)));
    }
  }

  void do_load_cell(CellInfo &info) {
    update_cell_info_force(info, info.cell->get_hash().as_slice());
  }

  void update_cell_info(CellInfo &info, const Ref<Cell> &cell) {
    CHECK(!cell.is_null());
    if (info.sync_with_db) {
      return;
    }
    info.cell = cell;
  }

  void update_cell_info_lazy(CellInfo &info, Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) {
    if (info.sync_with_db) {
      CHECK(info.cell.not_null());
      CHECK(info.cell->get_level_mask() == level_mask);
      return;
    }
    if (info.cell.is_null()) {
      auto ext_cell_r = create_empty_ext_cell(level_mask, hash, depth);
      if (ext_cell_r.is_error()) {
        //FIXME
        LOG(ERROR) << "Failed to create ext_cell" << ext_cell_r.error();
        return;
      }
      info.cell = ext_cell_r.move_as_ok();
      info.in_db = true;  // TODO
    }
  }
  void update_cell_info_force(CellInfo &info, td::Slice hash) {
    if (info.sync_with_db) {
      return;
    }

    do {
      CHECK(loader_);
      auto r_res = loader_->load(hash, true, *this);
      if (r_res.is_error()) {
        //FIXME
        LOG(ERROR) << "Failed to load cell from db" << r_res.error();
        break;
      }
      auto res = r_res.move_as_ok();
      if (res.status != CellLoader::LoadResult::Ok) {
        break;
      }
      info.cell = std::move(res.cell());
      CHECK(info.cell->get_hash().as_slice() == hash);
      info.in_db = true;
      info.db_refcnt = res.refcnt();
    } while (false);
    info.sync_with_db = true;
  }

  // same as update_cell_info_force, but with cell provided by a caller
  void update_cell_info_loaded(CellInfo &info, td::Slice hash, CellLoader::LoadResult res) {
    if (info.sync_with_db) {
      return;
    }
    DCHECK(res.status == CellLoader::LoadResult::Ok);
    info.cell = std::move(res.cell());
    CHECK(info.cell->get_hash().as_slice() == hash);
    info.in_db = true;
    info.db_refcnt = res.refcnt();
    info.sync_with_db = true;
  }

  // same as update_cell_info_lazy, but with cell provided by a caller
  void update_cell_info_created_ext(CellInfo &info, Ref<Cell> cell) {
    if (info.sync_with_db) {
      CHECK(info.cell.not_null());
      CHECK(info.cell->get_level_mask() == cell->get_level_mask());
      CHECK(info.cell->get_hash() == cell->get_hash());
      return;
    }
    if (info.cell.is_null()) {
      info.cell = std::move(cell);
      info.in_db = true;
    }
  }

  td::Result<Ref<Cell>> create_empty_ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) {
    TRY_RESULT(res, DynamicBocExtCell::create(PrunnedCellInfo{level_mask, hash, depth},
                                              DynamicBocExtCellExtra{cell_db_reader_}));
    return std::move(res);
  }

  struct PrepareCommitAsyncState {
    size_t remaining_ = 0;
    std::shared_ptr<AsyncExecutor> executor_;
    td::Promise<td::Unit> promise_;

    struct CellInfo2 {
      CellInfo *info{};
      std::vector<CellInfo2 *> parents;
      unsigned remaining_children = 0;
      Cell::Hash key() const {
        return info->key();
      }
      bool operator<(const CellInfo2 &other) const {
        return key() < other.key();
      }

      friend bool operator<(const CellInfo2 &a, td::Slice b) {
        return a.key().as_slice() < b;
      }

      friend bool operator<(td::Slice a, const CellInfo2 &b) {
        return a < b.key().as_slice();
      }

      struct Eq {
        using is_transparent = void;  // Pred to use
        bool operator()(const CellInfo2 &info, const CellInfo2 &other_info) const {
          return info.key() == other_info.key();
        }
        bool operator()(const CellInfo2 &info, td::Slice hash) const {
          return info.key().as_slice() == hash;
        }
        bool operator()(td::Slice hash, const CellInfo2 &info) const {
          return info.key().as_slice() == hash;
        }
      };
      struct Hash {
        using is_transparent = void;  // Pred to use
        using transparent_key_equal = Eq;
        size_t operator()(td::Slice hash) const {
          return cell_hash_slice_hash(hash);
        }
        size_t operator()(const CellInfo2 &info) const {
          return cell_hash_slice_hash(info.key().as_slice());
        }
      };
    };

    CellHashTable<CellInfo2> cells_;

    std::queue<CellInfo2*> load_queue_;
    td::uint32 active_load_ = 0;
    td::uint32 max_parallel_load_ = 4;
  };
  std::unique_ptr<PrepareCommitAsyncState> pca_state_;

  void prepare_commit_async(std::shared_ptr<AsyncExecutor> executor, td::Promise<td::Unit> promise) override {
    hash_table_ = {};
    if (pca_state_) {
      promise.set_error(td::Status::Error("Other prepare_commit_async is not finished"));
      return;
    }
    if (is_prepared_for_commit()) {
      promise.set_result(td::Unit());
      return;
    }
    pca_state_ = std::make_unique<PrepareCommitAsyncState>();
    pca_state_->executor_ = std::move(executor);
    pca_state_->promise_ = std::move(promise);
    for (auto &new_cell : to_inc_) {
      dfs_new_cells_in_db_async(new_cell);
    }
    pca_state_->cells_.for_each([&](PrepareCommitAsyncState::CellInfo2 &info) {
      ++pca_state_->remaining_;
      if (info.remaining_children == 0) {
        pca_load_from_db(&info);
      }
    });
    if (pca_state_->remaining_ == 0) {
      prepare_commit_async_cont();
    }
  }

  void dfs_new_cells_in_db_async(const td::Ref<vm::Cell> &cell, PrepareCommitAsyncState::CellInfo2 *parent = nullptr) {
    bool exists = true;
    pca_state_->cells_.apply(cell->get_hash().as_slice(), [&](PrepareCommitAsyncState::CellInfo2 &info) {
      if (info.info == nullptr) {
        exists = false;
        info.info = &get_cell_info(cell);
      }
    });
    auto info = pca_state_->cells_.get_if_exists(cell->get_hash().as_slice());
    if (parent) {
      info->parents.push_back(parent);
      ++parent->remaining_children;
    }
    if (exists) {
      return;
    }
    if (cell->is_loaded()) {
      vm::CellSlice cs(vm::NoVm{}, cell);
      for (unsigned i = 0; i < cs.size_refs(); i++) {
        dfs_new_cells_in_db_async(cs.prefetch_ref(i), info);
      }
    }
  }

  void pca_load_from_db(PrepareCommitAsyncState::CellInfo2 *info) {
    if (pca_state_->active_load_ >= pca_state_->max_parallel_load_) {
      pca_state_->load_queue_.push(info);
      return;
    }
    ++pca_state_->active_load_;
    pca_state_->executor_->execute_async(
        [db = this, info, executor = pca_state_->executor_, loader = *loader_]() mutable {
          auto res = loader.load_refcnt(info->info->cell->get_hash().as_slice()).move_as_ok();
          executor->execute_sync([db, info, res = std::move(res)]() {
            --db->pca_state_->active_load_;
            db->pca_process_load_queue();
            db->pca_set_in_db(info, std::move(res));
          });
        });
  }

  void pca_process_load_queue() {
    while (pca_state_->active_load_ < pca_state_->max_parallel_load_ && !pca_state_->load_queue_.empty()) {
      PrepareCommitAsyncState::CellInfo2 *info = pca_state_->load_queue_.front();
      pca_state_->load_queue_.pop();
      pca_load_from_db(info);
    }
  }

  void pca_set_in_db(PrepareCommitAsyncState::CellInfo2 *info, CellLoader::LoadResult result) {
    info->info->sync_with_db = true;
    if (result.status == CellLoader::LoadResult::Ok) {
      info->info->in_db = true;
      info->info->db_refcnt = result.refcnt();
    } else {
      info->info->in_db = false;
    }
    for (PrepareCommitAsyncState::CellInfo2 *parent_info : info->parents) {
      if (parent_info->info->sync_with_db) {
        continue;
      }
      if (!info->info->in_db) {
        pca_set_in_db(parent_info, {});
      } else if (--parent_info->remaining_children == 0) {
        pca_load_from_db(parent_info);
      }
    }
    CHECK(pca_state_->remaining_ != 0);
    if (--pca_state_->remaining_ == 0) {
      prepare_commit_async_cont();
    }
  }

  void prepare_commit_async_cont() {
    for (auto &new_cell : to_inc_) {
      auto &new_cell_info = get_cell_info(new_cell);
      dfs_new_cells(new_cell_info);
    }

    CHECK(pca_state_->remaining_ == 0);
    for (auto &old_cell : to_dec_) {
      auto &old_cell_info = get_cell_info(old_cell);
      dfs_old_cells_async(old_cell_info);
    }
    if (pca_state_->remaining_ == 0) {
      prepare_commit_async_cont2();
    }
  }

  void dfs_old_cells_async(CellInfo &info) {
    if (!info.was) {
      info.was = true;
      visited_.push_back(&info);
      if (!info.sync_with_db) {
        ++pca_state_->remaining_;
        load_cell_async(
            info.cell->get_hash().as_slice(), pca_state_->executor_,
            [executor = pca_state_->executor_, db = this, info = &info](td::Result<td::Ref<vm::DataCell>> R) {
              R.ensure();
              executor->execute_sync([db, info]() {
                CHECK(info->sync_with_db);
                db->dfs_old_cells_async(*info);
                if (--db->pca_state_->remaining_ == 0) {
                  db->prepare_commit_async_cont2();
                }
              });
            });
        return;
      }
    }
    info.refcnt_diff--;
    if (!info.sync_with_db) {
      return;
    }
    auto new_refcnt = info.refcnt_diff + info.db_refcnt;
    CHECK(new_refcnt >= 0);
    if (new_refcnt != 0) {
      return;
    }

    for_each(info, [this](auto &child_info) { dfs_old_cells_async(child_info); });
  }

  void prepare_commit_async_cont2() {
    save_diff_prepare();
    to_inc_.clear();
    to_dec_.clear();
    pca_state_->promise_.set_result(td::Unit());
    pca_state_ = {};
  }

};
}  // namespace

std::unique_ptr<DynamicBagOfCellsDb> DynamicBagOfCellsDb::create() {
  return std::make_unique<DynamicBagOfCellsDbImpl>();
}
}  // namespace vm
