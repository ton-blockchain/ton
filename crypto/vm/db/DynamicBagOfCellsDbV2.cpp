#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/db/CellStorage.h"
#include "vm/db/CellHashTable.h"

#include "vm/cells/ExtCell.h"

#include "td/utils/base64.h"
#include "td/utils/format.h"
#include "td/utils/ThreadSafeCounter.h"
#include "td/utils/misc.h"
#include "validator/validator.h"

#include "vm/cellslice.h"

#include <optional>

namespace vm {
namespace {

// Very stupid Vector/MpmcQueue
template <class T>
struct TsVector {
  TsVector() {
    first_block_size_ = 64;
    blocks_[0].data.resize(first_block_size_);
    blocks_[0].is_ready = true;
  }
  TsVector(std::vector<T> base) {
    first_block_size_ = base.size();
    blocks_[0].data = std::move(base);
    blocks_[0].is_ready = true;
  }
  struct Block {
    std::mutex mutex;
    std::atomic<bool> is_ready{false};
    std::vector<T> data;
  };
  T &at(size_t i) {
    td::uint64 j = i / first_block_size_;
    td::int32 hb = 63 - td::count_leading_zeroes64(j);  // hb = -1 if j=0, else hb>=0

    // If j=0, hb<0, so hb>>31 = -1 => mask=0
    // If j>0, hb>=0, so hb>>31=0  => mask=~0 (all ones)
    td::uint64 mask = ~(td::uint64)(hb >> 31);

    size_t block_i = hb + 1;
    uint64_t shift = hb & 63ULL;
    uint64_t start = ((1ULL << shift) * first_block_size_) & mask;
    size_t pos_in_block = i - start;
    auto &block = blocks_[block_i];
    if (block.is_ready.load(std::memory_order_acquire)) {
      return block.data.at(pos_in_block);
    }

    std::unique_lock<std::mutex> lock(block.mutex);
    if (block.is_ready.load(std::memory_order_acquire)) {
      return block.data.at(pos_in_block);
    }
    block.resize(start);
    block.is_ready.store(true, std::memory_order_release);
    return block.data.at(pos_in_block);
  }
  template <class S>
  void push_back(S &&value) {
    at(end_.fetch_add(1, std::memory_order_relaxed)) = std::forward<S>(value);
  }
  T pop_front() {
    auto pos = begin_.fetch_add(1, std::memory_order_relaxed);
    while (pos >= end_.load(std::memory_order_acquire)) {
      // This may (or may not) use too much CPU
      td::this_thread::yield();
    }
    return std::move(at(pos));
  }
  size_t size() const {
    return end_.load();
  }

