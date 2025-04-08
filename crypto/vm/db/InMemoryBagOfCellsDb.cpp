#include "CellStorage.h"
#include "DynamicBagOfCellsDb.h"
#include "td/utils/Timer.h"
#include "td/utils/base64.h"
#include "td/utils/format.h"
#include "td/utils/int_types.h"
#include "td/utils/misc.h"
#include "td/utils/port/Stat.h"
#include "vm/cells/CellHash.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/DataCell.h"
#include "vm/cells/ExtCell.h"

#include "td/utils/HashMap.h"
#include "td/utils/HashSet.h"

#include <optional>

#if TD_PORT_POSIX
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace vm {
namespace {
constexpr bool use_dense_hash_map = true;

template <class F>
void parallel_run(size_t n, F &&run_task, size_t extra_threads_n) {
  std::atomic<size_t> next_task_id{0};
  auto loop = [&] {
    while (true) {
      auto task_id = next_task_id++;
      if (task_id >= n) {
        break;
      }
      run_task(task_id);
    }
  };

  // NB: it could be important that td::thread is used, not std::thread
  std::vector<td::thread> threads;
  for (size_t i = 0; i < extra_threads_n; i++) {
    threads.emplace_back(loop);
  }
  loop();
  for (auto &thread : threads) {
    thread.join();
  }
  threads.clear();
}

struct UniqueAccess {
  struct Release {
    void operator()(UniqueAccess *access) const {
      if (access) {
        access->release();
      }
    }
  };
  using Lock = std::unique_ptr<UniqueAccess, Release>;
  Lock lock() {
    CHECK(!locked_.exchange(true));
    return Lock(this);
  }

 private:
  std::atomic<bool> locked_{false};
  void release() {
    locked_ = false;
  }
};
class DefaultPrunnedCellCreator : public ExtCellCreator {
 public:
  td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
    TRY_RESULT(cell, PrunnedCell<td::Unit>::create(PrunnedCellInfo{level_mask, hash, depth}, td::Unit{}));
    return cell;
  }
};

class ArenaPrunnedCellCreator : public ExtCellCreator {
  struct ArenaAllocator {
    ArenaAllocator() {
      // only one instance ever
      static UniqueAccess unique_access;
      [[maybe_unused]] auto ptr = unique_access.lock().release();
    }
    std::mutex mutex;
    struct Deleter {
      static constexpr size_t batch_size = 1 << 24;
#if TD_PORT_POSIX
      static std::unique_ptr<char, Deleter> alloc() {
        char *ptr = reinterpret_cast<char *>(
            mmap(NULL, batch_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        CHECK(ptr != nullptr);
        return std::unique_ptr<char, Deleter>(ptr);
      }
      void operator()(char *ptr) const {
        munmap(ptr, batch_size);
      }
#else
      static std::unique_ptr<char, Deleter> alloc() {
        auto ptr = reinterpret_cast<char *>(malloc(batch_size));
        CHECK(ptr != nullptr);
        return std::unique_ptr<char, Deleter>(ptr);
      }
      void operator()(char *ptr) const {
        free(ptr);
      }
#endif
    };
    std::vector<std::unique_ptr<char, Deleter>> arena;
    td::uint64 arena_generation{0};

    td::MutableSlice alloc_batch() {
      auto batch = Deleter::alloc();
      auto res = td::MutableSlice(batch.get(), Deleter::batch_size);
      std::lock_guard<std::mutex> guard(mutex);
      arena.emplace_back(std::move(batch));
      return res;
    }

    char *alloc(size_t size) {
      thread_local td::MutableSlice batch;
      thread_local td::uint64 batch_generation{0};
      auto aligned_size = (size + 7) / 8 * 8;
      if (batch.size() < size || batch_generation != arena_generation) {
        batch = alloc_batch();
        batch_generation = arena_generation;
      }
      auto res = batch.begin();
      batch.remove_prefix(aligned_size);
      return res;
    }
    void clear() {
      std::lock_guard<std::mutex> guard(mutex);
      arena_generation++;
      td::reset_to_empty(arena);
    }
  };
  static ArenaAllocator arena_;
  static td::ThreadSafeCounter cells_count_;

 public:
  struct Counter {
    Counter() {
      cells_count_.add(1);
    }
    Counter(Counter &&other) {
      cells_count_.add(1);
    }
    Counter(const Counter &other) {
      cells_count_.add(1);
    }
    ~Counter() {
      cells_count_.add(-1);
    }
  };

  td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
    TRY_RESULT(cell, PrunnedCell<Counter>::create([&](size_t bytes) { return arena_.alloc(bytes); }, false,
                                                  PrunnedCellInfo{level_mask, hash, depth}, Counter()));
    return cell;
  }
  static td::int64 count() {
    return cells_count_.sum();
  }
  static void clear_arena() {
    LOG_CHECK(cells_count_.sum() == 0) << cells_count_.sum();
    arena_.clear();
  }
};
td::ThreadSafeCounter ArenaPrunnedCellCreator::cells_count_;
ArenaPrunnedCellCreator::ArenaAllocator ArenaPrunnedCellCreator::arena_;

struct CellInfo {
  mutable td::int32 db_refcnt{0};
  Ref<DataCell> cell;
};
static_assert(sizeof(CellInfo) == 16);

CellHash as_cell_hash(const CellInfo &info) {
  return info.cell->get_hash();
}

struct CellInfoHashTableBaseline {
  td::HashSet<CellInfo, CellHashF, CellEqF> ht_;
  const CellInfo *find(CellHash hash) const {
    if (auto it = ht_.find(hash); it != ht_.end()) {
      return &*it;
    }
    return nullptr;
  }
  void erase(CellHash hash) {
    auto it = ht_.find(hash);
    CHECK(it != ht_.end());
    ht_.erase(it);
  }
  void insert(CellInfo info) {
    ht_.insert(std::move(info));
  }
  template <class Iterator>
  void init_from(Iterator begin, Iterator end) {
    ht_ = td::HashSet<CellInfo, CellHashF, CellEqF>(begin, end);
  }
  size_t size() const {
    return ht_.size();
  }
  auto begin() const {
    return ht_.begin();
  }
  auto end() const {
    return ht_.end();
  }
  size_t bucket_count() const {
    return ht_.bucket_count();
  }
  template <class F>
  auto for_each(F &&f) {
    for (auto &it : ht_) {
      f(it);
    }
  }
};

struct CellInfoHashTableDense {
  size_t dense_ht_size_{0};
  size_t dense_ht_buckets_{1};
  std::vector<size_t> dense_ht_offsets_{1};
  std::vector<CellInfo> dense_ht_values_;
  td::HashSet<CellInfo, CellHashF, CellEqF> new_ht_;
  size_t dense_choose_bucket(const CellHash &hash) const {
    return cell_hash_slice_hash(hash.as_slice()) % dense_ht_buckets_;
  }
  const CellInfo *dense_find(CellHash hash) const {
    auto bucket_i = dense_choose_bucket(hash);
    auto begin = dense_ht_values_.begin() + dense_ht_offsets_[bucket_i];
    auto end = dense_ht_values_.begin() + dense_ht_offsets_[bucket_i + 1];
    for (auto it = begin; it != end; ++it) {
      if (it->cell.not_null() && it->cell->get_hash() == hash) {
        return &*it;
      }
    }
    return nullptr;
  }
  CellInfo *dense_find_empty(CellHash hash) {
    auto bucket_i = dense_choose_bucket(hash);
    auto begin = dense_ht_values_.begin() + dense_ht_offsets_[bucket_i];
    auto end = dense_ht_values_.begin() + dense_ht_offsets_[bucket_i + 1];
    for (auto it = begin; it != end; ++it) {
      if (it->cell.is_null()) {
        return &*it;
      }
    }
    return nullptr;
  }
  const CellInfo *find(CellHash hash) const {
    if (auto it = new_ht_.find(hash); it != new_ht_.end()) {
      return &*it;
    }
    if (auto it = dense_find(hash)) {
      return it;
    }
    return nullptr;
  }
  void erase(CellHash hash) {
    if (auto it = new_ht_.find(hash); it != new_ht_.end()) {
      new_ht_.erase(it);
      return;
    }
    auto info = dense_find(hash);
    CHECK(info && info->db_refcnt > 0);
    info->db_refcnt = 0;
    const_cast<CellInfo *>(info)->cell = {};
    CHECK(dense_ht_size_ > 0);
    dense_ht_size_--;
  }

  void insert(CellInfo info) {
    if (auto dest = dense_find_empty(info.cell->get_hash())) {
      *dest = std::move(info);
      dense_ht_size_++;
      return;
    }
    new_ht_.insert(std::move(info));
  }
  template <class Iterator>
  void init_from(Iterator begin, Iterator end) {
    auto size = td::narrow_cast<size_t>(std::distance(begin, end));
    dense_ht_buckets_ = std::max(size_t(1), size_t(size / 8));

    std::vector<size_t> offsets(dense_ht_buckets_ + 2);
    for (auto it = begin; it != end; ++it) {
      auto bucket_i = dense_choose_bucket(it->cell->get_hash());
      offsets[bucket_i + 2]++;
    }
    for (size_t i = 1; i < offsets.size(); i++) {
      offsets[i] += offsets[i - 1];
    }
    dense_ht_values_.resize(size);
    for (auto it = begin; it != end; ++it) {
      auto bucket_i = dense_choose_bucket(it->cell->get_hash());
      dense_ht_values_[offsets[bucket_i + 1]++] = std::move(*it);
    }
    CHECK(offsets[0] == 0);
    CHECK(offsets[offsets.size() - 1] == size);
    CHECK(offsets[offsets.size() - 2] == size);
    dense_ht_offsets_ = std::move(offsets);
    dense_ht_size_ = size;
  }
  size_t size() const {
    return dense_ht_size_ + new_ht_.size();
  }
  template <class F>
  auto for_each(F &&f) {
    for (auto &it : dense_ht_values_) {
      if (it.cell.not_null()) {
        f(it);
      }
    }
    for (auto &it : new_ht_) {
      f(it);
    }
  }
  size_t bucket_count() const {
    return new_ht_.bucket_count() + dense_ht_values_.size();
  }
};

using CellInfoHashTable = std::conditional_t<use_dense_hash_map, CellInfoHashTableDense, CellInfoHashTableBaseline>;

class CellStorage {
  struct PrivateTag {};
  struct CellBucket;
  struct None {
    void operator()(CellBucket *bucket) {
    }
  };
  struct CellBucketRef {
    UniqueAccess::Lock lock;
    std::unique_ptr<CellBucket, None> bucket;
    CellBucket &operator*() {
      return *bucket;
    }
    CellBucket *operator->() {
      return bucket.get();
    }
  };
  struct CellBucket {
    mutable UniqueAccess access_;
    CellInfoHashTable infos_;
    std::vector<CellInfo> cells_;
    std::vector<Ref<DataCell>> roots_;
    size_t boc_count_{0};
    [[maybe_unused]] char pad3[TD_CONCURRENCY_PAD];

    void clear() {
      td::reset_to_empty(infos_);
      td::reset_to_empty(cells_);
      td::reset_to_empty(roots_);
    }

    CellBucketRef unique_access() const {
      auto lock = access_.lock();
      return CellBucketRef{.lock = std::move(lock),
                           .bucket = std::unique_ptr<CellBucket, None>(const_cast<CellBucket *>(this))};
    }
  };
  std::array<CellBucket, 256> buckets_{};
  bool inited_{false};

  const CellBucket &get_bucket(size_t i) const {
    return buckets_.at(i);
  }
  const CellBucket &get_bucket(const CellHash &hash) const {
    return get_bucket(hash.as_array()[0]);
  }

  mutable UniqueAccess local_access_;
  td::HashSet<Ref<DataCell>, CellHashF, CellEqF> local_roots_;
  DynamicBagOfCellsDb::Stats stats_;

  mutable std::mutex root_mutex_;
  td::HashSet<Ref<DataCell>, CellHashF, CellEqF> roots_;

 public:
  std::optional<CellInfo> get_info(const CellHash &hash) const {
    auto lock = local_access_.lock();
    auto &bucket = get_bucket(hash);
    if (auto info_ptr = bucket.infos_.find(hash)) {
      return *info_ptr;
    }
    return {};
  }

  DynamicBagOfCellsDb::Stats get_stats() {
    auto unique_access = local_access_.lock();
    auto stats = stats_;
    auto add_stat = [&stats](auto key, auto value) {
      stats.custom_stats.emplace_back(std::move(key), PSTRING() << value);
    };
    if constexpr (use_dense_hash_map) {
      size_t dense_ht_capacity = 0;
      size_t new_ht_capacity = 0;
      size_t dense_ht_size = 0;
      size_t new_ht_size = 0;
      for_each_bucket(0, [&](auto bucket_id, CellBucket &bucket) {
        // TODO: this leads to CE when use_dense_hash_map == false
        dense_ht_capacity += bucket.infos_.dense_ht_values_.size();
        dense_ht_size += bucket.infos_.dense_ht_size_;
        new_ht_capacity += bucket.infos_.new_ht_.bucket_count();
        new_ht_size += bucket.infos_.new_ht_.size();
      });
      auto size = new_ht_size + dense_ht_size;
      auto capacity = new_ht_capacity + dense_ht_capacity;
      add_stat("ht.capacity", capacity);
      add_stat("ht.size", size);
      add_stat("ht.load", double(size) / std::max(1.0, double(capacity)));
      add_stat("ht.dense_ht_capacity", dense_ht_capacity);
      add_stat("ht.dense_ht_size", dense_ht_size);
      add_stat("ht.dense_ht_load", double(dense_ht_size) / std::max(1.0, double(dense_ht_capacity)));
      add_stat("ht.new_ht_capacity", new_ht_capacity);
      add_stat("ht.new_ht_size", new_ht_size);
      add_stat("ht.new_ht_load", double(new_ht_size) / std::max(1.0, double(new_ht_capacity)));
    } else {
      size_t capacity = 0;
      size_t size = 0;
      for_each_bucket(0, [&](auto bucket_id, CellBucket &bucket) {
        capacity += bucket.infos_.bucket_count();
        size += bucket.infos_.size();
      });
      add_stat("ht.capacity", capacity);
      add_stat("ht.size", size);
      add_stat("ht.load", double(size) / std::max(1.0, double(capacity)));
    }
    CHECK(td::narrow_cast<size_t>(stats.roots_total_count) == local_roots_.size());
    return stats;
  }
  void apply_stats_diff(DynamicBagOfCellsDb::Stats diff) {
    auto unique_access = local_access_.lock();
    stats_.apply_diff(diff);
    CHECK(td::narrow_cast<size_t>(stats_.roots_total_count) == local_roots_.size());
    size_t cells_count{0};
    for_each_bucket(0, [&](size_t bucket_id, auto &bucket) { cells_count += bucket.infos_.size(); });
    CHECK(td::narrow_cast<size_t>(stats_.cells_total_count) == cells_count);
  }

  td::Result<Ref<DataCell>> load_cell(const CellHash &hash) const {
    auto lock = local_access_.lock();
    auto &bucket = get_bucket(hash);
    if (auto info_ptr = bucket.infos_.find(hash)) {
      return info_ptr->cell;
    }
    return td::Status::Error("not found");
  }

  td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<CellHash> hashes) const {
    std::vector<Ref<DataCell>> res;
    res.reserve(hashes.size());
    for (auto &hash : hashes) {
      TRY_RESULT(cell, load_cell(hash));
      res.push_back(std::move(cell));
    }
    return res;
  }

  td::Result<Ref<DataCell>> load_root_local(const CellHash &hash) const {
    auto lock = local_access_.lock();
    if (auto it = local_roots_.find(hash); it != local_roots_.end()) {
      return *it;
    }
    return td::Status::Error("not found");
  }
  td::Result<std::vector<Ref<DataCell>>> load_known_roots_local() const {
    auto lock = local_access_.lock();
    std::vector<Ref<DataCell>> result;
    for (auto &root : roots_) {
      result.emplace_back(root);
    }
    return result;
  }
  td::Result<Ref<DataCell>> load_root_shared(const CellHash &hash) const {
    std::lock_guard<std::mutex> lock(root_mutex_);
    if (auto it = roots_.find(hash); it != roots_.end()) {
      return *it;
    }
    return td::Status::Error("not found");
  }

  void erase(const CellHash &hash) {
    auto lock = local_access_.lock();
    auto bucket = get_bucket(hash).unique_access();
    bucket->infos_.erase(hash);
    if (auto local_it = local_roots_.find(hash); local_it != local_roots_.end()) {
      local_roots_.erase(local_it);
      std::lock_guard<std::mutex> root_lock(root_mutex_);
      auto shared_it = roots_.find(hash);
      CHECK(shared_it != roots_.end());
      roots_.erase(shared_it);
      CHECK(stats_.roots_total_count > 0);
      stats_.roots_total_count--;
    }
  }

  void add_new_root(Ref<DataCell> cell) {
    auto lock = local_access_.lock();
    if (local_roots_.insert(cell).second) {
      std::lock_guard<std::mutex> lock(root_mutex_);
      roots_.insert(std::move(cell));
      stats_.roots_total_count++;
    }
  }

  void set(td::int32 refcnt, Ref<DataCell> cell) {
    auto lock = local_access_.lock();
    //LOG(ERROR) << "setting refcnt to " << refcnt << ", cell " << td::base64_encode(cell->get_hash().as_slice());
    auto hash = cell->get_hash();
    auto bucket = get_bucket(hash).unique_access();
    if (auto info_ptr = bucket->infos_.find(hash)) {
      CHECK(info_ptr->cell.get() == cell.get());
      info_ptr->db_refcnt = refcnt;
    } else {
      bucket->infos_.insert({.db_refcnt = refcnt, .cell = std::move(cell)});
    }
  }

  template <class F>
  static td::unique_ptr<CellStorage> build(DynamicBagOfCellsDb::CreateInMemoryOptions options,
                                           F &&parallel_scan_cells) {
    auto storage = td::make_unique<CellStorage>(PrivateTag{});
    storage->do_build(options, parallel_scan_cells);
    return storage;
  }

  ~CellStorage() {
    clear();
  }
  CellStorage() = delete;
  explicit CellStorage(PrivateTag) {
  }

 private:
  template <class F>
  void do_build(DynamicBagOfCellsDb::CreateInMemoryOptions options, F &&parallel_scan_cells) {
    auto verbose = options.verbose;
    td::Slice P = "loading in-memory cell database: ";
    LOG_IF(WARNING, verbose) << P << "start with options use_arena=" << options.use_arena
                             << " use_less_memory_during_creation=" << options.use_less_memory_during_creation
                             << " use_dense_hash_map=" << use_dense_hash_map;
    auto full_timer = td::Timer();
    auto lock = local_access_.lock();
    CHECK(ArenaPrunnedCellCreator::count() == 0);
    ArenaPrunnedCellCreator arena_pc_creator;
    DefaultPrunnedCellCreator default_pc_creator;

    auto timer = td::Timer();
    td::int64 cell_count{0};
    td::int64 desc_count{0};
    if (options.use_less_memory_during_creation) {
      auto [new_cell_count, new_desc_count] = parallel_scan_cells(
          default_pc_creator, options.use_arena,
          [&](td::int32 refcnt, Ref<DataCell> cell) { initial_set_without_refs(refcnt, std::move(cell)); });
      cell_count = new_cell_count;
      desc_count = new_desc_count;
    } else {
      auto [new_cell_count, new_desc_count] =
          parallel_scan_cells(arena_pc_creator, options.use_arena,
                              [&](td::int32 refcnt, Ref<DataCell> cell) { initial_set(refcnt, std::move(cell)); });
      cell_count = new_cell_count;
      desc_count = new_desc_count;
    }
    LOG_IF(WARNING, verbose) << P << "cells loaded in " << timer.elapsed() << "s, cells_count= " << cell_count
                             << " prunned_cells_count=" << ArenaPrunnedCellCreator::count();

    timer = td::Timer();
    for_each_bucket(options.extra_threads, [&](size_t bucket_id, auto &bucket) { build_hashtable(bucket); });

    size_t ht_capacity = 0;
    size_t ht_size = 0;
    for_each_bucket(0, [&](size_t bucket_id, auto &bucket) {
      ht_size += bucket.infos_.size();
      ht_capacity += bucket.infos_.bucket_count();
    });
    double load_factor = double(ht_size) / std::max(double(ht_capacity), 1.0);
    LOG_IF(WARNING, verbose) << P << "hashtable created in " << timer.elapsed()
                             << "s,  hashtables_expected_size=" << td::format::as_size(ht_capacity * sizeof(CellInfo))
                             << " load_factor=" << load_factor;

    timer = td::Timer();
    if (options.use_less_memory_during_creation) {
      auto [new_cell_count, new_desc_count] =
          parallel_scan_cells(default_pc_creator, false,
                              [&](td::int32 refcnt, Ref<DataCell> cell) { secondary_set(refcnt, std::move(cell)); });
      CHECK(new_cell_count == cell_count);
      CHECK(new_desc_count == desc_count);
    } else {
      for_each_bucket(options.extra_threads, [&](size_t bucket_id, auto &bucket) { reset_refs(bucket); });
    }
    LOG_IF(WARNING, verbose) << P << "refs rearranged in " << timer.elapsed() << "s";

    timer = td::Timer();
    using Stats = DynamicBagOfCellsDb::Stats;
    std::vector<Stats> bucket_stats(buckets_.size());
    std::atomic<size_t> boc_count{0};
    for_each_bucket(options.extra_threads, [&](size_t bucket_id, auto &bucket) {
      bucket_stats[bucket_id] = validate_bucket_a(bucket);
      boc_count += bucket.boc_count_;
    });
    for_each_bucket(options.extra_threads, [&](size_t bucket_id, auto &bucket) { validate_bucket_b(bucket); });
    stats_ = {};
    for (auto &bucket_stat : bucket_stats) {
      stats_.apply_diff(bucket_stat);
    }
    LOG_IF(WARNING, verbose) << P << "refcnt validated in " << timer.elapsed() << "s";

    timer = td::Timer();
    build_roots();
    LOG_IF(WARNING, verbose) << P << "roots hashtable built in " << timer.elapsed() << "s";
    ArenaPrunnedCellCreator::clear_arena();
    LOG_IF(WARNING, verbose) << P << "arena cleared in " << timer.elapsed();

    lock.reset();
    auto r_mem_stat = td::mem_stat();
    td::MemStat mem_stat;
    if (r_mem_stat.is_ok()) {
      mem_stat = r_mem_stat.move_as_ok();
    }
    auto stats = get_stats();
    td::StringBuilder sb;
    for (auto &[key, value] : stats.custom_stats) {
      sb << "\n\t" << key << "=" << value;
    }
    LOG_IF(ERROR, desc_count != 0 && desc_count != stats.roots_total_count + 1)
        << "desc<> keys count is " << desc_count << " which is different from roots count " << stats.roots_total_count;
    LOG_IF(WARNING, verbose)
        << P << "done in " << full_timer.elapsed() << "\n\troots_count=" << stats.roots_total_count << "\n\t"
        << desc_count << "\n\tcells_count=" << stats.cells_total_count
        << "\n\tcells_size=" << td::format::as_size(stats.cells_total_size) << "\n\tboc_count=" << boc_count.load()
        << sb.as_cslice() << "\n\tdata_cells_size=" << td::format::as_size(sizeof(DataCell) * stats.cells_total_count)
        << "\n\tdata_cell_size=" << sizeof(DataCell) << "\n\texpected_memory_used="
        << td::format::as_size(stats.cells_total_count * (sizeof(DataCell) + sizeof(CellInfo) * 3 / 2) +
                               stats.cells_total_size)
        << "\n\tbest_possible_memory_used"
        << td::format::as_size(stats.cells_total_count * (sizeof(DataCell) + sizeof(CellInfo)) + stats.cells_total_size)
        << "\n\tmemory_used=" << td::format::as_size(mem_stat.resident_size_)
        << "\n\tpeak_memory_used=" << td::format::as_size(mem_stat.resident_size_peak_);

    inited_ = true;
  }

  template <class F>
  void for_each_bucket(size_t extra_threads, F &&f) {
    parallel_run(
        buckets_.size(), [&](auto task_id) { f(task_id, *get_bucket(task_id).unique_access()); }, extra_threads);
  }

  void clear() {
    auto unique_access = local_access_.lock();
    for_each_bucket(td::thread::hardware_concurrency(), [&](size_t bucket_id, auto &bucket) { bucket.clear(); });
    local_roots_.clear();
    {
      auto lock = std::lock_guard<std::mutex>(root_mutex_);
      roots_.clear();
    }
  }

  void initial_set(td::int32 refcnt, Ref<DataCell> cell) {
    CHECK(!inited_);
    auto bucket = get_bucket(cell->get_hash()).unique_access();
    bucket->cells_.push_back({.db_refcnt = refcnt, .cell = std::move(cell)});
  }

  void initial_set_without_refs(td::int32 refcnt, Ref<DataCell> cell_ref) {
    CHECK(!inited_);
    auto bucket = get_bucket(cell_ref->get_hash()).unique_access();
    auto &cell = const_cast<DataCell &>(*cell_ref);
    for (unsigned i = 0; i < cell.size_refs(); i++) {
      auto to_destroy = cell.reset_ref_unsafe(i, Ref<Cell>(), false);
      if (to_destroy->is_loaded()) {
        bucket->boc_count_++;
      }
    }
    bucket->cells_.push_back({.db_refcnt = refcnt, .cell = std::move(cell_ref)});
  }

  void secondary_set(td::int32 refcnt, Ref<DataCell> cell_copy) {
    CHECK(!inited_);
    auto bucket = get_bucket(cell_copy->get_hash()).unique_access();
    auto info = bucket->infos_.find(cell_copy->get_hash());
    CHECK(info);
    CellSlice cs(NoVm{}, std::move(cell_copy));
    auto &cell = const_cast<DataCell &>(*info->cell);
    CHECK(cs.size_refs() == cell.size_refs());
    for (unsigned i = 0; i < cell.size_refs(); i++) {
      auto prunned_cell_hash = cs.fetch_ref()->get_hash();
      auto &prunned_cell_bucket = get_bucket(prunned_cell_hash);
      auto full_cell_ptr = prunned_cell_bucket.infos_.find(prunned_cell_hash);
      CHECK(full_cell_ptr);
      auto full_cell = full_cell_ptr->cell;
      auto to_destroy = cell.reset_ref_unsafe(i, std::move(full_cell), false);
      CHECK(to_destroy.is_null());
    }
  }

  void build_hashtable(CellBucket &bucket) {
    bucket.infos_.init_from(bucket.cells_.begin(), bucket.cells_.end());
    LOG_CHECK(bucket.infos_.size() == bucket.cells_.size()) << bucket.infos_.size() << " vs " << bucket.cells_.size();
    td::reset_to_empty(bucket.cells_);
    LOG_CHECK(bucket.cells_.capacity() == 0) << bucket.cells_.capacity();
  }

  void reset_refs(CellBucket &bucket) {
    bucket.infos_.for_each([&](auto &it) {
      // This is generally very dangerous, but should be safe here
      auto &cell = const_cast<DataCell &>(*it.cell);
      for (unsigned i = 0; i < cell.size_refs(); i++) {
        auto prunned_cell = cell.get_ref_raw_ptr(i);
        auto prunned_cell_hash = prunned_cell->get_hash();
        auto &prunned_cell_bucket = get_bucket(prunned_cell_hash);
        auto full_cell_ptr = prunned_cell_bucket.infos_.find(prunned_cell_hash);
        CHECK(full_cell_ptr);
        auto full_cell = full_cell_ptr->cell;
        auto to_destroy = cell.reset_ref_unsafe(i, std::move(full_cell));
        if (!to_destroy->is_loaded()) {
          Ref<PrunnedCell<ArenaPrunnedCellCreator::Counter>> x(std::move(to_destroy));
          x->~PrunnedCell<ArenaPrunnedCellCreator::Counter>();
          x.release();
        } else {
          bucket.boc_count_++;
        }
      }
    });
  }

  DynamicBagOfCellsDb::Stats validate_bucket_a(CellBucket &bucket) {
    DynamicBagOfCellsDb::Stats stats;
    bucket.infos_.for_each([&](auto &it) {
      int cell_ref_cnt = it.cell->get_refcnt();
      CHECK(it.db_refcnt + 1 >= cell_ref_cnt);
      auto extra_refcnt = it.db_refcnt + 1 - cell_ref_cnt;
      if (extra_refcnt != 0) {
        bucket.roots_.push_back(it.cell);
        stats.roots_total_count++;
      }
      stats.cells_total_count++;
      stats.cells_total_size += static_cast<td::int64>(it.cell->get_storage_size());
    });
    return stats;
  }
  void validate_bucket_b(CellBucket &bucket) {
    // sanity check
    bucket.infos_.for_each([&](auto &it) {
      CellSlice cs(NoVm{}, it.cell);
      while (cs.have_refs()) {
        CHECK(cs.fetch_ref().not_null());
      }
    });
  }
  void build_roots() {
    for (auto &it : buckets_) {
      for (auto &root : it.roots_) {
        local_roots_.insert(std::move(root));
      }
      td::reset_to_empty(it.roots_);
    }
    auto lock = std::lock_guard<std::mutex>(root_mutex_);
    roots_ = local_roots_;
  }
};

class MetaStorage {
 public:
  explicit MetaStorage(std::vector<std::pair<std::string, std::string>> values)
      : meta_(std::move_iterator(values.begin()), std::move_iterator(values.end())) {
    for (auto &p : meta_) {
      CHECK(p.first.size() != CellTraits::hash_bytes);
    }
  }
  std::vector<std::pair<std::string, std::string>> meta_get_all(size_t max_count) const {
    std::vector<std::pair<std::string, std::string>> res;
    for (const auto &[k, v] : meta_) {
      if (res.size() >= max_count) {
        break;
      }
      res.emplace_back(k, v);
    }
    return res;
  }
  KeyValue::GetStatus meta_get(td::Slice key, std::string &value) const {
    auto lock = local_access_.lock();
    auto it = meta_.find(key.str());
    if (it == meta_.end()) {
      return KeyValue::GetStatus::NotFound;
    }
    value = it->second;
    return KeyValue::GetStatus::Ok;
  }
  void meta_set(td::Slice key, td::Slice value) {
    auto lock = local_access_.lock();
    meta_[key.str()] = value.str();
    meta_diffs_.push_back(
        CellStorer::MetaDiff{.type = CellStorer::MetaDiff::Set, .key = key.str(), .value = value.str()});
  }
  void meta_erase(td::Slice key) {
    auto lock = local_access_.lock();
    meta_.erase(key.str());
    meta_diffs_.push_back(CellStorer::MetaDiff{.type = CellStorer::MetaDiff::Erase, .key = key.str()});
  }
  std::vector<CellStorer::MetaDiff> extract_diffs() {
    auto lock = local_access_.lock();
    return std::move(meta_diffs_);
  }

 private:
  mutable UniqueAccess local_access_;
  std::unordered_map<std::string, std::string> meta_;
  std::vector<CellStorer::MetaDiff> meta_diffs_;
};

class InMemoryBagOfCellsDb : public DynamicBagOfCellsDb {
 public:
  explicit InMemoryBagOfCellsDb(td::unique_ptr<CellStorage> storage, td::unique_ptr<MetaStorage> meta_storage)
      : storage_(std::move(storage)), meta_storage_(std::move(meta_storage)) {
  }

  td::Result<std::vector<std::pair<std::string, std::string>>> meta_get_all(size_t max_count) const override {
    return meta_storage_->meta_get_all(max_count);
  }
  td::Result<KeyValue::GetStatus> meta_get(td::Slice key, std::string &value) override {
    CHECK(key.size() != CellTraits::hash_bytes);
    return meta_storage_->meta_get(key, value);
  }
  td::Status meta_set(td::Slice key, td::Slice value) override {
    meta_storage_->meta_set(key, value);
    return td::Status::OK();
  }
  td::Status meta_erase(td::Slice key) override {
    meta_storage_->meta_erase(key);
    return td::Status::OK();
  }

  td::Result<Ref<DataCell>> load_cell(td::Slice hash) override {
    return storage_->load_cell(CellHash::from_slice(hash));
  }

  td::Result<std::vector<Ref<DataCell>>> load_known_roots() const override {
    return storage_->load_known_roots_local();
  }
  td::Result<Ref<DataCell>> load_root(td::Slice hash) override {
    return storage_->load_root_local(CellHash::from_slice(hash));
  }
  td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<td::Slice> hashes) override {
    return storage_->load_bulk(td::transform(hashes, [](auto &hash) { return CellHash::from_slice(hash); }));
  }
  td::Result<Ref<DataCell>> load_root_thread_safe(td::Slice hash) const override {
    return storage_->load_root_shared(CellHash::from_slice(hash));
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

  td::Status commit(CellStorer &cell_storer) override {
    if (!to_inc_.empty() || !to_dec_.empty()) {
      TRY_STATUS(prepare_commit());
    }

    td::PerfWarningTimer times_save_diff("save diff");
    Stats diff;
    CHECK(to_dec_.empty());
    for (auto &info : info_) {
      if (info.diff_refcnt == 0) {
        continue;
      }
      auto refcnt = td::narrow_cast<td::int32>(static_cast<td::int64>(info.db_refcnt) + info.diff_refcnt);
      LOG_CHECK(refcnt >= 0) << info.db_refcnt << " + " << info.diff_refcnt;
      if (refcnt > 0) {
        if (info.db_refcnt == 0) {
          TRY_STATUS(cell_storer.set(refcnt, info.cell, false));
        } else {
          TRY_STATUS(cell_storer.merge(info.cell->get_hash().as_slice(), info.diff_refcnt));
        }
        storage_->set(refcnt, info.cell);
        if (info.db_refcnt == 0) {
          diff.cells_total_count++;
          diff.cells_total_size += static_cast<td::int64>(info.cell->get_storage_size());
        }
      } else {
        TRY_STATUS(cell_storer.erase(info.cell->get_hash().as_slice()));
        storage_->erase(info.cell->get_hash());
        diff.cells_total_count--;
        diff.cells_total_size -= static_cast<td::int64>(info.cell->get_storage_size());
      }
    }
    auto meta_diffs = meta_storage_->extract_diffs();
    for (const auto &meta_diff : meta_diffs) {
      TRY_STATUS(cell_storer.apply_meta_diff(meta_diff));
    }
    storage_->apply_stats_diff(diff);
    info_ = {};
    return td::Status::OK();
  }

  td::Result<Stats> get_stats() override {
    return storage_->get_stats();
  }

  // Not implemented or trivial or deprecated methods
  td::Status set_loader(std::unique_ptr<CellLoader> loader) override {
    return td::Status::OK();
  }

  td::Status prepare_commit() override {
    CHECK(info_.empty());
    for (auto &to_inc : to_inc_) {
      auto new_root = do_inc(to_inc);
      storage_->add_new_root(std::move(new_root));
    }
    for (auto &to_dec : to_dec_) {
      do_dec(to_dec);
    }
    to_dec_ = {};
    to_inc_ = {};
    return td::Status::OK();
  }
  void prepare_commit_async(std::shared_ptr<AsyncExecutor> executor, td::Promise<td::Unit> promise) override {
    TRY_STATUS_PROMISE(promise, prepare_commit());
    promise.set_value(td::Unit());
  }
  Stats get_stats_diff() override {
    LOG(FATAL) << "Not implemented";
    return {};
  }
  std::shared_ptr<CellDbReader> get_cell_db_reader() override {
    return {};
  }
  void set_celldb_compress_depth(td::uint32 value) override {
    LOG(FATAL) << "Not implemented";
  }
  ExtCellCreator &as_ext_cell_creator() override {
    UNREACHABLE();
  }
  void load_cell_async(td::Slice hash, std::shared_ptr<AsyncExecutor> executor,
                       td::Promise<Ref<DataCell>> promise) override {
    LOG(FATAL) << "Not implemented";
  }

 private:
  td::unique_ptr<CellStorage> storage_;
  td::unique_ptr<MetaStorage> meta_storage_;

  struct Info {
    mutable td::int32 db_refcnt{0};
    mutable td::int32 diff_refcnt{0};
    Ref<DataCell> cell;
    vm::CellHash key() const {
      return cell->get_hash();
    }
    struct Eq {
      using is_transparent = void;  // Pred to use
      bool operator()(const Info &info, const Info &other_info) const {
        return info.key() == other_info.key();
      }
      bool operator()(const Info &info, td::Slice hash) const {
        return info.key().as_slice() == hash;
      }
      bool operator()(td::Slice hash, const Info &info) const {
        return info.key().as_slice() == hash;
      }
    };
    struct Hash {
      using is_transparent = void;  // Pred to use
      using transparent_key_equal = Eq;
      size_t operator()(td::Slice hash) const {
        return cell_hash_slice_hash(hash);
      }
      size_t operator()(const Info &info) const {
        return cell_hash_slice_hash(info.key().as_slice());
      }
    };
  };
  td::HashSet<Info, Info::Hash, Info::Eq> info_;

  std::unique_ptr<CellLoader> loader_;
  std::vector<Ref<Cell>> to_inc_;
  std::vector<Ref<Cell>> to_dec_;

  Ref<DataCell> do_inc(Ref<Cell> cell) {
    auto cell_hash = cell->get_hash();
    if (auto it = info_.find(cell_hash.as_slice()); it != info_.end()) {
      CHECK(it->diff_refcnt != std::numeric_limits<td::int32>::max());
      it->diff_refcnt++;
      return it->cell;
    }
    if (auto o_info = storage_->get_info(cell_hash)) {
      info_.emplace(Info{.db_refcnt = o_info->db_refcnt, .diff_refcnt = 1, .cell = o_info->cell});
      return std::move(o_info->cell);
    }

    CellSlice cs(NoVm{}, std::move(cell));
    CellBuilder cb;
    cb.store_bits(cs.data(), cs.size());
    while (cs.have_refs()) {
      auto ref = do_inc(cs.fetch_ref());
      cb.store_ref(std::move(ref));
    }
    auto res = cb.finalize(cs.is_special());
    CHECK(res->get_hash() == cell_hash);
    info_.emplace(Info{.db_refcnt = 0, .diff_refcnt = 1, .cell = res});
    return res;
  }

  void do_dec(Ref<Cell> cell) {
    auto cell_hash = cell->get_hash();
    auto it = info_.find(cell_hash.as_slice());
    if (it != info_.end()) {
      CHECK(it->diff_refcnt != std::numeric_limits<td::int32>::min());
      --it->diff_refcnt;
    } else {
      auto info = *storage_->get_info(cell_hash);
      it = info_.emplace(Info{.db_refcnt = info.db_refcnt, .diff_refcnt = -1, .cell = info.cell}).first;
    }
    if (it->diff_refcnt + it->db_refcnt != 0) {
      return;
    }
    CellSlice cs(NoVm{}, std::move(cell));
    while (cs.have_refs()) {
      do_dec(cs.fetch_ref());
    }
  }
};

}  // namespace

