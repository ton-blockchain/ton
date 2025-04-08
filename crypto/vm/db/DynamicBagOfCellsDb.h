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
#include "td/db/KeyValue.h"
#include "vm/cells.h"

#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/actor/PromiseFuture.h"

#include <thread>

namespace td {
class KeyValueReader;
}
namespace vm {
class CellLoader;
class CellStorer;
}  // namespace vm

namespace vm {
class ExtCellCreator {
 public:
  virtual ~ExtCellCreator() = default;
  virtual td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) = 0;
};

class CellDbReader {
 public:
  virtual ~CellDbReader() = default;
  virtual td::Result<Ref<DataCell>> load_cell(td::Slice hash) = 0;
  virtual td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<td::Slice> hashes) = 0;
};

class DynamicBagOfCellsDb {
 public:
  virtual ~DynamicBagOfCellsDb() = default;

  virtual td::Result<std::vector<std::pair<std::string, std::string>>> meta_get_all(size_t max_count) const = 0;
  virtual td::Result<td::KeyValue::GetStatus> meta_get(td::Slice key, std::string &value) = 0;
  virtual td::Status meta_set(td::Slice key, td::Slice value) = 0;
  virtual td::Status meta_erase(td::Slice key) = 0;

  virtual td::Result<Ref<DataCell>> load_cell(td::Slice hash) = 0;
  virtual td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<td::Slice> hashes) = 0;
  virtual td::Result<Ref<DataCell>> load_root(td::Slice hash) = 0;
  virtual td::Result<Ref<DataCell>> load_root_thread_safe(td::Slice hash) const = 0;
  virtual td::Result<std::vector<Ref<DataCell>>> load_known_roots() const {
    return std::vector<Ref<DataCell>>();
  }
  struct Stats {
    td::int64 roots_total_count{0};
    td::int64 cells_total_count{0};
    td::int64 cells_total_size{0};
    td::NamedStats named_stats;
    std::vector<std::pair<std::string, std::string>> custom_stats;
    void apply_diff(const Stats &diff) {
      roots_total_count += diff.roots_total_count;
      cells_total_count += diff.cells_total_count;
      cells_total_size += diff.cells_total_size;
      CHECK(roots_total_count >= 0);
      CHECK(cells_total_count >= 0);
      CHECK(cells_total_size >= 0);
      named_stats.apply_diff(diff.named_stats);
    }
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const Stats &stats) {
      sb << "STATS\n";
      for (auto &p : stats.custom_stats) {
        sb << "\t" << p.first << "\t" << p.second << "\n";
      }
      for (auto &p : stats.named_stats.stats_int) {
        sb << "\t" << p.first << "\t" << p.second << "\n";
      }
      for (auto &p : stats.named_stats.stats_str) {
        sb << "\t" << p.first << "\t" << p.second << "\n";
      }
      return sb;
    }
  };
  virtual void inc(const Ref<Cell> &old_root) = 0;
  virtual void dec(const Ref<Cell> &old_root) = 0;

  virtual td::Status prepare_commit() = 0;
  virtual Stats get_stats_diff() = 0;
  virtual td::Result<Stats> get_stats() {
    return Stats{};
  }
  virtual td::Status commit(CellStorer &) = 0;
  virtual std::shared_ptr<CellDbReader> get_cell_db_reader() = 0;

  // restart with new loader will also reset stats_diff
  virtual td::Status set_loader(std::unique_ptr<CellLoader> loader) = 0;

  virtual void set_celldb_compress_depth(td::uint32 value) = 0;
  virtual vm::ExtCellCreator &as_ext_cell_creator() = 0;

  class AsyncExecutor {
   public:
    virtual ~AsyncExecutor() {
    }
    virtual void execute_async(std::function<void()> f) = 0;
    virtual void execute_sync(std::function<void()> f) = 0;
    virtual std::string describe() const {
      return "AsyncExecutor";
    }
  };

  struct CreateV1Options {
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const CreateV1Options &options) {
      return sb << "V1{}";
    }
  };
  static std::unique_ptr<DynamicBagOfCellsDb> create(CreateV1Options = {});

  struct CreateV2Options {
    size_t extra_threads{std::thread::hardware_concurrency()};
    std::shared_ptr<AsyncExecutor> executor{};
    size_t cache_ttl_max{2000};
    size_t cache_size_max{1000000};
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const CreateV2Options &options) {
      return sb << "V2{extra_threads=" << options.extra_threads << ", cache_ttl_max=" << options.cache_ttl_max
                << ", cache_size_max=" << options.cache_size_max << "}";
    }
  };
  static std::unique_ptr<DynamicBagOfCellsDb> create_v2(CreateV2Options options);

  struct CreateInMemoryOptions {
    size_t extra_threads{std::thread::hardware_concurrency()};
    bool verbose{true};
    // Allocated DataCells will never be deleted
    bool use_arena{false};
    // Almost no overhead in memory during creation, but will scan database twice
    bool use_less_memory_during_creation{true};
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const CreateInMemoryOptions &options) {
      return sb << "InMemory{extra_threads=" << options.extra_threads << ", use_arena=" << options.use_arena
                << ", use_less_memory_during_creation=" << options.use_less_memory_during_creation << "}";
    }
  };
  static std::unique_ptr<DynamicBagOfCellsDb> create_in_memory(td::KeyValueReader *kv, CreateInMemoryOptions options);

  virtual void load_cell_async(td::Slice hash, std::shared_ptr<AsyncExecutor> executor,
                               td::Promise<Ref<DataCell>> promise) = 0;
  virtual void prepare_commit_async(std::shared_ptr<AsyncExecutor> executor, td::Promise<td::Unit> promise) = 0;
};

}  // namespace vm