  std::array<Block, 64> blocks_;
  size_t first_block_size_{0};
  std::atomic<size_t> begin_{0};
  std::atomic<size_t> end_{0};
};
struct CellInfo;

class CellDbReaderExt;
struct DynamicBocExtCellExtra {
  std::shared_ptr<CellDbReaderExt> reader;
};

class DynamicBocCellLoader {
 public:
  static td::Result<Ref<DataCell>> load_data_cell(const ExtCell<DynamicBocExtCellExtra, DynamicBocCellLoader> &cell,
                                                  const DynamicBocExtCellExtra &extra);
};
using DynamicBocExtCell = ExtCell<DynamicBocExtCellExtra, DynamicBocCellLoader>;

class CellDbReaderExt : public CellDbReader {
 public:
  virtual td::Result<Ref<DataCell>> load_ext_cell(Ref<DynamicBocExtCell> cell) = 0;
};

td::Result<Ref<DataCell>> DynamicBocCellLoader::load_data_cell(const DynamicBocExtCell &cell,
                                                               const DynamicBocExtCellExtra &extra) {
  return extra.reader->load_ext_cell(Ref(&cell));
}

#define S(x)                                 \
  td::NamedThreadSafeCounter::CounterRef x { \
    nc.get_counter(#x)                       \
  }

struct CacheStats {
  td::NamedThreadSafeCounter nc;
  S(load_cell_ext);
  S(load_cell_ext_cache_hits);
  S(load_cell_sync);
  S(load_cell_sync_cache_hits);
  S(load_cell_async);
  S(load_cell_async_cache_hits);
  S(ext_cells);
  S(ext_cells_load);
  S(ext_cells_load_cache_hits);

  S(kv_read_found);
  S(kv_read_not_found);

  S(sync_with_db);
  S(sync_with_db_only_ref);
  S(load_cell_no_cache);
};

struct CommitStats {
  td::NamedThreadSafeCounter nc;

  S(to_inc);
  S(to_dec);

  S(gather_new_cells_calls);
  S(gather_new_cells_calls_it);
  S(update_parents_calls);
  S(update_parents_calls_it);
  S(dec_calls);
  S(dec_calls_it);

  S(new_cells);
  S(new_cells_leaves);

  S(new_cells_loaded_not_in_db);
  S(new_cells_loaded_in_db);
  S(new_cells_not_in_db_fast);

  S(dec_loaded);
  S(dec_to_zero);

  S(changes_loaded);

  // new diff logic
  S(diff_zero);
  S(diff_full);
  S(diff_erase);
  S(diff_ref_cnt);

  // old full data logic
  S(inc_save);
  S(inc_save_full);
  S(inc_save_only_ref_cnt);
  S(inc_new_cell);
  S(inc_just_ref_cnt);

  S(dec_save);
  S(dec_save_full);
  S(dec_save_only_refcnt);
  S(dec_save_erase);
  S(dec_erase_cell);
  S(dec_just_ref_cnt);
};

template <class T>
struct AtomicPod {
  T load() const {
    while (true) {
      if (auto res = try_read_stable()) {
        return res->second;
      }
    }
  }

  template <class F>
  std::pair<T, bool> update(F &&f) {
    while (true) {
      auto res = try_read_stable();
      if (!res) {
        continue;
      }
      auto [before, old_data] = *res;

      auto o_new_data = f(old_data);
      if (!o_new_data) {
        return {old_data, false};
      }

      if (!lock_.compare_exchange_weak(before, before + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }

      data_ = *o_new_data;  // relaxed store inside lock
      lock_.fetch_add(1, std::memory_order_release);
      return {*o_new_data, true};
    }
  }

 private:
  mutable std::atomic<std::uint64_t> lock_{0};
  T data_{};

  std::optional<std::pair<std::uint64_t, T>> try_read_stable() const {
    auto before = lock_.load(std::memory_order_acquire);
    if (before % 2 == 1) {
      return std::nullopt;
    }
    T temp = data_;  // relaxed read is ok, checked by versioning
    auto after = lock_.load(std::memory_order_acquire);
    if (after != before) {
      return std::nullopt;
    }
    return std::make_pair(before, temp);
  }
};

struct InDbInfo {
  std::vector<CellInfo *> parents;
  std::atomic<td::uint32> pending_children{0};
  std::atomic<bool> maybe_in_db{true};
  std::atomic<bool> visited_in_gather_new_cells{false};
};
td::StringBuilder &operator<<(td::StringBuilder &sb, const InDbInfo &info) {
  sb << "mb_in_db:" << info.maybe_in_db.load() << " chld_n:" << info.pending_children
     << " prnt_n:" << info.parents.size();
  return sb;
}
struct CellInfo {
  struct State {
    // db_ref_cnt and in_db are correct
    bool sync_with_db{false};

    // ignore if sync_with_db is false
    td::int32 db_ref_cnt{0};
    td::int32 db_refcnt_fixup{0};

    // if true - cell is definitely in db
    // if false - we know that cell is not in db only is sync_with_db=true
    bool in_db{false};

    // diff to be applied
  };
  AtomicPod<State> state;
  std::atomic<td::int32> ref_cnt_diff{0};

  std::atomic<bool> visited{false};
  td::unique_ptr<InDbInfo> in_db_info_ptr;
  std::mutex mutex;

  // Could be AtomicRef<Cell>, but is am not sure that it is worth it
  const Ref<Cell> cell;

  explicit CellInfo(Ref<Cell> cell) : cell(std::move(cell)) {
  }

  InDbInfo &in_db_info() {
    return *in_db_info_ptr;
  }
  const InDbInfo &in_db_info() const {
    return *in_db_info_ptr;
  }
  InDbInfo &in_db_info_create() {  // NOT thread safe
    if (!in_db_info_ptr) {
      in_db_info_ptr = td::make_unique<InDbInfo>();
    }
    return in_db_info();
  }
  InDbInfo &in_db_info_create(CellInfo *parent) {  // Thread Safe
    std::unique_lock lock(mutex);
    if (!in_db_info_ptr) {
      in_db_info_ptr = td::make_unique<InDbInfo>();
    }
    auto &res = *in_db_info_ptr;
    if (parent != nullptr) {
      res.parents.emplace_back(parent);
    }
    lock.unlock();
    return res;
  }
  void in_db_info_destroy() {
    in_db_info_ptr = nullptr;
  }
  td::int32 inc_ref_cnt() {
    return ref_cnt_diff.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  td::int32 dec_ref_cnt() {
    return ref_cnt_diff.fetch_sub(1, std::memory_order_relaxed) - 1;
  }
  td::int32 get_ref_cnt_diff() const {
    return ref_cnt_diff.load(std::memory_order_relaxed);
  }

  void set_not_in_db() {
    state.update([&](State state) -> std::optional<State> {
      if (state.sync_with_db) {
        CHECK(state.db_ref_cnt == 0);
        CHECK(!state.in_db);
        return {};
      }
      state.sync_with_db = true;
      state.in_db = false;
      state.db_ref_cnt = 0;
      return state;
    });
  }
  void set_in_db() {
    state.update([&](State state) -> std::optional<State> {
      if (state.sync_with_db) {
        //LOG_CHECK(state.in_db) << *this;
        return {};
      }
      state.in_db = true;
      return state;
    });
  }
  void synced_with_db(td::int32 db_ref_cnt) {
    state.update([&](State state) -> std::optional<State> {
      if (state.sync_with_db) {
        CHECK(state.in_db);
        CHECK(state.db_ref_cnt == db_ref_cnt);
        return {};
      }
      state.in_db = true;
      state.db_ref_cnt = db_ref_cnt;
      return state;
    });
  }
  bool visit() {
    return !visited.exchange(true);
  }
  void on_written_to_db() {
    auto diff = ref_cnt_diff.exchange(0);
    state.update([&](State state) -> std::optional<State> {
      if (diff == 0) {
        return {};
      }
      if (state.sync_with_db) {
        state.db_ref_cnt += diff;
        CHECK(state.db_ref_cnt >= 0);
        state.in_db = state.db_ref_cnt > 0;
      } else {
        CHECK(diff > 0);
        state.in_db = true;
        state.db_refcnt_fixup += diff;
      }
      return state;
    });
  }

  td::Result<Ref<DataCell>> get_data_cell() {
    TRY_RESULT(loaded_cell, cell->load_cell());
    return loaded_cell.data_cell;
  }
  Cell::Hash key() const {
    return cell->get_hash();
  }
  bool operator<(const CellInfo &other) const {
    return key() < other.key();
  }

  struct Eq {
    using is_transparent = void;  // Pred to use
    bool operator()(const CellInfo &info, const CellInfo &other_info) const {
      return info.key() == other_info.key();
    }
    bool operator()(const CellInfo &info, td::Slice hash) const {
      return info.key().as_slice() == hash;
    }
    bool operator()(td::Slice hash, const CellInfo &info) const {
      return info.key().as_slice() == hash;
    }
  };
  struct Hash {
    using is_transparent = void;  // Pred to use
    using transparent_key_equal = Eq;
    size_t operator()(td::Slice hash) const {
      return cell_hash_slice_hash(hash);
    }
    size_t operator()(const CellInfo &info) const {
      return cell_hash_slice_hash(info.key().as_slice());
    }
  };
};
td::StringBuilder &operator<<(td::StringBuilder &sb, const CellInfo &info) {
  if (info.cell->is_loaded()) {
    auto data_cell = info.cell->load_cell().move_as_ok().data_cell;
    vm::CellSlice cs(vm::NoVm{}, data_cell);
    sb << data_cell->get_hash().to_hex().substr(0, 8) << " refs:" << data_cell->size_refs()
       << " data:" << cs.data_bits().to_hex(cs.size()) << " data_ptr=" << data_cell.get() << " data_ref_cnt("
       << data_cell->get_refcnt() << ")";
  } else {
    sb << info.cell->get_hash().to_hex().substr(0, 8);
  }
  auto state = info.state.load();
  sb << " " << &info;
  sb << "\n\tin_db=" << state.in_db << " sync_with_db=" << state.sync_with_db
     << " ref_cnt_diff=" << info.get_ref_cnt_diff() << " db_ref_cnt=" << state.db_ref_cnt
     << " db_ref_cnt_fixup=" << state.db_refcnt_fixup;
  if (state.sync_with_db) {
    sb << " REFS(" << info.get_ref_cnt_diff() + state.db_ref_cnt << ")";
  }
  if (info.in_db_info_ptr) {
    sb << " " << info.in_db_info();
  }
  sb << " visited=" << info.visited.load();
  return sb;
}

struct ExecutorOptions {
  size_t extra_threads_n{0};
  std::shared_ptr<DynamicBagOfCellsDb::AsyncExecutor> async_executor;
};
template <class InputT = CellInfo *, class OutputT = CellInfo *>
class ExecutorImpl {
 public:
  ExecutorImpl(ExecutorOptions options) : options_(options) {
  }
  ExecutorOptions options_;
  using InputData = std::vector<std::vector<InputT>>;
  using OutputData = std::vector<std::vector<OutputT>>;
  struct InputChunk {
    td::Span<InputT> infos;
    size_t begin{};
    size_t end{};
  };

  template <class F>
  OutputData process(const InputData &data, const F &process_task_f) {
    if (options_.extra_threads_n > 0) {
      return process_parallel(data, process_task_f);
    } else {
      return process_sequential(data, process_task_f);
    }
  }
  template <class F>
  struct SingleThreadWorker {
    const F &process_task_f;
    mutable std::vector<OutputT> results{};
    void add_task(InputT input) const {
      process_task_f(input, *this);
    }
    void add_result(OutputT output) const {
      results.push_back(output);
    }
  };
  template <class F>
  OutputData process_sequential(const InputData &data, const F &process_task_f) {
    auto w = SingleThreadWorker<F>{process_task_f};
    for (auto &chunk : data) {
      for (auto &info : chunk) {
        process_task_f(info, w);
      }
    }

    return {std::move(w.results)};
  }

  template <class ProcessTaskF>
  struct Shared;

  template <class ProcessTaskF>
  struct Worker {
    size_t worker_i{};
    std::shared_ptr<Shared<ProcessTaskF>> shared;

    void add_task(InputT input) const {
      shared->delay_or_process_task(input, *this);
    }
    void add_result(OutputT value) const {
      shared->add_result(value, worker_i);
    }
    void loop() const {
      shared->loop(*this);
    }
  };

  template <class ProcessTaskF>
  struct Shared {
    Shared(size_t workers_n, const InputData &input_data, const ProcessTaskF &process_task_f)
        : input_chunks(prepare_input_chunks(input_data))
        , workers_n(workers_n)
        , input_size(input_chunks.empty() ? 0 : input_chunks.back().end)
        , batch_size(std::clamp(input_size / workers_n / 4, size_t(1), size_t(128)))
        , process_task_f(process_task_f) {
    }

    const std::vector<InputChunk> input_chunks;

    const size_t workers_n{0};
    const size_t input_size{0};
    const size_t batch_size{128};

    const ProcessTaskF &process_task_f;

    // Position in input
    std::atomic<size_t> next_input_i{0};

    // Shared queue
    // Probably a simpler queue would also work fine
    td::MpmcQueue<InputT> mpmc_queue{workers_n};
    using Waiter = td::MpmcSleepyWaiter;
    Waiter waiter;
    std::atomic<size_t> mpmc_queue_size{workers_n};  // guard

    // Output vectors
    struct ThreadData {
      std::vector<OutputT> output;
      char pad[TD_CONCURRENCY_PAD - sizeof(output)];
    };
    std::vector<ThreadData> thread_data{workers_n};

    auto prepare_input_chunks(const InputData &input_data) {
      std::vector<InputChunk> chunks;
      for (auto &chunk : input_data) {
        size_t prev_end = chunks.empty() ? 0 : chunks.back().end;
        chunks.push_back({.infos = td::as_span(chunk), .begin = prev_end, .end = prev_end + chunk.size()});
      }
      return chunks;
    }

    void delay_or_process_task(InputT input, const Worker<ProcessTaskF> &worker) {
      // if there is enough tasks in queue, we continue recursion
      if (mpmc_queue_size.load(std::memory_order_acquire) > 256) {
        process_task_f(input, worker);
      } else {
        mpmc_queue_size.fetch_add(1, std::memory_order_acq_rel);
        mpmc_queue.push(input, worker.worker_i);
        waiter.notify();
      }
    }

    void add_result(OutputT result, size_t worker_i) {
      thread_data[worker_i].output.push_back(std::move(result));
    }

    void process_initial_input(const Worker<ProcessTaskF> &worker) {
      size_t input_chunk_i = 0;
      while (true) {
        auto begin_i = next_input_i.fetch_add(batch_size, std::memory_order_relaxed);
        auto end_i = begin_i + batch_size;
        if (begin_i >= input_size) {
          break;
        }
        for (size_t i = begin_i; i < end_i && i < input_size; i++) {
          while (input_chunks[input_chunk_i].end <= i) {
            input_chunk_i++;
          }
          auto offset = i - input_chunks[input_chunk_i].begin;
          auto task = input_chunks[input_chunk_i].infos[offset];
          process_task_f(task, worker);
        }
      }
    }

    void on_processed_task_from_queue(size_t worker_i) {
      if (mpmc_queue_size.fetch_add(-1, std::memory_order_acq_rel) == 1) {
        for (size_t i = 0; i < workers_n; i++) {
          mpmc_queue.push(nullptr, worker_i);
          waiter.notify();
        }
      }
    }

    void process_queue(const Worker<ProcessTaskF> &worker) {
      on_processed_task_from_queue(worker.worker_i);

      Waiter::Slot slot;
      waiter.init_slot(slot, td::narrow_cast<td::int32>(worker.worker_i));

      while (true) {
        InputT input{};
        if (mpmc_queue.try_pop(input, worker.worker_i)) {
          waiter.stop_wait(slot);
          if (!input) {
            break;
          }
          process_task_f(input, worker);
          on_processed_task_from_queue(worker.worker_i);
        } else {
          waiter.wait(slot);
        }
      }
    }
    void loop(const Worker<ProcessTaskF> &worker) {
      process_initial_input(worker);
      process_queue(worker);
    }
    void finish() const {
      CHECK(mpmc_queue_size == 0);
    }
  };

  template <class F>
  OutputData process_parallel(const InputData &input_data, const F &process_task_f) {
    const size_t workers_n = options_.extra_threads_n + 1;
    auto shared = std::make_shared<Shared<F>>(workers_n, input_data, process_task_f);

    CHECK(workers_n >= 1);
    for (size_t i = 0; i < workers_n; i++) {
      auto to_run = [worker = Worker<F>{.worker_i = i, .shared = shared}] { worker.loop(); };

      if (i + 1 == workers_n) {
        to_run();
      } else if (options_.async_executor) {
        options_.async_executor->execute_async(std::move(to_run));
      } else {
        // NB: td::thread, NOT std::thread
        td::thread(std::move(to_run)).detach();
      }
    }
    shared->finish();
    return td::transform(shared->thread_data, [](auto &&x) { return std::move(x.output); });
  }
};
struct Executor {
  Executor(ExecutorOptions options = {}) : options_(options) {
  }

  template <class InputT = CellInfo *, class OutputT = CellInfo *, class F>
  auto operator()(const std::vector<std::vector<InputT>> &data, const F &process_task_f) {
    return ExecutorImpl<InputT, OutputT>(options_).process(data, process_task_f);
  }

 private:
  ExecutorOptions options_;
};

// Thread safe storage for CellInfo
// Will be used by everybody as shared cache. Yes there is some overhead, but it don't want to create other hash table
struct CellInfoStorage {
 public:
  // All methods are thead safe
  // All CellInfo pointers lives as long as CellInfoStorage

  // returns CellInfo, only if it is already exists
  CellInfo *get_cell_info(td::Slice hash) {
    return lock(hash)->hash_table.get_if_exists(hash);
  }

  CellInfo &create_cell_info_from_db(Ref<DataCell> data_cell, td::int32 ref_cnt) {
    auto &info = create_cell_info_from_data_cell(std::move(data_cell));
    info.synced_with_db(ref_cnt);
    return info;
  }

  // Creates CellInfo from data_cell, or updates existing CellInfo if is not yet loaded
  CellInfo &create_cell_info_from_data_cell(Ref<DataCell> cell) {
    CHECK(cell.not_null());
    CHECK(cell->is_loaded());

    auto hash = cell->get_hash();
    auto [info, created] = lock(hash.as_slice())->hash_table.emplace(hash.as_slice(), std::move(cell));

    if (!created) {
      info.cell->set_data_cell(std::move(cell));
    }
    return info;
  }

  // Creates CellInfo from cell. If cell is loaded, it will be used to rewrite or udpate current cell
  CellInfo &create_cell_info(Ref<Cell> cell, CellDbReaderExt *from_reader, CacheStats &stats) {
    if (cell->is_loaded()) {
      return create_cell_info_from_data_cell(cell->load_cell().move_as_ok().data_cell);
    }

    bool our_ext_cell = false;
    auto ext_cell = dynamic_cast<const DynamicBocExtCell *>(cell.get());
    if (ext_cell) {
      auto prunned_cell = ext_cell->get_prunned_cell();
      if (prunned_cell.not_null()) {
        our_ext_cell = prunned_cell->get_extra().reader.get() == from_reader;
      }
      our_ext_cell = true;
    } else if (!cell->is_loaded()) {
      // if we cached cell from OTHER db is good idea to drop it ASAP
      force_drop_cache_.store(true, std::memory_order_relaxed);
    }

    auto hash = cell->get_hash();
    auto [info, created] = lock(hash.as_slice())->hash_table.emplace(hash.as_slice(), std::move(cell));
    if (our_ext_cell) {
      stats.ext_cells_load.inc();
      if (info.cell->is_loaded()) {
        stats.ext_cells_load_cache_hits.inc();
      }
      info.set_in_db();
    }
    return info;
  }

  void dump() {
    LOG(ERROR) << "===========BEGIN DUMP===========";
    for (auto &bucket : buckets_) {
      std::lock_guard guard(bucket.mutex);
      bucket.hash_table.for_each([&](auto &info) { LOG(INFO) << info; });
    }
    LOG(ERROR) << "===========END   DUMP===========";
  }

  size_t cache_size() {
    size_t res = 0;
    for (auto &bucket : buckets_) {
      std::lock_guard guard(bucket.mutex);
      res += bucket.hash_table.size();
    }
    return res;
  }
  bool force_drop_cache() {
    return force_drop_cache_.load(std::memory_order_relaxed);
  }

 private:
  struct Bucket {
    std::mutex mutex;
    CellHashTable<CellInfo> hash_table;
  };
  constexpr static size_t buckets_n = 8192;
  std::array<Bucket, buckets_n> bucket_;

  struct Unlock {
    void operator()(Bucket *bucket) const {
      bucket->mutex.unlock();
    }
  };
  std::array<Bucket, buckets_n> buckets_{};
  std::atomic<bool> force_drop_cache_{false};

  std::unique_ptr<Bucket, Unlock> lock(Bucket &bucket) {
    bucket.mutex.lock();
    return std::unique_ptr<Bucket, Unlock>(&bucket);
  }
  std::unique_ptr<Bucket, Unlock> lock(td::Slice key) {
    auto hash = td::as<size_t>(key.substr(16, 8).ubegin());
    auto bucket_i = hash % buckets_n;
    return lock(buckets_[bucket_i]);
  }
};

class DynamicBagOfCellsDbImplV2 : public DynamicBagOfCellsDb {
 public:
  explicit DynamicBagOfCellsDbImplV2(CreateV2Options options) : options_(options) {
    get_thread_safe_counter().inc();
    // LOG(ERROR) << "Constructor called for DynamicBagOfCellsDbImplV2";
  }
  ~DynamicBagOfCellsDbImplV2() {
    // LOG(ERROR) << "Destructor called for DynamicBagOfCellsDbImplV2";
    get_thread_safe_counter().add(-1);

    if (cell_db_reader_) {
      cell_db_reader_->drop_cache();
    }
  }

  td::Result<std::vector<std::pair<std::string, std::string>>> meta_get_all(size_t max_count) const override {
    CHECK(meta_db_fixup_.empty());
    std::vector<std::pair<std::string, std::string>> result;
    auto s = cell_db_reader_->key_value_reader().for_each_in_range(
        "desc", "desd", [&](const td::Slice &key, const td::Slice &value) {
           if (result.size() >= max_count) {
             return td::Status::Error("COUNT_LIMIT");
           }
          if (td::begins_with(key, "desc") && key.size() != 32) {
            result.emplace_back(key.str(), value.str());
          }
          return td::Status::OK();
        });
    if (s.message() == "COUNT_LIMIT") {
      s = td::Status::OK();
    }
    TRY_STATUS(std::move(s));
    return result;
  }
  td::Result<KeyValue::GetStatus> meta_get(td::Slice key, std::string &value) override {
    auto it = meta_db_fixup_.find(key);
    if (it != meta_db_fixup_.end()) {
      if (it->second.empty()) {
        return KeyValue::GetStatus::NotFound;
      }
      value = it->second;
      return KeyValue::GetStatus::Ok;
    }
    return cell_db_reader_->key_value_reader().get(key, value);
  }
  td::Status meta_set(td::Slice key, td::Slice value) override {
    meta_diffs_.push_back(
        CellStorer::MetaDiff{.type = CellStorer::MetaDiff::Set, .key = key.str(), .value = value.str()});
    return td::Status::OK();
  }
  td::Status meta_erase(td::Slice key) override {
    meta_diffs_.push_back(CellStorer::MetaDiff{.type = CellStorer::MetaDiff::Erase, .key = key.str()});
    return td::Status::OK();
  }
  td::Result<Ref<DataCell>> load_cell(td::Slice hash) override {
    CHECK(cell_db_reader_);
    return cell_db_reader_->load_cell(hash);
  }
  td::Result<Ref<DataCell>> load_root(td::Slice hash) override {
    return load_cell(hash);
  }
  td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<td::Slice> hashes) override {
    CHECK(cell_db_reader_);
    return cell_db_reader_->load_bulk(hashes);
  }
  td::Result<Ref<DataCell>> load_root_thread_safe(td::Slice hash) const override {
    // TODO: it is better to use AtomicRef, or atomic shared pointer
    // But to use AtomicRef we need a little refactoring
    // And std::atomic<std::shared_ptr<>> is still unsupported by clang
    std::unique_lock lock(atomic_cell_db_reader_mutex_);
    auto reader = atomic_cell_db_reader_;
    lock.unlock();
    if (!reader) {
      return td::Status::Error("Empty reader");
    }
    return reader->load_cell(hash);
  }
  void load_cell_async(td::Slice hash, std::shared_ptr<AsyncExecutor> executor,
                       td::Promise<Ref<DataCell>> promise) override {
    CHECK(cell_db_reader_);
    return cell_db_reader_->load_cell_async(hash, std::move(executor), std::move(promise));
  }
  void prepare_commit_async(std::shared_ptr<AsyncExecutor> executor, td::Promise<td::Unit> promise) override {
    auto promise_ptr = std::make_shared<td::Promise<td::Unit>>(std::move(promise));
    executor->execute_async([this, promise_ptr = std::move(promise_ptr)] {
      prepare_commit();
      promise_ptr->set_value(td::Unit());
    });
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
    return {};
  }

  td::Status prepare_commit() override {
    if (is_prepared_for_commit()) {
      return td::Status::OK();
    }
    // NB: we don't use options.executor, because it is prone to deadlocks. We need extra_threads_n threads
    // available for blocking
    Executor executor{{.extra_threads_n = options_.extra_threads, .async_executor = {}}};
    // calculate in_db for all vertices reachable from to_inc_ roots
    // - for ext cells we already know they are in db
    // - calculate in_db up from leaves
    // - if at least one child is not in db, then the cell is definitely not in db
    // - so in best case only leaves will be loaded from db
    // - this is optional step. All other logic must work in any case
    // - only already loaded cells are loaded from db

    stats_.to_inc.add(to_inc_.size());
    stats_.to_dec.add(to_dec_.size());

    std::vector<std::vector<CellInfo *>> visited_cells;
    auto add_visited_cells = [&](std::vector<std::vector<CellInfo *>> new_visited_cells) {
      for (auto &x : new_visited_cells) {
        visited_cells.push_back(std::move(x));
      }
    };

    std::vector<std::vector<CellInfo *>> new_cells_leaves;
    {
      td::PerfWarningTimer timer("celldb_v2: gather_new_cells");
      std::vector<CellInfo *> prepared_to_inc;
      std::vector<CellInfo *> visited_roots;
      for (auto &cell : to_inc_) {
        auto &info = cell_db_reader_->cell_info(cell);
        if (info.inc_ref_cnt() == 1 && info.visit()) {
          visited_roots.push_back(&info);
        }
        if (info.state.load().in_db) {
          continue;
        }
        auto &in_db_info = info.in_db_info_create(nullptr);
        if (!in_db_info.visited_in_gather_new_cells.exchange(true)) {
          prepared_to_inc.push_back(&info);
        }
      }
      new_cells_leaves =
          executor({std::move(prepared_to_inc)}, [&](CellInfo *info, auto &worker) { gather_new_cells(info, worker); });
      visited_cells.push_back(std::move(visited_roots));
    }

    // LOG(WARNING) << "new_cells_leaves: " << new_cells_leaves.size();
    {
      td::PerfWarningTimer timer("celldb_v2: update_parents");
      add_visited_cells(
          executor({std::move(new_cells_leaves)}, [&](CellInfo *info, auto &worker) { update_parents(info, worker); }));
    }
    {
      td::PerfWarningTimer timer("dec");
      std::vector<CellInfo *> prepared_to_dec;
      for (auto &cell : to_dec_) {
        auto &info = cell_db_reader_->cell_info(cell);
        prepared_to_dec.push_back(&info);
      }
      add_visited_cells(
          executor({std::move(prepared_to_dec)}, [&](CellInfo *info, auto &worker) { dec_cell(info, worker); }));
    }

    td::PerfWarningTimer timer_serialize("celldb_v2: save_diff_serialize", 0.01);
    // LOG(INFO) << "threads_n = " << options_.extra_threads + 1;
    diff_chunks_ = executor.operator()<CellInfo *, CellStorer::Diff>(
        visited_cells, [&](CellInfo *info, auto &worker) { serialize_diff(info, worker); });
    timer_serialize.reset();

    {
      td::PerfWarningTimer timer("celldb_v2: clear");
      to_inc_.clear();
      to_dec_.clear();
    }

    //cell_db_reader_->dump();
    return td::Status::OK();
  }

  td::Status commit(CellStorer &storer) override {
    prepare_commit();
    save_diff(storer);
    // We DON'T delete entries from cache, so cache actually represents diff with snapshot in reader
    // But we don't want took keep old snapshot forever
    LOG_IF(ERROR, dbg) << "clear cell_db_reader";
    //cell_db_reader_->dump();
    //TODO: call drop_cache reliably via rtti

    constexpr bool always_drop_cache = false;
    if (always_drop_cache) {
      td::PerfWarningTimer timer("celldb_v2: reset reader");
      cell_db_reader_->drop_cache();
      cache_stats_.apply_diff(cell_db_reader_->get_stats());
      cache_stats_.stats_int["commits"] += 1;
      cell_db_reader_ = {};
      // keep atomic reader, to it will be reused
    }
    return td::Status::OK();
  }

  std::shared_ptr<CellDbReader> get_cell_db_reader() override {
    CHECK(cell_db_reader_);
    return cell_db_reader_;
  }

  td::Status set_loader(std::unique_ptr<CellLoader> loader) override {
    if (cell_db_reader_) {
      auto cache_size = cell_db_reader_->cache_size();
      bool force_drop_cache = cell_db_reader_->force_drop_cache();
      if (loader && cache_size < options_.cache_size_max && cell_db_reader_ttl_ < options_.cache_ttl_max &&
          !force_drop_cache) {
        // keep cache
        cell_db_reader_ttl_++;
        return td::Status::OK();
      }

      td::PerfWarningTimer timer(PSTRING() << "celldb_v2: reset reader, TTL=" << cell_db_reader_ttl_ << "/"
                                           << options_.cache_ttl_max << ", cache_size=" << cache_size
                                           << ", force_drop_cache=" << force_drop_cache);
      cache_stats_.apply_diff(cell_db_reader_->get_stats());
      cell_db_reader_->drop_cache();
      cell_db_reader_ = {};
      meta_db_fixup_ = {};
      cell_db_reader_ttl_ = 0;
    }

    if (loader) {
      cell_db_reader_ = std::make_shared<CellDbReaderImpl>(std::move(loader));
      cell_db_reader_ttl_ = 0;
    }

    {
      std::lock_guard guard(atomic_cell_db_reader_mutex_);
      atomic_cell_db_reader_ = cell_db_reader_;
    }
    return td::Status::OK();
  }

  void set_celldb_compress_depth(td::uint32 value) override {
    celldb_compress_depth_ = value;
  }

  vm::ExtCellCreator &as_ext_cell_creator() override {
    CHECK(cell_db_reader_);
    return *cell_db_reader_;
  }
  td::Result<Stats> get_stats() override {
    auto ps = stats_.nc.get_stats().with_prefix("storage_");
    ps.apply_diff(cache_stats_.with_prefix("cache_cum_"));
    if (cell_db_reader_) {
      ps.apply_diff(cell_db_reader_->get_stats().with_prefix("cache_now_"));
      ps.apply_diff(cell_db_reader_->get_stats().with_prefix("cache_cum_"));
    }
    Stats res;
    res.named_stats = std::move(ps);
    res.named_stats.stats_int["cache.size"] = cell_db_reader_ ? cell_db_reader_->cache_size() : 0;
    res.named_stats.stats_int["cache.size_max"] = options_.cache_size_max;
    res.named_stats.stats_int["cache.ttl"] = cell_db_reader_ttl_;
    res.named_stats.stats_int["cache.ttl_max"] = options_.cache_ttl_max;
    return res;
  }

 private:
  static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
    static auto res = td::NamedThreadSafeCounter::get_default().get_counter("DynamicBagOfCellsDb");
    return res;
  }

  class CellDbReaderImpl : public CellDbReaderExt,
                           public ExtCellCreator,
                           public std::enable_shared_from_this<CellDbReaderImpl> {
   public:
    explicit CellDbReaderImpl(std::unique_ptr<CellLoader> cell_loader) : cell_loader_(std::move(cell_loader)) {
    }

    size_t cache_size() const {
      // NOT thread safe
      if (internal_storage_) {
        return internal_storage_->cache_size();
      }
      return 0;
    }
    bool force_drop_cache() const {
      // NOT thread safe
      if (internal_storage_) {
        return internal_storage_->force_drop_cache();
      }
      return false;
    }
    void drop_cache() {
      // NOT thread safe
      internal_storage_.reset();
    }

    td::Result<Ref<Cell>> ext_cell(Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
      // thread safe function
      stats_.ext_cells.inc();
      TRY_RESULT(ext_cell, DynamicBocExtCell::create(PrunnedCellInfo{level_mask, hash, depth},
                                                     DynamicBocExtCellExtra{shared_from_this()}));

      return ext_cell;
    }
    CellInfo *register_ext_cell_inner(Ref<DynamicBocExtCell> ext_cell, CellInfoStorage &storage) {
      auto &info = storage.create_cell_info(std::move(ext_cell), this, stats_);
      return &info;
    }

    void load_cell_async(td::Slice hash, std::shared_ptr<AsyncExecutor> executor, td::Promise<Ref<DataCell>> promise) {
      // thread safe function
      stats_.load_cell_async.inc();
      auto maybe_cell = load_cell_fast_path(hash, false, nullptr);
      if (maybe_cell.not_null()) {
        stats_.load_cell_async_cache_hits.inc();
        return promise.set_value(std::move(maybe_cell));
      }
      auto promise_ptr = std::make_shared<td::Promise<Ref<DataCell>>>(std::move(promise));

      executor->execute_async(
          [self = shared_from_this(), promise_ptr = std::move(promise_ptr), hash = CellHash::from_slice(hash)]() {
            promise_ptr->set_result(self->load_cell(hash.as_slice()));
          });
    }

    td::Result<Ref<DataCell>> load_cell(td::Slice hash) override {
      // thread safe function
      stats_.load_cell_sync.inc();
      bool loaded{false};
      auto maybe_cell = load_cell_fast_path(hash, true, &loaded);
      if (maybe_cell.not_null()) {
        if (!loaded) {
          stats_.load_cell_sync_cache_hits.inc();
        }
        return maybe_cell;
      }
      return load_cell_slow_path(hash);
    }

    td::Result<std::vector<Ref<DataCell>>> load_bulk(td::Span<td::Slice> hashes) override {
      // thread safe function
      std::vector<Ref<DataCell>> result;
      result.reserve(hashes.size());
      for (auto &hash : hashes) {
        auto maybe_cell = load_cell(hash);
        if (maybe_cell.is_error()) {
          return maybe_cell.move_as_error();
        }
        result.push_back(maybe_cell.move_as_ok());
      }
      return result;
    }

    td::Result<Ref<DataCell>> load_ext_cell(Ref<DynamicBocExtCell> ext_cell) override {
      // thread safe function.
      // Called by external cell
      stats_.load_cell_ext.inc();
      auto storage = weak_storage_.lock();
      if (!storage) {
        TRY_RESULT(load_result, load_cell_no_cache(ext_cell->get_hash().as_slice()));
        return load_result.cell_;
      }
      // we delayed registering ext cell till this moment
      auto cell_info = register_ext_cell_inner(std::move(ext_cell), *storage);

      CHECK(cell_info != nullptr);  // currently all ext_cells are registered in cache
      if (!cell_info->cell->is_loaded()) {
        sync_with_db(*cell_info, true);
        CHECK(cell_info->cell->is_loaded());  // critical, better to fail
      } else {
        stats_.load_cell_ext_cache_hits.inc();
      }
      return cell_info->cell->load_cell().move_as_ok().data_cell;
    }

    CellInfo &cell_info(Ref<Cell> cell) {
      // thread safe function, but called only by DB
      CHECK(internal_storage_)
      return internal_storage_->create_cell_info(std::move(cell), this, stats_);
    }

    std::pair<CellInfo::State, bool> sync_with_db(CellInfo &info, bool need_data) {
      // thread safe function, but called only by DB
      auto effective_need_data = need_data;
      if (info.cell->is_loaded()) {
        effective_need_data = false;
      }
      return info.state.update([&](CellInfo::State state) -> std::optional<CellInfo::State> {
        if (state.sync_with_db) {
          return {};
        }
        stats_.sync_with_db.inc();
        if (!effective_need_data) {
          stats_.sync_with_db_only_ref.inc();
        }
        auto load_result =
            cell_loader_->load(info.cell->get_hash().as_slice(), effective_need_data, *this).move_as_ok();

        state.sync_with_db = true;
        if (load_result.status == CellLoader::LoadResult::NotFound) {
          CHECK(state.in_db == false);
          CHECK(state.db_ref_cnt == 0);
          stats_.kv_read_not_found.inc();
          return state;
        }
        stats_.kv_read_found.inc();

        state.in_db = true;
        state.db_ref_cnt = load_result.refcnt() + state.db_refcnt_fixup;
        if (load_result.cell().not_null()) {
          info.cell->set_data_cell(std::move(load_result.cell()));
        }
        CHECK(!need_data || info.cell->is_loaded());
        return state;
      });
    }

    void dump() {
      internal_storage_->dump();
    }

    td::NamedStats get_stats() const {
      return stats_.nc.get_stats();
    }
    td::KeyValueReader &key_value_reader() {
      return cell_loader_->key_value_reader();
    }

   private:
    static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
      static auto res = td::NamedThreadSafeCounter::get_default().get_counter("DynamicBagOfCellsDbLoader");
      return res;
    }
    std::shared_ptr<CellInfoStorage> internal_storage_{std::make_shared<CellInfoStorage>()};
    std::weak_ptr<CellInfoStorage> weak_storage_{internal_storage_};
    std::unique_ptr<CellLoader> cell_loader_;
    CacheStats stats_;

    Ref<DataCell> load_cell_fast_path(td::Slice hash, bool may_block, bool *loaded) {
      auto storage = weak_storage_.lock();
      if (!storage) {
        return {};
      }
      auto cell_info = storage->get_cell_info(hash);
      if (cell_info != nullptr) {
        if (!cell_info->cell->is_loaded()) {
          if (may_block) {
            if (loaded) {
              *loaded = true;
            }
            CHECK(cell_info->state.load().in_db);
            sync_with_db(*cell_info, true);
            CHECK(cell_info->cell->is_loaded());
          } else {
            return {};
          }
        }
        return cell_info->cell->load_cell().move_as_ok().data_cell;
      }
      return {};
    }
    td::Result<CellLoader::LoadResult> load_cell_no_cache(td::Slice hash) {
      stats_.load_cell_no_cache.inc();
      TRY_RESULT(load_result, cell_loader_->load(hash, true, *this));
      if (load_result.status == CellLoader::LoadResult::NotFound) {
        stats_.kv_read_not_found.inc();
        return td::Status::Error("Cell load failed: not in db");
      }
      stats_.kv_read_found.inc();
      return load_result;
    }
    td::Result<Ref<DataCell>> load_cell_slow_path(td::Slice hash) {
      TRY_RESULT(load_result, load_cell_no_cache(hash));
      auto storage = weak_storage_.lock();
      if (!storage) {
        return load_result.cell_;
      }
      auto &cell_info = storage->create_cell_info_from_db(std::move(load_result.cell()), load_result.refcnt());
      return cell_info.cell->load_cell().move_as_ok().data_cell;
    }
  };

  CreateV2Options options_;
  td::int32 celldb_compress_depth_{0};
  std::vector<Ref<Cell>> to_inc_;
  std::vector<Ref<Cell>> to_dec_;
  std::vector<std::vector<CellStorer::Diff>> diff_chunks_;
  std::vector<CellStorer::MetaDiff> meta_diffs_;
  std::map<std::string, std::string, std::less<>> meta_db_fixup_;

  mutable std::mutex atomic_cell_db_reader_mutex_;
  std::shared_ptr<CellDbReaderImpl> atomic_cell_db_reader_;

  std::shared_ptr<CellDbReaderImpl> cell_db_reader_;
  size_t cell_db_reader_ttl_{0};
  td::NamedStats cache_stats_;
  CommitStats stats_;
  bool dbg{false};

  template <class WorkerT>
  void gather_new_cells(CellInfo *info, WorkerT &worker) {
    stats_.gather_new_cells_calls.inc();
    do {
      // invariant: info is not in DB; with created in_db_info
      // we enter into each root only once
      stats_.gather_new_cells_calls_it.inc();
      stats_.new_cells.inc();
      auto &in_db_info = info->in_db_info();

      CellSlice cs(vm::NoVm{}, info->cell);  // ensure cell is loaded
      CellInfo *prev_child_info = nullptr;
      while (cs.have_refs()) {
        auto *child_info = &cell_db_reader_->cell_info(cs.fetch_ref());
        auto child_state = child_info->state.load();

        if (child_state.in_db) {
          LOG_IF(INFO, dbg) << "gather_new_cells: IN DB\n\tchld: " << *child_info;
          continue;
        }

        auto &child_in_db_info = child_info->in_db_info_create(info);
        in_db_info.pending_children.fetch_add(1, std::memory_order_relaxed);

        if (child_in_db_info.visited_in_gather_new_cells.exchange(true)) {
          continue;
        }

        if (prev_child_info != nullptr) {
          worker.add_task(prev_child_info);
        }
        prev_child_info = child_info;
      }
      LOG_IF(INFO, dbg) << "gather_new_cells: NOT IN DB\n\t" << *info;
      if (in_db_info.pending_children.load(std::memory_order_relaxed) == 0) {
        worker.add_result(info);
        stats_.new_cells_leaves.inc();
        LOG_IF(WARNING, dbg) << "gather_new_cells: ADD LEAVE\n\t" << *info;
      }
      info = prev_child_info;
    } while (info != nullptr);
  }

  template <class WorkerT>
  void update_parents(CellInfo *info, const WorkerT &worker) {
    stats_.update_parents_calls.inc();
    size_t it = 0;
    do {
      stats_.update_parents_calls_it.inc();
      it++;
      //LOG(INFO) << "update_parents: it=" << it << "\n\t";
      auto &in_db_info = info->in_db_info();
      bool in_db = false;
      if (in_db_info.maybe_in_db.load(std::memory_order_relaxed)) {
        auto [state, loaded] = cell_db_reader_->sync_with_db(*info, false);
        in_db = state.in_db;
        if (in_db) {
          stats_.new_cells_loaded_in_db.inc();
        } else {
          stats_.new_cells_loaded_not_in_db.inc();
        }
      } else {
        stats_.new_cells_not_in_db_fast.inc();
        info->set_not_in_db();
      }
      LOG_IF(INFO, dbg) << "update_parents: it=" << it << "\n\t" << *info;

      CellInfo *prev_parent{nullptr};
      for (auto &parent : in_db_info.parents) {
        auto &parent_in_db_info = parent->in_db_info();
        if (!in_db) {
          parent_in_db_info.maybe_in_db.store(false, std::memory_order_relaxed);
        }
        if (parent_in_db_info.pending_children.fetch_sub(1, std::memory_order_release) == 1) {
          if (prev_parent) {
            worker.add_task(prev_parent);
          }
          prev_parent = parent;
        }
      }
      if (!in_db) {
        CellSlice cs(vm::NoVm{}, info->cell);
        while (cs.have_refs()) {
          auto child = cs.fetch_ref();
          auto &child_info = cell_db_reader_->cell_info(std::move(child));
          if (child_info.inc_ref_cnt() == 1 && child_info.visit()) {
            worker.add_result(&child_info);
          }
        }
      }
      info->in_db_info_destroy();
      info = prev_parent;
    } while (info);
  }

  template <class WorkerT>
  void dec_cell(CellInfo *info, WorkerT &worker) {
    stats_.dec_calls.inc();

    while (true) {
      stats_.dec_calls_it.inc();
      if (info->visit()) {
        worker.add_result(info);
      }
      auto ref_cnt_diff = info->dec_ref_cnt();
      if (ref_cnt_diff > 0) {
        LOG_IF(INFO, dbg) << "NOT DEC"
                          << "\n\t" << info;
        break;
      }
      auto state = info->state.load();
      if (ref_cnt_diff == 0 && state.in_db) {
        LOG_IF(INFO, dbg) << "NOT DEC (in_db) "
                          << "\n\t" << info;
        break;
      }
      if (!state.sync_with_db) {
        state = cell_db_reader_->sync_with_db(*info, true).first;
        stats_.dec_loaded.inc();
        CHECK(ref_cnt_diff == 0 || state.in_db);
      }
      auto ref_cnt = state.db_ref_cnt + ref_cnt_diff;
      if (ref_cnt > 0) {
        LOG_IF(INFO, dbg) << "DEC " << ref_cnt << "\n\t" << info;
      } else {
        LOG_IF(ERROR, dbg) << "DEC " << ref_cnt << "\n\t" << info;
      }
      CHECK(ref_cnt >= 0);
      if (ref_cnt > 0) {
        break;
      }
      stats_.dec_to_zero.inc();
      CellSlice cs(vm::NoVm{}, info->cell);
      if (!cs.have_refs()) {
        break;
      }
      while (cs.size_refs() > 1) {
        worker.add_task(&cell_db_reader_->cell_info(cs.fetch_ref()));
      }
      info = &cell_db_reader_->cell_info(cs.fetch_ref());
    }
  }

  template <class Worker>
  void serialize_diff(CellInfo *info, Worker &worker) {
    info->visited.store(false, std::memory_order_relaxed);
    auto ref_cnt_diff = info->get_ref_cnt_diff();
    if (ref_cnt_diff == 0) {
      stats_.diff_zero.inc();
      return;
    }
    auto should_compress = celldb_compress_depth_ != 0 && info->cell->get_depth() == celldb_compress_depth_;

    bool merge_supported = true;
    if (merge_supported) {
      auto state = info->state.load();
      if (ref_cnt_diff < 0) {
        CHECK(state.sync_with_db);
        /*
        if (state.db_ref_cnt + ref_cnt_diff == 0) {
          LOG(ERROR) << "DEC ERASE " << info->cell->get_hash().to_hex();
        } else {
          LOG(ERROR) << "DEC MERGE " << info->cell->get_hash().to_hex() << *info;
        }
        */
      }
      if (ref_cnt_diff < 0 && state.sync_with_db && state.db_ref_cnt + ref_cnt_diff == 0) {
        // Erase is better than Merge+CompactionFilter
        // So I see no reason for CompactionFilter at all
        worker.add_result({.type = CellStorer::Diff::Erase, .key = info->cell->get_hash()});
        stats_.diff_erase.inc();
      } else {
        bool with_data = ref_cnt_diff > 0 && !state.in_db;
        if (with_data) {
          CHECK(state.sync_with_db);
          auto data_cell = info->cell->load_cell().move_as_ok().data_cell;
          stats_.diff_full.inc();
          worker.add_result({.type = CellStorer::Diff::Set,
                             .key = info->cell->get_hash(),
                             .value = CellStorer::serialize_value(ref_cnt_diff + state.db_ref_cnt, data_cell, should_compress)});
        } else {
          stats_.diff_ref_cnt.inc();
          worker.add_result({.type = CellStorer::Diff::Merge,
                             .key = info->cell->get_hash(),
                             .value = CellStorer::serialize_refcnt_diffs(ref_cnt_diff)});
        }
      }
      info->on_written_to_db();
      return;
    }

    auto state = info->state.load();
    if (!state.sync_with_db) {
      stats_.changes_loaded.inc();
      state = cell_db_reader_->sync_with_db(*info, true).first;
    }
    CHECK(state.sync_with_db);
    auto new_ref_cnt = ref_cnt_diff + state.db_ref_cnt;

    if (ref_cnt_diff < 0) {
      stats_.dec_save.inc();
      if (new_ref_cnt == 0) {
        stats_.dec_erase_cell.inc();

        LOG_IF(ERROR, dbg) << "DEC ERASE " << *info;
        worker.add_result({.type = CellStorer::Diff::Erase, .key = info->cell->get_hash()});
        stats_.dec_save_erase.inc();
      } else {
        stats_.dec_just_ref_cnt.inc();

        LOG_IF(ERROR, dbg) << "DEC REFCNT " << *info;
        CHECK(info->cell->is_loaded());
        worker.add_result(
            {.type = CellStorer::Diff::Set,
             .key = info->cell->get_hash(),
             .value = CellStorer::serialize_value(new_ref_cnt, info->cell->load_cell().move_as_ok().data_cell, should_compress)});
        stats_.dec_save_full.inc();
      }
    } else {
      stats_.inc_save.inc();
      CHECK(info->cell->is_loaded());
      if (state.db_ref_cnt == 0) {
        stats_.inc_new_cell.inc();
        LOG_IF(ERROR, dbg) << "INC CREATE " << *info;
      } else {
        stats_.inc_just_ref_cnt.inc();
        LOG_IF(ERROR, dbg) << "INC REFCNT " << *info;
      }

      worker.add_result(
          {.type = CellStorer::Diff::Set,
           .key = info->cell->get_hash(),
           .value = CellStorer::serialize_value(new_ref_cnt, info->cell->load_cell().move_as_ok().data_cell, should_compress)});
      stats_.inc_save_full.inc();
    }
  }

  void save_diff(CellStorer &storer) {
    td::PerfWarningTimer timer("celldb_v2: save_diff");
    td::PerfWarningTimer timer_store_to_db("celldb_v2: save_diff_store_to_db", 0.01);
    // Have no idea hot to parallelize this in case of rocksdb
    for (auto &diffs : diff_chunks_) {
      for (auto &diff : diffs) {
        storer.apply_diff(diff).ensure();
      }
    }
    for (auto &meta_diff : meta_diffs_) {
      meta_db_fixup_[meta_diff.key] = meta_diff.value;
      storer.apply_meta_diff(meta_diff).ensure();
    }
    timer_store_to_db.reset();
    td::PerfWarningTimer timer_clear("celldb_v2: save_diff_clear");
    diff_chunks_.clear();
    meta_diffs_.clear();
    timer_clear.reset();
  }
};
}  // namespace

std::unique_ptr<DynamicBagOfCellsDb> DynamicBagOfCellsDb::create_v2(CreateV2Options options) {
  return std::make_unique<DynamicBagOfCellsDbImplV2>(options);
}
}  // namespace vm