std::unique_ptr<DynamicBagOfCellsDb> DynamicBagOfCellsDb::create_in_memory(td::KeyValueReader *kv,
                                                                           CreateInMemoryOptions options) {
  if (kv == nullptr) {
    LOG_IF(WARNING, options.verbose) << "Create empty in-memory cells database (no key value is given)";
    auto storage = CellStorage::build(options, [](auto, auto, auto) { return std::make_pair(0, 0); });
    auto meta_storage = td::make_unique<MetaStorage>(std::vector<std::pair<std::string, std::string>>{});
    return std::make_unique<InMemoryBagOfCellsDb>(std::move(storage), std::move(meta_storage));
  }

  std::vector<std::string> keys;
  keys.emplace_back("");
  for (td::uint32 c = 1; c <= 0xff; c++) {
    keys.emplace_back(1, static_cast<char>(c));
  }
  keys.emplace_back(33, static_cast<char>(0xff));

  auto parallel_scan_cells = [&](ExtCellCreator &pc_creator, bool use_arena,
                                 auto &&f) -> std::pair<td::int64, td::int64> {
    std::atomic<td::int64> cell_count{0};
    std::atomic<td::int64> desc_count{0};
    parallel_run(
        keys.size() - 1,
        [&](auto task_id) {
          td::int64 local_cell_count = 0;
          td::int64 local_desc_count = 0;
          CHECK(!DataCell::use_arena);
          DataCell::use_arena = use_arena;
          kv->for_each_in_range(keys.at(task_id), keys.at(task_id + 1), [&](td::Slice key, td::Slice value) {
              if (td::begins_with(key, "desc") && key.size() != 32) {
                local_desc_count++;
                return td::Status::OK();
              }
              if (key.size() != 32) {
                return td::Status::OK();
              }
              auto r_res = CellLoader::load(key, value.str(), true, pc_creator);
              if (r_res.is_error()) {
                LOG(ERROR) << r_res.error() << " at " << td::format::escaped(key);
                return td::Status::OK();
              }
              CHECK(key.size() == 32);
              CHECK(key.ubegin()[0] == task_id);
              auto res = r_res.move_as_ok();
              f(res.refcnt(), res.cell());
              local_cell_count++;
              return td::Status::OK();
            }).ensure();
          DataCell::use_arena = false;
          cell_count += local_cell_count;
          desc_count += local_desc_count;
        },
        options.extra_threads);
    return std::make_pair(cell_count.load(), desc_count.load());
  };

  auto storage = CellStorage::build(options, parallel_scan_cells);

  std::vector<std::pair<std::string, std::string>> meta;
  // NB: it scans 1/(2^32) of the database which is not much
  kv->for_each_in_range("desc", "desd", [&meta](td::Slice key, td::Slice value) {
    if (key.size() != 32) {
      meta.emplace_back(key.str(), value.str());
    }
    return td::Status::OK();
  });
  // this is for tests mostly. desc* keys are expected to correspond to roots
  kv->for_each_in_range("meta", "metb", [&meta](td::Slice key, td::Slice value) {
    if (key.size() != 32) {
      meta.emplace_back(key.str(), value.str());
    }
    return td::Status::OK();
  });
  auto meta_storage = td::make_unique<MetaStorage>(std::move(meta));

  return std::make_unique<InMemoryBagOfCellsDb>(std::move(storage), std::move(meta_storage));
}
}  // namespace vm
