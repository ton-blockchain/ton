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
#include "vm/boc.h"
#include "vm/cells.h"
#include "common/AtomicRef.h"
#include "vm/cells/CellString.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/db/CellStorage.h"
#include "vm/db/TonDb.h"
#include "vm/db/StaticBagOfCellsDb.h"

#include "td/utils/base64.h"
#include "td/utils/benchmark.h"
#include "td/utils/crypto.h"
#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/Span.h"
#include "td/utils/Status.h"
#include "td/utils/Timer.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"
#include "td/utils/format.h"
#include "td/utils/misc.h"
#include "td/utils/tests.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/tl_helpers.h"

#include "td/db/utils/BlobView.h"
#include "td/db/RocksDb.h"
#include "td/db/MemoryKeyValue.h"
#include "td/db/utils/CyclicBuffer.h"

#include <set>
#include <map>
#include <thread>
#include <barrier>

#include <openssl/sha.h>

#include "openssl/digest.hpp"
#include "storage/db.h"
#include "td/utils/VectorQueue.h"
#include "vm/dict.h"

#include <latch>
#include <numeric>
#include <optional>
#include <variant>

#include <rocksdb/compaction_filter.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/db.h>

#include "td/actor/actor.h"
#include "td/utils/overloaded.h"

class ActorExecutor : public vm::DynamicBagOfCellsDb::AsyncExecutor {
 public:
  ActorExecutor(size_t tn) : tn_(tn) {
    scheduler_.run_in_context([&] { worker_ = td::actor::create_actor<Worker>("Worker"); });
    thread_ = td::thread([this]() { scheduler_.run(); });
  }
  ~ActorExecutor() {
    scheduler_.run_in_context_external([&] { send_closure(worker_, &Worker::close); });
    thread_.join();
  }
  std::string describe() const override {
    return PSTRING() << "ActorExecutor(tn=" << tn_ << ")";
  }
  class Worker : public td::actor::Actor {
   public:
    void close() {
      td::actor::core::SchedulerContext::get()->stop();
      stop();
    }
    void execute_sync(std::function<void()> f) {
      f();
    }
  };

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
    auto context = td::actor::SchedulerContext::get();
    if (context) {
      td::actor::create_actor<Runner>("executeasync", std::move(f)).release();
    } else {
      scheduler_.run_in_context_external(
          [&] { td::actor::create_actor<Runner>("executeasync", std::move(f)).release(); });
    }
  }

  void execute_sync(std::function<void()> f) override {
    auto context = td::actor::SchedulerContext::get();
    if (context) {
      td::actor::send_closure(worker_, &Worker::execute_sync, std::move(f));
    } else {
      scheduler_.run_in_context_external(
          [&] { td::actor::send_closure(worker_, &Worker::execute_sync, std::move(f)); });
    }
  }

 private:
  size_t tn_;
  td::actor::Scheduler scheduler_{{tn_}, false, td::actor::Scheduler::Paused};
  td::actor::ActorOwn<Worker> worker_;
  td::thread thread_;
};

namespace vm {
std::vector<int> do_get_serialization_modes() {
  std::vector<int> res;
  for (int i = 0; i < 32; i++) {
    if ((i & BagOfCells::Mode::WithCacheBits) && !(i & BagOfCells::Mode::WithIndex)) {
      continue;
    }
    res.push_back(i);
  }
  return res;
}
const std::vector<int> &get_serialization_modes() {
  static auto modes = do_get_serialization_modes();
  return modes;
}

template <class T>
int get_random_serialization_mode(T &rnd) {
  auto &modes = get_serialization_modes();
  return modes[rnd.fast(0, (int)modes.size() - 1)];
}

class BenchSha : public td::Benchmark {
 public:
  explicit BenchSha(size_t n) : str_(n, 'a') {
  }
  std::string get_description() const override {
    return PSTRING() << get_name() << " length=" << str_.size();
  }

  virtual std::string get_name() const = 0;

 protected:
  std::string str_;
};
class BenchSha256 : public BenchSha {
 public:
  using BenchSha::BenchSha;
  std::string get_name() const override {
    return "SHA256";
  }

  void run(int n) override {
    int res = 0;
    for (int i = 0; i < n; i++) {
      digest::SHA256 hasher;
      hasher.feed(str_);
      unsigned char buf[32];
      hasher.extract(buf);
      res += buf[0];
    }
    td::do_not_optimize_away(res);
  }
};
class BenchSha256Reuse : public BenchSha {
 public:
  using BenchSha::BenchSha;

  std::string get_name() const override {
    return "SHA256 reuse (used in DataCell)";
  }

  void run(int n) override {
    int res = 0;
    digest::SHA256 hasher;
    for (int i = 0; i < n; i++) {
      hasher.reset();
      hasher.feed(str_);
      unsigned char buf[32];
      hasher.extract(buf);
      res += buf[0];
    }
    td::do_not_optimize_away(res);
  }
};
class BenchSha256Low : public BenchSha {
 public:
  using BenchSha::BenchSha;

  std::string get_name() const override {
    return "SHA256 low level";
  }

// Use the old method to check for performance degradation
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // Disable deprecated warning for MSVC
#endif
  void run(int n) override {
    int res = 0;
    SHA256_CTX ctx;
    for (int i = 0; i < n; i++) {
      SHA256_Init(&ctx);
      SHA256_Update(&ctx, str_.data(), str_.size());
      unsigned char buf[32];
      SHA256_Final(buf, &ctx);
      res += buf[0];
    }
    td::do_not_optimize_away(res);
  }
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

class BenchSha256Tdlib : public BenchSha {
 public:
  using BenchSha::BenchSha;

  std::string get_name() const override {
    return "SHA256 TDLib";
  }

  void run(int n) override {
    int res = 0;
    static TD_THREAD_LOCAL td::Sha256State *ctx;
    for (int i = 0; i < n; i++) {
      td::init_thread_local<td::Sha256State>(ctx);
      ctx->init();
      ctx->feed(str_);
      unsigned char buf[32];
      ctx->extract(td::MutableSlice(buf, 32), false);
      res += buf[0];
    }
    td::do_not_optimize_away(res);
  }
};

template <class F>
void bench_threaded(F &&f) {
  class Threaded : public td::Benchmark {
   public:
    explicit Threaded(F &&f) : f_(std::move(f)), base(f_()) {
    }
    F f_;
    std::decay_t<decltype(f_())> base;

    std::string get_description() const override {
      return base.get_description() + " threaded";
    }

    void run(int n) override {
      std::atomic<int> task_i{0};
      int chunk_size = 1024;
      int num_threads = 16;
      n *= num_threads;
      std::vector<td::thread> threads;
      for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() mutable {
          auto bench = f_();
          while (true) {
            i = task_i.fetch_add(chunk_size, std::memory_order_relaxed);
            auto i_end = std::min(n, i + chunk_size);
            if (i > n) {
              break;
            }
            bench.run(i_end - i);
          }
        });
      }
      for (auto &thread : threads) {
        thread.join();
      }
    };
  };
  bench(Threaded(std::forward<F>(f)));
}
TEST(Cell, sha_benchmark) {
  for (size_t n : {4, 64, 128}) {
    bench(BenchSha256Tdlib(n));
    bench(BenchSha256Low(n));
    bench(BenchSha256Reuse(n));
    bench(BenchSha256(n));
  }
}
TEST(Cell, sha_benchmark_threaded) {
  for (size_t n : {4, 64, 128}) {
    bench_threaded([n] { return BenchSha256Tdlib(n); });
    bench_threaded([n]() { return BenchSha256Low(n); });
    bench_threaded([n]() { return BenchSha256Reuse(n); });
    bench_threaded([n]() { return BenchSha256(n); });
  }
}
class BenchTasks : public td::Benchmark {
 public:
  explicit BenchTasks(size_t tn) : tn_(tn) {
  }

  std::string get_description() const override {
    return PSTRING() << "bench_tasks(threads_n=" << tn_ << ")";
  }

  void run(int n) override {
    ActorExecutor executor(tn_);
    for (int i = 0; i < n; i++) {
      std::latch latch(tn_);
      for (size_t j = 0; j < tn_; j++) {
        executor.execute_async([&]() { latch.count_down(); });
      }
      latch.wait();
    }
  }

 private:
  size_t tn_{};
};
TEST(Bench, Tasks) {
  for (size_t tn : {1, 4, 16}) {
    bench(BenchTasks(tn));
  }
}

std::string serialize_boc(Ref<Cell> cell, int mode = 31) {
  CHECK(cell.not_null());
  vm::BagOfCells boc;
  boc.add_root(std::move(cell));
  boc.import_cells().ensure();
  auto res = boc.serialize_to_string(mode);
  CHECK(res.size() != 0);
  return res;
}
std::string serialize_boc(td::Span<Ref<Cell>> cells, int mode = 31) {
  CHECK(!cells.empty());
  vm::BagOfCells boc;
  for (auto cell : cells) {
    boc.add_root(std::move(cell));
  }
  boc.import_cells().ensure();
  auto res = boc.serialize_to_string(mode);
  CHECK(res.size() != 0);
  return res;
}

Ref<Cell> deserialize_boc(td::Slice serialized) {
  vm::BagOfCells boc;
  boc.deserialize(serialized).ensure();
  return boc.get_root_cell();
}
std::vector<Ref<Cell>> deserialize_boc_multiple(td::Slice serialized) {
  vm::BagOfCells boc;
  boc.deserialize(serialized).ensure();
  std::vector<Ref<Cell>> res;
  for (int i = 0; i < boc.get_root_count(); i++) {
    res.push_back(boc.get_root_cell(i));
  }
  return res;
}

class CellExplorer {
 public:
  struct Op {
    enum { Pop, ReadCellSlice } type;
    bool should_load;
    int children_mask;
  };
  struct Exploration {
    std::vector<Op> ops;
    std::string log;
    std::set<Cell::Hash> visited;
    std::vector<Ref<Cell>> visited_cells;
  };

  static Exploration explore(Ref<Cell> root, std::vector<Op> ops) {
    CellExplorer e(root);
    for (auto op : ops) {
      e.do_op(op);
    }
    return e.get_exploration();
  }

  template <class T>
  static Exploration random_explore(Ref<Cell> root, T &rnd) {
    CellExplorer e(root);
    int it = 0;
    int cnt = rnd.fast(1, 100);
    while (it++ < cnt && e.do_random_op(rnd)) {
    }
    return e.get_exploration();
  }

 private:
  CellExplorer(Ref<Cell> root) {
    if (root.not_null()) {
      cells_.push_back(std::move(root));
    }
  }

  std::vector<Ref<Cell>> cells_;
  Ref<CellSlice> cs_;
  std::vector<Op> ops_;
  std::set<Cell::Hash> visited_;
  std::map<Cell::Hash, Ref<Cell>> visited_cells_;
  td::StringBuilder log_{{}, true};

  void do_op(Op op) {
    ops_.push_back(op);
    log_op(op);
    switch (op.type) {
      case op.Pop: {
        CHECK(!cells_.empty());
        CHECK(cs_.is_null());
        auto cell = std::move(cells_.back());
        cells_.pop_back();
        visited_cells_.emplace(cell->get_hash(), cell);
        log_cell(cell);
        if (op.should_load) {
          log_loaded_cell(cell);
          visited_.insert(cell->get_hash());
          // It is ok to visit the same vertex multiple times
          cs_ = Ref<CellSlice>{true, NoVm(), std::move(cell)};
        }
        break;
      }
      case op.ReadCellSlice: {
        CHECK(cs_.not_null());
        log_cell_slice(cs_);
        for (unsigned i = 0; i < cs_->size_refs(); i++) {
          if ((op.children_mask >> i) % 2 != 0) {
            cells_.push_back(cs_->prefetch_ref(i));
          }
        }
        cs_ = {};
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  template <class T>
  bool do_random_op(T &rnd) {
    if (cs_.not_null()) {
      int children_mask = 0;
      if (cs_->size_refs() != 0 && rnd.fast(0, 3) != 0) {
        //children_mask = rnd.fast(1, (1 << cs_->size_refs()) - 1);
        children_mask = (1 << cs_->size_refs()) - 1;
      }
      do_op({Op::ReadCellSlice, false, children_mask});
      return true;
    }
    if (!cells_.empty()) {
      do_op({Op::Pop, rnd.fast(0, 30) != 0, 0});
      return true;
    }
    return false;
  }

  Exploration get_exploration() {
    std::vector<Ref<Cell>> visited_cells;
    for (auto &it : visited_cells_) {
      visited_cells.push_back(it.second);
    }
    return {std::move(ops_), log_.as_cslice().str(), std::move(visited_), std::move(visited_cells)};
  }

  void log_op(Op op) {
    switch (op.type) {
      case op.Pop:
        log_ << "pop" << (op.should_load ? " and load" : "") << "\n";
        break;
      case op.ReadCellSlice:
        log_ << "read slice " << op.children_mask << "\n";
        break;
      default:
        UNREACHABLE();
    }
  }
  void log_cell(const Ref<Cell> &cell) {
    log_ << cell->get_level_mask().get_mask() << " " << cell->get_hash() << "\n";
  }
  void log_loaded_cell(const Ref<Cell> &cell) {
    log_ << "depth: ";
    for (unsigned i = 0; i <= cell->get_level(); i++) {
      log_ << cell->get_depth(i) << " ";
    }
    log_ << "\n";
  }
  void log_cell_slice(const Ref<CellSlice> &cs) {
    log_ << cs->special_type() << " " << cs->size() << " " << cs->size_refs() << " "
         << td::bitstring::bits_to_hex(cs->data_bits(), cs->size()) << "\n";
  }
};

class RandomBagOfCells {
 public:
  template <class T>
  RandomBagOfCells(size_t size, T &rnd, bool with_prunned_branches, std::vector<Ref<Cell>> cells) {
    std::map<CellHash, int> depth;

    for (auto &cell : cells) {
      nodes_.emplace_back(cell, calc_depth(cell, depth));
    }

    for (size_t i = 0; i < size; i++) {
      add_random_cell(rnd, with_prunned_branches);
    }
  }

  Ref<Cell> get_root() {
    CHECK(!nodes_.empty());
    // Fix root to be zero level
    while (nodes_.back().cell->get_level() != 0) {
      nodes_.emplace_back(CellBuilder::create_merkle_proof(nodes_.back().cell), nodes_.back().merkle_depth + 1);
    }
    return nodes_.back().cell;
  }
  template <class T>
  std::vector<Ref<Cell>> get_random_roots(size_t size, T &rnd) {
    CHECK(!nodes_.empty());
    std::vector<Ref<Cell>> res(size);
    for (auto &c : res) {
      c = nodes_[rnd.fast(0, static_cast<int>(nodes_.size()) - 1)].cell;
    }
    return res;
  }

  size_t get_size() const {
    return nodes_.size();
  }

  template <class T>
  void add_random_cell(T &rnd, bool with_prunned_branches = true) {
    int cnt = 0;
    while (true) {
      CellBuilder cb;
      int next_cnt = rnd.fast(0, Cell::max_refs);
      int merkle_depth = 0;
      for (int j = 0; j < next_cnt && !nodes_.empty(); j++) {
        int to = rnd.fast(j == 0 && nodes_.size() > 3 ? (int)nodes_.size() - 3 : 0, (int)nodes_.size() - 1);
        merkle_depth = td::max(merkle_depth, nodes_.at(to).merkle_depth);
        cb.store_ref(nodes_[to].cell);
      }
      int size = rnd.fast(0, 4);
      for (int j = 0; j < size; j++) {
        cb.store_bytes(&"ab"[rnd.fast(0, 1)], 1);
      }
      if (rnd.fast(0, 4) == 4) {
        cb.store_bits(rnd.fast(0, 1) ? "\xff" : "\x55", rnd.fast(1, 7));
      }
      Ref<Cell> cell = cb.finalize();
      auto cell_level = cell->get_level();
      if (with_prunned_branches) {
        if (rnd.fast(0, 5) == 0 && cell_level + 1 < Cell::max_level) {
          cell = CellBuilder::create_pruned_branch(std::move(cell), cell_level + 1);
        }
        if (merkle_depth + 1 + cell->get_level() < Cell::max_level && rnd.fast(0, 10) == 0) {
          cell = CellBuilder::create_merkle_proof(std::move(cell));
          merkle_depth++;
        }
      }
      if (merkle_depth + cell->get_level() >= Cell::max_level) {
        cnt++;
        CHECK(cnt < 1000);
        continue;
      }
      CHECK(cell.not_null());
      nodes_.emplace_back(std::move(cell), merkle_depth);
      break;
    }
  }

 private:
  struct Node {
    Node() = default;
    Node(Ref<Cell> cell, int merkle_depth) : cell(std::move(cell)), merkle_depth(merkle_depth) {
    }
    Ref<Cell> cell;
    int merkle_depth;
  };
  std::vector<Node> nodes_;

  auto calc_depth(const Ref<Cell> &root, std::map<CellHash, int> &depth) -> int {
    auto it_flag = depth.emplace(root->get_hash(), 0);
    if (!it_flag.second) {
      return it_flag.first->second;
    }
    auto res = 0;
    CellSlice cs(NoVm(), root);
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      res = std::max(res, calc_depth(cs.prefetch_ref(i), depth));
    }
    if (cs.special_type() == Cell::SpecialType::MerkleProof) {
      res++;
    }
    depth[root->get_hash()] = res;
    return res;
  };
};

Ref<Cell> gen_random_cell(int size, td::Random::Xorshift128plus &rnd, bool with_prunned_branches = true,
                          std::vector<Ref<Cell>> cells = {}) {
  if (!cells.empty()) {
    td::random_shuffle(as_mutable_span(cells), rnd);
    cells.resize(cells.size() % rnd());
  }
  return RandomBagOfCells(size, rnd, with_prunned_branches, std::move(cells)).get_root();
}
std::vector<Ref<Cell>> gen_random_cells(int roots, int size, td::Random::Xorshift128plus &rnd,
                                        bool with_prunned_branches = true, std::vector<Ref<Cell>> cells = {}) {
  if (!cells.empty()) {
    td::random_shuffle(as_mutable_span(cells), rnd);
    cells.resize(cells.size() % rnd());
  }
  return RandomBagOfCells(size, rnd, with_prunned_branches, std::move(cells)).get_random_roots(roots, rnd);
}

TEST(Cell, MerkleProof) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    bool with_prunned_branches = true;
    auto cell = gen_random_cell(rnd.fast(1, 1000), rnd, with_prunned_branches);
    auto exploration = CellExplorer::random_explore(cell, rnd);

    auto usage_tree = std::make_shared<CellUsageTree>();
    auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
    auto exploration2 = CellExplorer::explore(usage_cell, exploration.ops);
    ASSERT_EQ(exploration.log, exploration2.log);

    auto is_prunned = [&](const Ref<Cell> &cell_to_check) {
      return exploration.visited.count(cell_to_check->get_hash()) == 0;
    };
    auto proof = MerkleProof::generate(cell, is_prunned);
    // CellBuilder::virtualize(proof, 1);
    //ASSERT_EQ(1u, proof->get_level());
    auto virtualized_proof = MerkleProof::virtualize(proof, 1);
    auto exploration3 = CellExplorer::explore(virtualized_proof, exploration.ops);
    ASSERT_EQ(exploration.log, exploration3.log);

    auto proof2 = MerkleProof::generate(cell, usage_tree.get());
    CHECK(proof2->get_depth() == proof->get_depth());
    auto virtualized_proof2 = MerkleProof::virtualize(proof2, 1);
    auto exploration4 = CellExplorer::explore(virtualized_proof2, exploration.ops);
    ASSERT_EQ(exploration.log, exploration4.log);
  }
};

TEST(Cell, MerkleProofCombine) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    bool with_prunned_branches = true;
    auto cell = gen_random_cell(rnd.fast(1, 1000), rnd, with_prunned_branches);
    auto exploration1 = CellExplorer::random_explore(cell, rnd);
    auto exploration2 = CellExplorer::random_explore(cell, rnd);

    Ref<Cell> proof1;
    {
      auto usage_tree = std::make_shared<CellUsageTree>();
      auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
      CellExplorer::explore(usage_cell, exploration1.ops);
      proof1 = MerkleProof::generate(cell, usage_tree.get());

      auto virtualized_proof = MerkleProof::virtualize(proof1, 1);
      auto exploration = CellExplorer::explore(virtualized_proof, exploration1.ops);
      ASSERT_EQ(exploration.log, exploration1.log);
    }

    Ref<Cell> proof2;
    {
      auto usage_tree = std::make_shared<CellUsageTree>();
      auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
      CellExplorer::explore(usage_cell, exploration2.ops);
      proof2 = MerkleProof::generate(cell, usage_tree.get());

      auto virtualized_proof = MerkleProof::virtualize(proof2, 1);
      auto exploration = CellExplorer::explore(virtualized_proof, exploration2.ops);
      ASSERT_EQ(exploration.log, exploration2.log);
    }

    Ref<Cell> proof12;
    {
      auto usage_tree = std::make_shared<CellUsageTree>();
      auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
      CellExplorer::explore(usage_cell, exploration1.ops);
      CellExplorer::explore(usage_cell, exploration2.ops);
      proof12 = MerkleProof::generate(cell, usage_tree.get());

      auto virtualized_proof = MerkleProof::virtualize(proof12, 1);
      auto exploration_a = CellExplorer::explore(virtualized_proof, exploration1.ops);
      auto exploration_b = CellExplorer::explore(virtualized_proof, exploration2.ops);
      ASSERT_EQ(exploration_a.log, exploration1.log);
      ASSERT_EQ(exploration_b.log, exploration2.log);
    }

    {
      auto check = [&](auto proof_union) {
        auto virtualized_proof = MerkleProof::virtualize(proof_union, 1);
        auto exploration_a = CellExplorer::explore(virtualized_proof, exploration1.ops);
        auto exploration_b = CellExplorer::explore(virtualized_proof, exploration2.ops);
        ASSERT_EQ(exploration_a.log, exploration1.log);
        ASSERT_EQ(exploration_b.log, exploration2.log);
      };
      auto proof_union = MerkleProof::combine(proof1, proof2);
      ASSERT_EQ(proof_union->get_hash(), proof12->get_hash());
      check(proof_union);

      auto proof_union_fast = MerkleProof::combine_fast(proof1, proof2);
      check(proof_union_fast);
    }
    {
      cell = MerkleProof::virtualize(proof12, 1);

      auto usage_tree = std::make_shared<CellUsageTree>();
      auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
      CellExplorer::explore(usage_cell, exploration1.ops);
      auto proof = MerkleProof::generate(cell, usage_tree.get());

      auto virtualized_proof = MerkleProof::virtualize(proof, 2);
      auto exploration = CellExplorer::explore(virtualized_proof, exploration1.ops);
      ASSERT_EQ(exploration.log, exploration1.log);
      if (proof->get_hash() != proof1->get_hash()) {
        CellSlice(NoVm(), proof12).print_rec(std::cerr);
        CellSlice(NoVm(), proof).print_rec(std::cerr);
        CellSlice(NoVm(), proof1).print_rec(std::cerr);
        LOG(ERROR) << proof->get_level() << " " << proof->get_hash().to_hex();
        LOG(ERROR) << proof->get_level() << " " << proof1->get_hash().to_hex();
        LOG(FATAL) << "?";
      }
    }
  }
};

int X = 20;
Ref<Cell> gen_random_cell(int size, Ref<Cell> from, td::Random::Xorshift128plus &rnd,
                          bool with_prunned_branches = true) {
  auto exploration = CellExplorer::random_explore(from, rnd);
  return gen_random_cell(size, rnd, with_prunned_branches, std::move(exploration.visited_cells));
}
auto gen_merkle_update(Ref<Cell> cell, td::Random::Xorshift128plus &rnd, bool with_prunned_branches) {
  auto usage_tree = std::make_shared<CellUsageTree>();
  auto usage_cell = UsageCell::create(cell, usage_tree->root_ptr());
  auto new_cell = gen_random_cell(rnd.fast(1, X), usage_cell, rnd, with_prunned_branches);
  auto update = MerkleUpdate::generate(cell, new_cell, usage_tree.get());
  return std::make_tuple(new_cell, update, usage_tree);
};

void check_merkle_update(Ref<Cell> A, Ref<Cell> B, Ref<Cell> AB) {
  CHECK(AB.not_null());
  CHECK(A.not_null());
  MerkleUpdate::may_apply(A, AB).ensure();
  MerkleUpdate::validate(AB).ensure();
  auto got_B = MerkleUpdate::apply(A, AB);
  ASSERT_EQ(B->get_hash(), got_B->get_hash());
};

TEST(Cell, MerkleUpdate) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    bool with_prunned_branches = true;
    auto A = gen_random_cell(rnd.fast(1, 1000), rnd, with_prunned_branches);

    Ref<Cell> B;
    Ref<Cell> AB;
    std::tie(B, AB, std::ignore) = gen_merkle_update(A, rnd, with_prunned_branches);
    check_merkle_update(A, B, AB);
  }
};

TEST(Cell, MerkleUpdateCombine) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    bool with_prunned_branches = true;
    auto A = gen_random_cell(rnd.fast(1, X), rnd, with_prunned_branches);

    Ref<Cell> B;
    Ref<Cell> AB;
    std::tie(B, AB, std::ignore) = gen_merkle_update(A, rnd, with_prunned_branches);
    check_merkle_update(A, B, AB);

    Ref<Cell> C;
    Ref<Cell> BC;
    std::tie(C, BC, std::ignore) = gen_merkle_update(B, rnd, with_prunned_branches);
    check_merkle_update(B, C, BC);

    check_merkle_update(A, C, MerkleUpdate::combine(AB, BC));
  }
};

class BenchCellBuilder : public td::Benchmark {
 public:
  std::string get_description() const override {
    return "BenchCellBuilder";
  }

  void run(int n) override {
    td::Random::Xorshift128plus rnd(123);
    std::string data(128, ' ');
    for (auto &c : data) {
      c = static_cast<char>(rnd());
    }

    for (int i = 0; i < n; i++) {
      CellBuilder cb;
      cb.store_bytes(data.data(), rnd() & 127);
      cb.finalize(false);
    }
  }
};
TEST(TonDb, BenchCellBuilder) {
  td::bench(BenchCellBuilder());
}
class BenchCellBuilder2 : public td::Benchmark {
 public:
  std::string get_description() const override {
    return "BenchCellBuilder";
  }

  void run(int n) override {
    td::Random::Xorshift128plus rnd(123);

    for (int i = 0; i < n; i++) {
      gen_random_cell(rnd.fast(1, 1000), rnd);
    }
  }
};
TEST(TonDb, BenchCellBuilder2) {
  td::bench(BenchCellBuilder2());
}
class BenchCellBuilder3 : public td::Benchmark {
 public:
  std::string get_description() const override {
    return "BenchCellBuilder";
  }

  void run(int n) override {
    td::Random::Xorshift128plus rnd(123);

    for (int i = 0; i < n; i++) {
      auto cell = gen_random_cell(rnd.fast(1, 1000), rnd, false);
      auto cell_hash = cell->get_hash().to_hex();

      int mode = get_random_serialization_mode(rnd);

      auto serialized = serialize_boc(std::move(cell), mode);
      CHECK(serialized.size() != 0);

      auto loaded_cell = deserialize_boc(serialized);
      ASSERT_EQ(cell_hash, loaded_cell->get_hash().to_hex());

      auto new_serialized = serialize_boc(std::move(loaded_cell), mode);
      ASSERT_EQ(serialized, new_serialized);
    }
  }
};
TEST(TonDb, BenchCellBuilder3) {
  td::bench(BenchCellBuilder3());
}

TEST(TonDb, BocFuzz) {
  vm::std_boc_deserialize(td::base64_decode("te6ccgEBAQEAAgAoAAA=").move_as_ok()).ensure_error();
  vm::std_boc_deserialize(td::base64_decode("te6ccgQBQQdQAAAAAAEAte6ccgQBB1BBAAAAAAEAAAAAAP/"
                                            "wAACJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJicmJiYmJiYmJiYmJiQ0NDQ0NDQ0NDQ0NDQ0ND"
                                            "Q0NiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiQAA//AAAO4=")
                              .move_as_ok());
  vm::std_boc_deserialize(td::base64_decode("SEkh/w==").move_as_ok()).ensure_error();
  vm::std_boc_deserialize(
      td::base64_decode(
          "te6ccqwBMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMAKCEAAAAgAQ==")
          .move_as_ok())
      .ensure_error();
}
void test_parse_prefix(td::Slice boc) {
  for (size_t i = 0; i <= boc.size(); i++) {
    auto prefix = boc.substr(0, i);
    vm::BagOfCells::Info info;
    auto res = info.parse_serialized_header(prefix);
    if (res > 0) {
      break;
    }
    CHECK(res != 0);
    CHECK(-res > (int)i);
  }
}

TEST(TonDb, Boc) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    auto cell = gen_random_cell(rnd.fast(1, 1000), rnd);
    auto cell_hash = cell->get_hash();
    auto mode = get_random_serialization_mode(rnd);

    auto serialized = serialize_boc(std::move(cell), mode);
    CHECK(serialized.size() != 0);

    test_parse_prefix(serialized);

    auto loaded_cell = deserialize_boc(serialized);
    ASSERT_EQ(cell_hash, loaded_cell->get_hash());

    auto new_serialized = serialize_boc(std::move(loaded_cell), mode);
    ASSERT_EQ(serialized, new_serialized);
  }
};
TEST(TonDb, BocMultipleRoots) {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 200; t++) {
    auto cells = gen_random_cells(rnd.fast(1, 10), rnd.fast(1, 1000), rnd);
    std::vector<Cell::Hash> cell_hashes;
    for (size_t i = 0; i < cells.size(); i++) {
      cell_hashes.push_back(cells[i]->get_hash());
    }
    auto mode = get_random_serialization_mode(rnd);
    auto serialized = serialize_boc(cells, mode);
    CHECK(serialized.size() != 0);

    auto loaded_cells = deserialize_boc_multiple(serialized);
    ASSERT_EQ(cell_hashes.size(), loaded_cells.size());

    for (size_t i = 0; i < cell_hashes.size(); i++) {
      ASSERT_EQ(cell_hashes[i], loaded_cells[i]->get_hash());
    }
    auto new_serialized = serialize_boc(std::move(loaded_cells), mode);
    ASSERT_EQ(serialized, new_serialized);
  }
};

TEST(TonDb, InMemoryDynamicBocSimple) {
  auto counter = [] { return td::NamedThreadSafeCounter::get_default().get_counter("DataCell").sum(); };
  auto before = counter();
  SCOPE_EXIT {
    LOG_CHECK(before == counter()) << before << " vs " << counter();
  };
  td::Random::Xorshift128plus rnd{123};
  auto kv = std::make_shared<td::MemoryKeyValue>();
  CellStorer storer(*kv);

  auto boc = DynamicBagOfCellsDb::create_in_memory(kv.get(), {});

  auto empty_cell = vm::CellBuilder().finalize();
  boc->inc(empty_cell);
  boc->prepare_commit().ensure();
  boc->commit(storer).ensure();
  auto got_empty_cell = boc->load_cell(empty_cell->get_hash().as_slice()).move_as_ok();
  ASSERT_EQ(empty_cell->get_hash(), got_empty_cell->get_hash());

  boc->dec(empty_cell);

  auto one_ref_cell = vm::CellBuilder().store_ref(empty_cell).finalize();
  boc->inc(one_ref_cell);
  boc->prepare_commit().ensure();
  boc->commit(storer).ensure();
  auto got_one_ref_cell = boc->load_cell(one_ref_cell->get_hash().as_slice()).move_as_ok();
  ASSERT_EQ(one_ref_cell->get_hash(), got_one_ref_cell->get_hash());
  boc = DynamicBagOfCellsDb::create_in_memory(kv.get(), {});

  auto random_ref_cell = gen_random_cell(3, rnd);
  boc->inc(random_ref_cell);
  boc->prepare_commit().ensure();
  boc->commit(storer).ensure();
  auto got_random_ref_cell = boc->load_cell(random_ref_cell->get_hash().as_slice()).move_as_ok();
  ASSERT_EQ(random_ref_cell->get_hash(), got_random_ref_cell->get_hash());
  boc = DynamicBagOfCellsDb::create_in_memory(kv.get(), {});
}

int VERBOSITY_NAME(boc) = VERBOSITY_NAME(DEBUG) + 10;

struct CellMerger : td::Merger {
  void merge_value_and_update(std::string &value, td::Slice update) override {
    return CellStorer::merge_value_and_refcnt_diff(value, update);
  }
  void merge_update_and_update(std::string &left_update, td::Slice right_update) override {
    LOG(ERROR) << "update_and_update";
    UNREACHABLE();
    return CellStorer::merge_refcnt_diffs(left_update, right_update);
  }
};
struct CompactionFilterEraseEmptyValues : public rocksdb::CompactionFilter {
  bool Filter(int level, const rocksdb::Slice & /*key*/, const rocksdb::Slice &existing_value, std::string *new_value,
              bool *value_changed) const override {
    return existing_value.empty();
  }
  bool FilterMergeOperand(int, const rocksdb::Slice & /*key*/, const rocksdb::Slice &operand) const override {
    return operand.empty();
  }

  // Name of the compaction filter
  const char *Name() const override {
    return "CompactionFilterEraseEmptyValues";
  }
};
auto to_td(rocksdb::Slice value) -> td::Slice {
  return td::Slice(value.data(), value.size());
}

struct MergeOperatorAddCellRefcnt : public rocksdb::MergeOperator {
  const char *Name() const override {
    return "MergeOperatorAddCellRefcnt";
  }
  bool FullMergeV2(const MergeOperationInput &merge_in, MergeOperationOutput *merge_out) const override {
    CHECK(merge_in.existing_value);
    auto &value = *merge_in.existing_value;
    CHECK(merge_in.operand_list.size() >= 1);
    td::Slice diff;
    std::string diff_buf;
    if (merge_in.operand_list.size() == 1) {
      diff = to_td(merge_in.operand_list[0]);
    } else {
      diff_buf = merge_in.operand_list[0].ToString();
      for (size_t i = 1; i < merge_in.operand_list.size(); ++i) {
        CellStorer::merge_refcnt_diffs(diff_buf, to_td(merge_in.operand_list[i]));
      }
      diff = diff_buf;
    }

    merge_out->new_value = value.ToString();
    CellStorer::merge_value_and_refcnt_diff(merge_out->new_value, diff);
    return true;
  }
  bool PartialMerge(const rocksdb::Slice & /*key*/, const rocksdb::Slice &left, const rocksdb::Slice &right,
                    std::string *new_value, rocksdb::Logger *logger) const override {
    *new_value = left.ToString();
    CellStorer::merge_refcnt_diffs(*new_value, to_td(right));
    return true;
  }
};

struct DB {
  std::unique_ptr<DynamicBagOfCellsDb> dboc;
  std::shared_ptr<td::KeyValue> kv;
  void reset_loader() {
    dboc->set_loader(std::make_unique<CellLoader>(kv->snapshot()));
  }
};
struct BocOptions {
  using AsyncExecutor = DynamicBagOfCellsDb::AsyncExecutor;

  using CreateInMemoryOptions = DynamicBagOfCellsDb::CreateInMemoryOptions;
  using CreateV1Options = DynamicBagOfCellsDb::CreateV1Options;
  using CreateV2Options = DynamicBagOfCellsDb::CreateV2Options;

  std::shared_ptr<AsyncExecutor> async_executor;
  struct KvOptions {
    enum KvType { InMemory, RocksDb } kv_type{InMemory};
    bool experimental{false};
    bool no_transactions{false};
    size_t cache_size{0};
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const KvOptions &kv_options) {
      if (kv_options.kv_type == KvType::InMemory) {
        return sb << "InMemory{}";
      }
      return sb << "RockDb{cache_size=" << kv_options.cache_size << ", no_transactions=" << kv_options.no_transactions
                << ", experimental=" << kv_options.experimental << "}";
    }
  };
  KvOptions kv_options;
  std::variant<CreateV1Options, CreateV2Options, CreateInMemoryOptions> options;
  std::pair<int, int> compress_depth_range{0, 0};
  td::uint64 seed{123};
  td::Random::Xorshift128plus rnd{123};

  std::shared_ptr<KeyValue> create_kv(std::shared_ptr<KeyValue> old_key_value, bool no_reads = false) {
    if (kv_options.kv_type == KvOptions::InMemory) {
      if (old_key_value) {
        return old_key_value;
      }
      return std::make_shared<td::MemoryKeyValue>(std::make_shared<CellMerger>());
    } else if (kv_options.kv_type == KvOptions::RocksDb) {
      auto merge_operator = std::make_shared<MergeOperatorAddCellRefcnt>();
      static const CompactionFilterEraseEmptyValues compaction_filter;
      CHECK(!old_key_value || old_key_value.use_count() == 1);
      std::string db_path = "test_celldb";
      if (old_key_value) {
        //LOG(ERROR) << "Reload rocksdb";
        old_key_value.reset();
      } else {
        //LOG(ERROR) << "New rocksdb";
        td::RocksDb::destroy(db_path).ensure();
      }
      auto db_options = td::RocksDbOptions{
          .block_cache = {},
          .merge_operator = merge_operator,
          .compaction_filter = &compaction_filter,
          .experimental = kv_options.experimental,
          .no_reads = no_reads,
          .no_transactions = kv_options.no_transactions,
          .use_direct_reads = true,
          .no_block_cache = true,
      };
      if (kv_options.cache_size != 0) {
        db_options.no_block_cache = false;
        db_options.block_cache = rocksdb::NewLRUCache(kv_options.cache_size);
      }
      return std::make_shared<td::RocksDb>(td::RocksDb::open(db_path, std::move(db_options)).move_as_ok());
    } else {
      UNREACHABLE();
    }
  }
  void check_kv_is_empty(KeyValue &kv) {
    if (kv_options.kv_type == KvOptions::InMemory) {
      ASSERT_EQ(0u, kv.count("").move_as_ok());
      return;
    }

    size_t non_empty_values = 0;
    kv.for_each([&](auto key, auto value) {
      non_empty_values += !value.empty();
      return td::Status::OK();
    });
    if (non_empty_values != 0) {
      kv.for_each([&](auto key, auto value) {
        LOG(ERROR) << "Key: " << td::hex_encode(key) << " Value: " << td::hex_encode(value);
        std::string x;
        LOG(ERROR) << int(kv.get(key, x).move_as_ok());
        return td::Status::OK();
      });
    }
    ASSERT_EQ(0u, non_empty_values);
  }

  [[nodiscard]] auto create_db(DB db, std::optional<td::int64> o_root_n) {
    auto old_boc = std::move(db.dboc);
    auto old_kv = std::move(db.kv);
    old_boc.reset();
    using ResT = DB;
    auto res = std::visit(td::overloaded(
                              [&](CreateV1Options &) -> ResT {
                                auto new_kv = create_kv(std::move(old_kv));
                                auto res = DynamicBagOfCellsDb::create();
                                res->set_loader(std::make_unique<CellLoader>(new_kv->snapshot()));
                                return DB{.dboc = std::move(res), .kv = std::move(new_kv)};
                              },
                              [&](CreateV2Options &options) -> ResT {
                                auto new_kv = create_kv(std::move(old_kv));
                                auto res = DynamicBagOfCellsDb::create_v2(options);
                                res->set_loader(std::make_unique<CellLoader>(new_kv->snapshot()));
                                return DB{.dboc = std::move(res), .kv = std::move(new_kv)};
                              },
                              [&](CreateInMemoryOptions &options) -> ResT {
                                auto read_kv = create_kv(std::move(old_kv), false);
                                auto res = DynamicBagOfCellsDb::create_in_memory(read_kv.get(), options);
                                auto new_kv = create_kv(std::move(read_kv), true);
                                res->set_loader(std::make_unique<CellLoader>(new_kv->snapshot()));
                                auto stats = res->get_stats().move_as_ok();
                                if (o_root_n) {
                                  ASSERT_EQ(*o_root_n, stats.roots_total_count);
                                }
                                VLOG(boc) << "reset roots_n=" << stats.roots_total_count
                                          << " cells_n=" << stats.cells_total_count;
                                return DB{.dboc = std::move(res), .kv = std::move(new_kv)};
                              }),
                          options);
    if (compress_depth_range.second != 0) {
      res.dboc->set_celldb_compress_depth(rnd.fast(compress_depth_range.first, compress_depth_range.second));
    }
    return res;
  };
  void prepare_commit(DynamicBagOfCellsDb &dboc) {
    td::PerfWarningTimer warning_timer("test_db_prepare_commit");
    if (async_executor) {
      std::latch latch(1);
      td::Result<td::Unit> res;
      async_executor->execute_sync([&] {
        dboc.prepare_commit_async(async_executor, [&](auto r) {
          res = std::move(r);
          latch.count_down();
        });
      });
      latch.wait();
      async_executor->execute_sync([&] {});
      res.ensure();
    } else {
      dboc.prepare_commit();
    }
  }
  enum CacheAction { ResetCache, KeepCache };
  void write_commit(DynamicBagOfCellsDb &dboc, std::shared_ptr<KeyValue> kv, CacheAction action) {
    td::PerfWarningTimer warning_timer("test_db_write_commit");
    kv->begin_write_batch().ensure();
    CellStorer cell_storer(*kv);
    {
      td::PerfWarningTimer timer("test_db_commit");
      dboc.commit(cell_storer).ensure();
    }
    {
      td::PerfWarningTimer timer("test_db_commit_write_batch");
      kv->commit_write_batch().ensure();
    }
    switch (action) {
      case ResetCache: {
        td::PerfWarningTimer timer("test_db_reset_cache");
        dboc.set_loader(std::make_unique<CellLoader>(kv->snapshot()));
        break;
      }
      case KeepCache:
        break;
    }
  }

  void commit(DB &db, CacheAction action = ResetCache) {
    prepare_commit(*db.dboc);
    write_commit(*db.dboc, db.kv, action);
  }

  std::string description() const {
    td::StringBuilder sb;

    sb << "DBOC(type=";
    std::visit(td::overloaded([&](const CreateV1Options &) { sb << "V1"; },
                              [&](const CreateV2Options &options) {
                                sb << "V2(concurrency=" << options.extra_threads + 1;
                                if (options.executor) {
                                  sb << ", executor=" << options.executor->describe();
                                } else {
                                  sb << ", executor=threads";
                                }
                                sb << ")";
                              },
                              [&](const CreateInMemoryOptions &options) {
                                sb << "InMemory(use_arena=" << options.use_arena
                                   << ", less_memory=" << options.use_less_memory_during_creation << ")";
                              }),
               options);
    sb << kv_options;
    if (async_executor) {
      sb << ", executor=" << async_executor->describe();
    }
    if (compress_depth_range.second != 0) {
      sb << ", compress_depth=[" << compress_depth_range.first << ";" << compress_depth_range.second << "]";
    }
    sb << ")";

    return sb.as_cslice().str();
  }
};

template <class F>
void with_all_boc_options(F &&f, size_t tests_n, bool single_thread = false) {
  LOG(INFO) << "Test dynamic boc";
  auto counter = [] { return td::NamedThreadSafeCounter::get_default().get_counter("DataCell").sum(); };
  std::map<std::string, std::vector<std::pair<td::int64, std::string>>> benches;
  auto run = [&](BocOptions options) {
    auto description = options.description();
    LOG(INFO) << "Running " << description;
    auto start = td::Timestamp::now();
    DynamicBagOfCellsDb::Stats stats;
    auto o_in_memory = std::get_if<DynamicBagOfCellsDb::CreateInMemoryOptions>(&options.options);
    for (td::uint32 i = 0; i < tests_n; i++) {
      auto before = counter();

      options.seed = i == 0 ? 123 : i;
      options.rnd = td::Random::Xorshift128plus{options.seed};
      auto stats_diff = f(options);
      stats.apply_diff(stats_diff);

      auto after = counter();
      LOG_CHECK((o_in_memory && o_in_memory->use_arena) || before == after) << before << " vs " << after;
    }
    LOG(INFO) << "\ttook " << td::Timestamp::now().at() - start.at() << " seconds";
    LOG(INFO) << stats;
    for (auto &[key, value] : stats.named_stats.stats_int) {
      if (td::begins_with(key, "bench_")) {
        benches[key].emplace_back(value, description);
      }
    }
  };

  // NB: use .experimental to play with different RocksDb parameters
  // Note, that new benchmark are necessary to fully understand the effect of different RocksDb options
  std::vector kv_options_list = {
      // BocOptions::KvOptions{.kv_type = BocOptions::KvOptions::InMemory},
      // BocOptions::KvOptions{.kv_type = BocOptions::KvOptions::RocksDb, .experimental = false, .cache_size = 0},
      BocOptions::KvOptions{
          .kv_type = BocOptions::KvOptions::RocksDb, .experimental = false, .cache_size = size_t{128 << 20}},
  };
  std::vector<std::pair<int, int>> compress_depth_ranges = {{0, 5}, {5, 5}, {0, 0}};
  std::vector<bool> has_executor_options = {false, true};
  for (auto compress_depth_range : compress_depth_ranges) {
    for (auto kv_options : kv_options_list) {
      for (bool has_executor : has_executor_options) {
        std::shared_ptr<DynamicBagOfCellsDb::AsyncExecutor> executor;
        if (has_executor) {
          executor = std::make_shared<ActorExecutor>(
              4);  // 4 - to compare V1 and V2, because V1 has parallel_load = 4 by default
        }
        // V2 - 4 threads
        run({
            .async_executor = executor,
            .kv_options = kv_options,
            .options =
                DynamicBagOfCellsDb::CreateV2Options{.extra_threads = 3, .executor = executor, .cache_ttl_max = 5},
            .compress_depth_range = compress_depth_range,
        });

        // V1
        run({.async_executor = executor,
             .kv_options = kv_options,
             .options = DynamicBagOfCellsDb::CreateV1Options{},
             .compress_depth_range = compress_depth_range});

        // V2 - one thread
        run({.async_executor = executor,
             .kv_options = kv_options,
             .options =
                 DynamicBagOfCellsDb::CreateV2Options{.extra_threads = 0, .executor = executor, .cache_ttl_max = 5},
             .compress_depth_range = compress_depth_range});

        // InMemory
        if (compress_depth_range.second == 0) {
          for (auto use_arena : {false, true}) {
            for (auto less_memory : {false, true}) {
              run({.async_executor = executor,
                   .kv_options = kv_options,
                   .options =
                       DynamicBagOfCellsDb::CreateInMemoryOptions{.extra_threads = std::thread::hardware_concurrency(),
                                                                  .verbose = false,
                                                                  .use_arena = use_arena,
                                                                  .use_less_memory_during_creation = less_memory}});
            }
          }
        }
      }
    }
  }

  for (auto &[name, v] : benches) {
    std::sort(v.begin(), v.end());
    LOG(INFO) << "Bench " << name;
    for (auto &[t, name] : v) {
      LOG(INFO) << "\t" << name << " " << double(t) / 1000 << "s";
    }
  }
}

DynamicBagOfCellsDb::Stats test_dynamic_boc(BocOptions options) {
  DynamicBagOfCellsDb::Stats stats;
  auto &rnd = options.rnd;
  std::string old_root_hash;
  std::string old_root_serialization;
  DB db;
  auto reload_db = [&]() {
    auto roots_n = old_root_hash.empty() ? 0 : 1;
    db = options.create_db(std::move(db), roots_n);
  };
  reload_db();
  for (int t = 1000; t >= 0; t--) {
    if (rnd() % 10 == 0) {
      reload_db();
    }
    db.dboc->load_cell(vm::CellHash{}.as_slice()).ensure_error();

    db.reset_loader();
    Ref<Cell> old_root;
    if (!old_root_hash.empty()) {
      old_root = db.dboc->load_cell(old_root_hash).move_as_ok();
      auto serialization = serialize_boc(old_root);
      ASSERT_EQ(old_root_serialization, serialization);
    }

    auto cell = gen_random_cell(rnd.fast(1, 1000), rnd);
    old_root_hash = cell->get_hash().as_slice().str();
    old_root_serialization = serialize_boc(cell);

    // Check that DynamicBagOfCells properly loads cells
    cell = vm::StaticBagOfCellsDbLazy::create(td::BufferSliceBlobView::create(td::BufferSlice(old_root_serialization)))
               .move_as_ok()
               ->get_root_cell(0)
               .move_as_ok();

    db.dboc->dec(old_root);
    if (t != 0) {
      db.dboc->inc(cell);
    }
    options.commit(db, BocOptions::ResetCache);
  }
  options.check_kv_is_empty(*db.kv);

  stats.named_stats.apply_diff(db.kv->get_usage_stats().to_named_stats());
  return stats;
}

TEST(TonDb, DynamicBoc) {
  with_all_boc_options(test_dynamic_boc, 1);
};

DynamicBagOfCellsDb::Stats test_dynamic_boc2(BocOptions options) {
  auto &rnd = options.rnd;
  DynamicBagOfCellsDb::Stats stats;

  int total_roots = rnd.fast(1, !rnd.fast(0, 30) * 100 + 10);
  int max_roots = rnd.fast(1, 20);
  int max_cells = 20;

  // VERBOSITY_NAME(boc) = 1;
  // LOG(WARNING) << "====================================================\n\n";
  // max_roots = 2;
  // total_roots = 2;
  // max_cells = 2;

  auto meta_key = [](size_t i) { return PSTRING() << "meta." << i; };
  std::array<std::string, 8> meta;

  int last_commit_at = 0;
  int first_root_id = 0;
  int last_root_id = 0;
  DB db;
  auto reload_db = [&](td::int64 root_n) { db = options.create_db(std::move(db), root_n); };
  reload_db(0);

  auto counter = [] { return td::NamedThreadSafeCounter::get_default().get_counter("DataCell").sum(); };
  auto before = counter();
  SCOPE_EXIT {
    bool skip_check = false;
    if (std::holds_alternative<DynamicBagOfCellsDb::CreateInMemoryOptions>(options.options) &&
        std::get<DynamicBagOfCellsDb::CreateInMemoryOptions>(options.options).use_arena) {
      skip_check = true;
    }
    LOG_IF(FATAL, !(skip_check || before == counter())) << before << " vs " << counter();
  };

  std::vector<Ref<Cell>> roots(max_roots);
  std::vector<std::string> root_hashes(max_roots);
  auto add_root = [&](Ref<Cell> root) {
    db.dboc->inc(root);
    root_hashes[last_root_id % max_roots] = (root->get_hash().as_slice().str());
    roots[last_root_id % max_roots] = root;
    last_root_id++;
  };

  auto get_root = [&](int root_id) -> Ref<Cell> {
    VLOG(boc) << "  from older root #" << root_id;
    auto from_root = roots[root_id % max_roots];
    if (from_root.is_null()) {
      VLOG(boc) << "  from db";
      auto from_root_hash = root_hashes[root_id % max_roots];
      if (rnd() % 2 == 0) {
        from_root = db.dboc->load_root(from_root_hash).move_as_ok();
      } else {
        from_root = db.dboc->load_cell(from_root_hash).move_as_ok();
      }
    } else {
      VLOG(boc) << "FROM MEMORY";
    }
    return from_root;
  };
  std::map<CellHash, int> root_cnt;
  auto new_root = [&] {
    if (last_root_id == total_roots) {
      return;
    }
    if (last_root_id - first_root_id >= max_roots) {
      return;
    }
    VLOG(boc) << "add root";
    Ref<Cell> from_root;
    if (first_root_id != last_root_id) {
      from_root = get_root(rnd.fast(first_root_id, last_root_id - 1));
    }
    VLOG(boc) << "  ...";
    auto new_root_cell = gen_random_cell(rnd.fast(1, max_cells), from_root, rnd);
    root_cnt[new_root_cell->get_hash()]++;
    add_root(std::move(new_root_cell));
    VLOG(boc) << "  OK";
  };

  td::UsageStats commit_stats{};
  auto commit = [&](bool finish = false) {
    for (size_t i = 0; i < meta.size(); i++) {
      std::string value;
      auto status = db.dboc->meta_get(meta_key(i), value).move_as_ok();
      if (status == KeyValue::GetStatus::Ok) {
        ASSERT_EQ(value, meta[i]);
        ASSERT_TRUE(!meta[i].empty());
      } else {
        ASSERT_TRUE(meta[i].empty());
      }

      if (meta[i].empty()) {
        if (!finish && rnd() % 2 == 0) {
          meta[i] = td::to_string(rnd());
          db.dboc->meta_set(meta_key(i), meta[i]);
          VLOG(boc) << "meta set " << meta_key(i) << " " << meta[i];
        }
      } else {
        auto f = finish ? 1 : rnd() % 3;
        if (f == 0) {
          meta[i] = td::to_string(rnd());
          db.dboc->meta_set(meta_key(i), meta[i]);
          VLOG(boc) << "meta set " << meta_key(i) << " " << meta[i];
        } else if (f == 1) {
          meta[i] = "";
          db.dboc->meta_erase(meta_key(i));
          VLOG(boc) << "meta erase " << meta_key(i);
        }
      }
    }

    VLOG(boc) << "before commit cells_in_db=" << db.kv->count("");
    //rnd.fast(0, 1);
    auto stats_before = db.kv->get_usage_stats();
    options.commit(db, BocOptions::ResetCache);
    auto stats_after = db.kv->get_usage_stats();
    commit_stats = commit_stats + stats_after - stats_before;
    VLOG(boc) << "after commit cells_in_db=" << db.kv->count("");

    // db.reset_loader();
    for (int i = last_commit_at; i < last_root_id; i++) {
      roots[i % max_roots].clear();
    }
    last_commit_at = last_root_id;
  };
  auto reset = [&](bool force_full = false) {
    VLOG(boc) << "reset";
    commit();
    if (rnd() % 3 == 0 || force_full) {
      // very slow for rocksdb
      auto r_stats = db.dboc->get_stats();
      if (r_stats.is_ok()) {
        stats.apply_diff(r_stats.ok());
      }
      reload_db(root_cnt.size());
    }
  };

  auto delete_root = [&] {
    VLOG(boc) << "Delete root";
    if (first_root_id == last_root_id) {
      return;
    }
    auto old_root = get_root(first_root_id);
    auto it = root_cnt.find(old_root->get_hash());
    it->second--;
    CHECK(it->second >= 0);
    if (it->second == 0) {
      root_cnt.erase(it);
    }

    db.dboc->dec(std::move(old_root));
    first_root_id++;
    VLOG(boc) << "  OK";
  };

  td::RandomSteps steps({{new_root, 10}, {delete_root, 9}, {commit, 2}, {reset, 1}});
  while (first_root_id != total_roots) {
    VLOG(boc) << first_root_id << " " << last_root_id;  // << " " << db.kv->count("").ok();
    steps.step(rnd);
  }
  commit(true);
  options.check_kv_is_empty(*db.kv);

  // auto stats = kv->get_usage_stats();
  // LOG(ERROR) << "total:  " << stats;
  reset(true);
  stats.named_stats.apply_diff(db.kv->get_usage_stats().to_named_stats());
  return stats;
}

TEST(TonDb, DynamicBoc2) {
  with_all_boc_options(test_dynamic_boc2, 50);
}

template <class BocDeserializerT>
td::Status test_boc_deserializer(std::vector<Ref<Cell>> cells, int mode) {
  auto total_data_cells_before = vm::DataCell::get_total_data_cells();
  SCOPE_EXIT {
    auto total_data_cells_after = vm::DataCell::get_total_data_cells();
    ASSERT_EQ(total_data_cells_before, total_data_cells_after);
  };
  auto serialized = serialize_boc(cells, mode);
  CHECK(serialized.size() != 0);

  TRY_RESULT(boc_deserializer, BocDeserializerT::create(serialized));
  TRY_RESULT(root_count, boc_deserializer->get_root_count());
  ASSERT_EQ(cells.size(), root_count);

  std::vector<Ref<Cell>> loaded_cells;
  for (size_t root_i = 0; root_i < root_count; root_i++) {
    TRY_RESULT(loaded_cell, boc_deserializer->get_root_cell(root_i));
    auto cell = cells[root_i];
    ASSERT_EQ(cell->get_level(), loaded_cell->get_level());
    for (int i = 0; i <= (int)cell->get_level(); i++) {
      ASSERT_EQ(cell->get_hash(i), loaded_cell->get_hash(i));
    }
    ASSERT_EQ(loaded_cell->get_hash(cell->get_level()), loaded_cell->get_hash());
    loaded_cells.push_back(std::move(loaded_cell));
  }

  auto new_serialized = serialize_boc(std::move(loaded_cells), mode);
  ASSERT_EQ(serialized, new_serialized);
  return td::Status::OK();
}

template <class BocDeserializerT>
td::Status test_boc_deserializer_threads(Ref<Cell> cell, int mode, td::Random::Xorshift128plus &rnd,
                                         size_t threads_n = 4) {
  auto serialized = serialize_boc(cell, mode);
  CHECK(serialized.size() != 0);
  std::vector<CellExplorer::Exploration> explorations;
  for (size_t i = 0; i < threads_n; i++) {
    explorations.push_back(CellExplorer::random_explore(cell, rnd));
  }
  TRY_RESULT(boc_deserializer, BocDeserializerT::create(serialized));
  TRY_RESULT(root_count, boc_deserializer->get_root_count());
  ASSERT_EQ(1u, root_count);
  TRY_RESULT(loaded_cell, boc_deserializer->get_root_cell(0));
  std::vector<td::thread> threads;
  for (auto &exploration : explorations) {
    threads.emplace_back([&] {
      auto exploration2 = CellExplorer::explore(loaded_cell, exploration.ops);
      ASSERT_EQ(exploration.log, exploration2.log);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  return td::Status::OK();
}

td::Status test_boc_deserializer_full(std::vector<Ref<Cell>> cells) {
  for (auto mode : get_serialization_modes()) {
    TRY_STATUS(vm::test_boc_deserializer<vm::StaticBagOfCellsDbBaseline>(cells, mode));
    TRY_STATUS(vm::test_boc_deserializer<vm::StaticBagOfCellsDbLazy>(cells, mode));
  }
  return td::Status::OK();
}
td::Status test_boc_deserializer_full(Ref<Cell> cell) {
  return test_boc_deserializer_full(std::vector<Ref<Cell>>{std::move(cell)});
}

template <class BocDeserializerT>
void test_boc_deserializer() {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 1000; t++) {
    auto cells = gen_random_cells(rnd.fast(1, 10), static_cast<int>(rnd() % 1000 + 1), rnd);
    for (auto mode : get_serialization_modes()) {
      test_boc_deserializer<BocDeserializerT>(cells, mode).ensure();
    }
  }
}

TEST(TonDb, BocDeserializerBaseline) {
  test_boc_deserializer<StaticBagOfCellsDbBaseline>();
}

TEST(TonDb, BocDeserializerSimple) {
  test_boc_deserializer<StaticBagOfCellsDbLazy>();
}

template <class BocDeserializerT>
void test_boc_deserializer_threads() {
  td::Random::Xorshift128plus rnd{123};
  for (int t = 0; t < 20; t++) {
    auto cell = gen_random_cell(static_cast<int>(rnd() % 1000 + 1), rnd);
    for (auto mode : get_serialization_modes()) {
      test_boc_deserializer_threads<BocDeserializerT>(cell, mode, rnd).ensure();
    }
  }
}

TEST(TonDb, BocDeserializerSimpleThreads) {
  test_boc_deserializer_threads<StaticBagOfCellsDbLazy>();
}

class CompactArray {
 public:
  CompactArray(size_t size) {
    root_ = create(size, 0);
    size_ = size;
  }
  CompactArray(size_t size, Ref<Cell> root) {
    root_ = std::move(root);
    size_ = size;
  }
  CompactArray(td::Span<td::uint64> span) {
    root_ = create(span);
    size_ = span.size();
  }
  CompactArray(CompactArray &&other) = default;
  CompactArray &operator=(CompactArray &&other) = default;

  td::Slice hash() const {
    return root()->get_hash().as_slice();
  }
  void set(size_t pos, td::uint64 value) {
    root_ = set(root_, size_, pos, value);
  }
  td::uint64 get(size_t pos) {
    return get(root_, size_, pos, nullptr);
  }

  const Ref<Cell> &root() const {
    return root_;
  }
  size_t size() const {
    return size_;
  }
  void reset() {
    size_ = 0;
    root_ = {};
  }

  Ref<Cell> merkle_proof(std::vector<size_t> keys) {
    std::set<Cell::Hash> hashes;
    for (auto key : keys) {
      get(root_, size_, key, &hashes);
    }

    auto is_prunned = [&](const Ref<Cell> &cell) { return hashes.count(cell->get_hash()) == 0; };
    return MerkleProof::generate_raw(root_, is_prunned);
  }

 private:
  Ref<Cell> root_;
  size_t size_;

  static Ref<DataCell> create_list(td::uint64 value) {
    CellBuilder cb;
    cb.store_long(value, 64);
    return cb.finalize();
  }
  static Ref<DataCell> create_node(Ref<Cell> left, Ref<Cell> right) {
    CellBuilder cb;
    cb.store_ref(std::move(left));
    cb.store_ref(std::move(right));
    return cb.finalize();
  }
  static Ref<DataCell> create(size_t size, td::uint64 value) {
    if (size == 1) {
      return create_list(value);
    }
    return create_node(create(size / 2, value), create((size + 1) / 2, value));
  }
  static Ref<DataCell> create(td::Span<td::uint64> value) {
    if (value.size() == 1) {
      return create_list(value[0]);
    }
    return create_node(create(value.substr(0, value.size() / 2)), create(value.substr(value.size() / 2)));
  }

  static td::uint64 get(Ref<Cell> any_cell, size_t size, size_t pos, std::set<Cell::Hash> *hashes) {
    if (hashes) {
      hashes->insert(any_cell->get_hash());
    }
    CellSlice cs(NoVm(), any_cell);
    assert(pos < size);
    if (size == 1) {
      return cs.fetch_long(64);
    }
    auto left = cs.fetch_ref();
    if (pos < size / 2) {
      return get(left, size / 2, pos, hashes);
    }
    pos -= size / 2;
    auto right = cs.fetch_ref();
    return get(right, (size + 1) / 2, pos, hashes);
  }

  static Ref<DataCell> set(Ref<Cell> any_cell, size_t size, size_t pos, td::uint64 value) {
    CellSlice cs(NoVm(), any_cell);
    assert(pos < size);
    if (size == 1) {
      return create_list(value);
    }
    //LOG(ERROR) << cell->size_refs() << " " << cell->size_bits();
    auto left = cs.fetch_ref();
    auto right = cs.fetch_ref();
    if (pos < size / 2) {
      left = set(left, size / 2, pos, value);
    } else {
      pos -= size / 2;
      right = set(right, (size + 1) / 2, pos, value);
    }
    return create_node(left, right);
  }
};

class FastCompactArray {
 public:
  FastCompactArray(size_t size) : v_(size) {
  }
  void set(size_t pos, td::uint64 value) {
    v_.at(pos) = value;
  }
  td::uint64 get(size_t pos) {
    return v_.at(pos);
  }
  td::Span<td::uint64> as_span() const {
    return v_;
  }

 private:
  std::vector<td::uint64> v_;
};

struct BocTestHelper {
 public:
  BocTestHelper() = default;
  BocTestHelper(td::int64 seed) : rnd_(seed) {
  }

  CompactArray create_array(size_t size, td::uint64 max_value) {
    std::vector<td::uint64> v(size);
    td::Random::Xorshift128plus rnd{123};
    for (auto &x : v) {
      x = rnd() % max_value;
    }
    return CompactArray(v);
  }

 private:
  td::Random::Xorshift128plus rnd_{123};
};

DynamicBagOfCellsDb::Stats bench_dboc_get_and_set(BocOptions options) {
  BocTestHelper helper(options.seed);
  size_t n = 1 << 20;
  size_t max_value = 1 << 26;
  auto arr = helper.create_array(n, max_value);

  // auto kv = std::make_shared<td::MemoryKeyValue>();
  td::Slice db_path = "compact_array_db";
  td::RocksDb::destroy(db_path).ensure();

  DB db = options.create_db({}, {});
  DynamicBagOfCellsDb::Stats stats;

  td::Timer total_timer;

  auto bench = [&](td::Slice desc, auto &&f) {
    auto before = db.dboc->get_stats().move_as_ok();
    td::Timer timer;
    LOG(ERROR) << "Benchmarking " << desc;
    f();
    stats.named_stats.stats_int[desc.str()] = td::int64(timer.elapsed() * 1000);
    LOG(ERROR) << "Benchmarking " << desc << " done: " << timer.elapsed() << "s\n";
    auto after = db.dboc->get_stats().move_as_ok();
    after.named_stats.subtract_diff(before.named_stats);
    LOG(ERROR) << after;
  };

  td::VectorQueue<vm::CellHash> roots;
  // Save array in db
  bench(PSLICE() << "bench_inc_large_db(n=" << n << ")", [&] {
    db.dboc->inc(arr.root());
    roots.push(arr.root()->get_hash());
    options.commit(db, BocOptions::ResetCache);
  });
  bench("bench_compactify", [&] {
    auto status = dynamic_cast<td::RocksDb &>(*db.kv).raw_db()->CompactRange({}, nullptr, nullptr);
    LOG_IF(FATAL, !status.ok()) << status.ToString();
  });
  db = options.create_db(std::move(db), {});

  bench(PSLICE() << "bench_inc_large_existed_db(n=" << n << ")", [&] {
    db.dboc->inc(arr.root());
    roots.push(arr.root()->get_hash());
    options.commit(db, BocOptions::ResetCache);
  });

  td::Random::Xorshift128plus rnd{123};
  while (false) {
    auto hash = arr.root()->get_hash();
    arr = CompactArray{n, db.dboc->load_root(hash.as_slice()).move_as_ok()};
    td::Timer timer;
    for (size_t i = 0; i < 10000; i++) {
      auto pos = rnd() % n;
      arr.get(pos);
    }
    LOG(ERROR) << timer.elapsed() << "s\n";
    db.reset_loader();
  }

  for (auto p : std::vector<std::pair<size_t, size_t>>{{10000, 0}, {10000, 5}, {5000, 5000}, {5, 10000}, {0, 10000}}) {
    auto get_n = p.first;
    auto set_n = p.second;
    auto hash = arr.root()->get_hash();
    arr = CompactArray{n, db.dboc->load_root(hash.as_slice()).move_as_ok()};
    bench(PSTRING() << "bench_changes(get_n=" << get_n << ", set_n=" << set_n << ")", [&] {
      for (size_t i = 0; i < get_n; i++) {
        auto pos = rnd() % n;
        arr.get(pos);
      }
      for (size_t i = 0; i < set_n; i++) {
        auto pos = rnd() % n;
        auto value = rnd() % max_value;
        arr.set(pos, value);
      }
    });
    bench(PSTRING() << "bench_commit(get_n=" << get_n << ", set_n=" << set_n << ")", [&] {
      db.dboc->inc(arr.root());
      roots.push(arr.root()->get_hash());
      options.commit(db, BocOptions::ResetCache);
    });
  }
  arr.reset();

  bench(PSLICE() << "bench_dec_some_roots()", [&] {
    while (roots.size() > 1) {
      auto hash = roots.pop();
      auto cell = db.dboc->load_cell(hash.as_slice()).move_as_ok();
      db.dboc->dec(cell);
    }
    options.commit(db, BocOptions::ResetCache);
  });

  db = options.create_db(std::move(db), {});

  bench(PSLICE() << "bench_dec_large_root(n=" << n << ")", [&] {
    while (!roots.empty()) {
      auto hash = roots.pop();
      auto cell = db.dboc->load_cell(hash.as_slice()).move_as_ok();
      db.dboc->dec(cell);

      /*
      do {
        auto cell = db.dboc->load_cell(hash.as_slice()).move_as_ok();
        db.dboc->dec(cell);
        cell = {};
	options.prepare_commit(*db.dboc);
        //db.dboc->prepare_commit().ensure();
        db.reset_loader();
        db = options.create_db(std::move(db), {});
      } while (true);
      */
    }
    options.commit(db, BocOptions::ResetCache);
  });
  stats.named_stats.stats_int["bench_total"] = td::int64(total_timer.elapsed() * 1000);

  return stats;
}

TEST(TonDb, BenchDynamicBocGetAndSet) {
  with_all_boc_options(bench_dboc_get_and_set, 1);
}

TEST(TonDb, DynamicBocIncSimple) {
  auto kv = std::make_shared<td::MemoryKeyValue>(std::make_shared<CellMerger>());
  auto db = DynamicBagOfCellsDb::create_v2({.extra_threads = 0});
  db->set_loader(std::make_unique<vm::CellLoader>(kv));

  td::Random::Xorshift128plus rnd(123);
  size_t size = 4;
  std::vector<td::uint64> values(size);
  for (auto &v : values) {
    //v = rnd() % 2;
    v = rnd();
  }
  // 1. Create large dictionary and store it in db
  auto arr_ptr = std::make_unique<CompactArray>(values);
  auto &arr = *arr_ptr;
  td::VectorQueue<CellHash> queue;
  auto push = [&]() {
    //LOG(ERROR) << "PUSH ROOT";
    auto begin_stats = kv->get_usage_stats();
    db->inc(arr.root());
    queue.push(arr.root()->get_hash());
    vm::CellStorer cell_storer(*kv);
    db->commit(cell_storer);
    auto end_stats = kv->get_usage_stats();
    LOG(ERROR) << end_stats - begin_stats;
    db->set_loader(std::make_unique<vm::CellLoader>(kv));
    auto hash = arr.root()->get_hash();
    arr = CompactArray{size, db->load_root(hash.as_slice()).move_as_ok()};
    //LOG(ERROR) << "CELLS IN DB: " << kv->count("").move_as_ok();
  };
  auto pop = [&]() {
    if (queue.empty()) {
      return;
    }
    //LOG(ERROR) << "POP ROOT";
    auto begin_stats = kv->get_usage_stats();
    auto cell = db->load_cell(queue.pop().as_slice()).move_as_ok();
    db->dec(cell);
    vm::CellStorer cell_storer(*kv);
    db->commit(cell_storer);
    auto end_stats = kv->get_usage_stats();
    db->set_loader(std::make_unique<vm::CellLoader>(kv));
    //LOG(ERROR) << end_stats - begin_stats;
    //LOG(ERROR) << "CELLS IN DB: " << kv->count("").move_as_ok();
  };
  auto upd = [&] {
    for (int i = 0; i < 20; i++) {
      auto pos = rnd.fast(0, td::narrow_cast<int>(size) - 1);
      if (rnd() % 2) {
        auto value = rnd() % 2;
        arr.set(pos, value);
      } else {
        arr.get(pos);
      }
    }
  };

  //LOG(ERROR) << "Created compact array";
  push();
  pop();
  //CHECK(kv->count("").move_as_ok() == 0);

  // 2. Lets change first 20 keys and read last 20 keys
  /*
  for (size_t i = 0; i < 20 && i < size; i++) {
    arr.set(i, rnd());
  }
  */
  //arr.set(0, rnd());
  arr.set(size - 1, rnd());
  for (size_t i = 0; i < 20 && i < size; i++) {
    arr.get(size - i - 1);
  }

  // 3. And now commit diff with stats
  push();
  push();
  upd();
  upd();
  push();
  push();
  upd();
  pop();
  pop();
  upd();
  push();
  push();
  while (!queue.empty()) {
    pop();
  }
  LOG(ERROR) << "CELLS IN DB: " << kv->count("").move_as_ok();
}

class BenchCellStorerMergeRefcntDiffs : public td::Benchmark {
 public:
  std::string get_description() const override {
    return PSTRING() << "bench_cells_storer_merge_refcnt_diffs";
  }

  void run(int n) override {
    auto cell = vm::CellBuilder().store_bytes(std::string(32, 'A')).finalize();
    auto left_update = CellStorer::serialize_refcnt_diffs(1);
    auto right_update = CellStorer::serialize_refcnt_diffs(1);
    for (int i = 0; i < n; i++) {
      CellStorer::merge_refcnt_diffs(left_update, right_update);
    }
  }

 private:
  size_t tn_{};
};
class BenchCellStorerMergeValueAndRefcntDiff : public td::Benchmark {
 public:
  std::string get_description() const override {
    return PSTRING() << "bench_cells_storer_merge_value_and_refcnt_diffs";
  }

  void run(int n) override {
    auto cell = vm::CellBuilder().store_bytes(std::string(32, 'A')).finalize();
    auto value = CellStorer::serialize_value(10, cell, false);
    auto update = CellStorer::serialize_refcnt_diffs(1);
    for (int i = 0; i < n; i++) {
      CellStorer::merge_value_and_refcnt_diff(value, update);
    }
  }

 private:
  size_t tn_{};
};
TEST(Bench, CellStorerMerge) {
  bench(BenchCellStorerMergeRefcntDiffs());
  bench(BenchCellStorerMergeValueAndRefcntDiff());
}

TEST(Cell, BocHands) {
  serialize_boc(CellBuilder{}.store_bytes("AAAAAAAA").finalize());
  auto a = CellBuilder{}.store_bytes("abcd").store_ref(CellBuilder{}.store_bytes("???").finalize()).finalize();
  a = CellBuilder{}
          .store_bits("XXX", 3)
          .store_ref(CellBuilder::create_pruned_branch(std::move(a), Cell::max_level))
          .finalize();
  auto serialized = serialize_boc(a);
  deserialize_boc(serialized);
  deserialize_boc(serialize_boc(std::vector<Ref<Cell>>{a, a}));

  // CHECK backward compatibility with
  // serialized_boc_idx and serialized_boc_idx_crc32c
  //auto serialized_idx_crc_x = serialize_boc(a, BagOfCells::WithIndex | BagOfCells::WithCRC32C);
  //LOG(ERROR) << td::format::escaped(serialized_idx_crc_x);
  std::string serialized_idx_crc =
      td::Slice(
          "\254\303\247(\001\001\002\001\000*\004*\201\001P\001\210H\001\004\024\271\313\264\253\277\265\350dN\250{,"
          "\372\021\012:I\354\322|\255\245\330\204+&\345\214\026\300\064\000\001\032\231\063\274")
          .str();
  //auto serialized_idx_x = serialize_boc(a, BagOfCells::WithIndex);
  //LOG(ERROR) << td::format::escaped(serialized_idx_x);
  std::string serialized_idx =
      td::Slice(
          "h\377e\363\001\001\002\001\000*\004*\201\001P\001\210H\001\004\024\271\313\264\253\277\265\350dN\250{,"
          "\372\021\012:I\354\322|\255\245\330\204+&\345\214\026\300\064\000\001")
          .str();

  ASSERT_EQ(serialized, serialize_boc(deserialize_boc(serialized_idx)));
  ASSERT_EQ(serialized, serialize_boc(deserialize_boc(serialized_idx_crc)));
}

TEST(Cell, MerkleProofHands) {
  // data has a reference, because we do not prune lists
  auto data = CellBuilder{}.store_bytes("pruned data").store_ref(CellBuilder{}.finalize()).finalize();
  auto prunned_data = CellBuilder::create_pruned_branch(data, data->get_level() + 1);
  ASSERT_EQ(1u, prunned_data->get_level());
  ASSERT_EQ(prunned_data->get_hash(0), data->get_hash(0));
  ASSERT_EQ(data->get_hash(0), data->get_hash(1));
  ASSERT_TRUE(prunned_data->get_hash(1) != prunned_data->get_hash(0));

  auto node = CellBuilder{}.store_bytes("protected data").store_ref(data).finalize();
  auto proof = CellBuilder{}.store_bits(node->get_data(), node->get_bits()).store_ref(prunned_data).finalize();
  ASSERT_EQ(0u, node->get_level());
  ASSERT_EQ(1u, proof->get_level());
  ASSERT_EQ(proof->get_hash(0), node->get_hash(0));
  ASSERT_TRUE(proof->get_hash(1) != node->get_hash(1));
  test_boc_deserializer_full(proof).ensure();

  auto merkle_proof = CellBuilder::create_merkle_proof(proof);
  ASSERT_EQ(0u, merkle_proof->get_level());
  test_boc_deserializer_full(merkle_proof).ensure();

  {
    auto virtual_node = proof->virtualize({0, 1});
    ASSERT_EQ(0u, virtual_node->get_level());
    ASSERT_EQ(1u, virtual_node->get_virtualization());
    CellSlice cs{NoVm(), virtual_node};
    auto virtual_data = cs.fetch_ref();
    ASSERT_EQ(0u, virtual_data->get_level());
    ASSERT_EQ(1u, virtual_data->get_virtualization());
    ASSERT_EQ(data->get_hash(), virtual_data->get_hash());

    auto virtual_node_copy =
        CellBuilder{}.store_bits(node->get_data(), node->get_bits()).store_ref(virtual_data).finalize();
    ASSERT_EQ(0u, virtual_node_copy->get_level());
    ASSERT_EQ(1u, virtual_node_copy->get_virtualization());
    ASSERT_EQ(virtual_node->get_hash(), virtual_node_copy->get_hash());

    {
      auto two_nodes = CellBuilder{}.store_ref(virtual_node).store_ref(node).finalize();
      ASSERT_EQ(0u, two_nodes->get_level());
      ASSERT_EQ(1u, two_nodes->get_virtualization());
      CellSlice cs2(NoVm(), two_nodes);
      ASSERT_EQ(1u, cs2.prefetch_ref(0)->get_virtualization());
      ASSERT_EQ(0u, cs2.prefetch_ref(1)->get_virtualization());
    }
  }
  LOG(ERROR) << td::NamedThreadSafeCounter::get_default();
}

TEST(Cell, MerkleProofArrayHands) {
  // create simple array
  CompactArray arr(17);
  for (size_t i = 0; i < arr.size(); i++) {
    arr.set(i, i / 3);
  }

  // create merke proof for 4 5 6 and 16th elements
  std::vector<size_t> keys = {4, 5, 6, 16};
  auto proof = arr.merkle_proof(keys);

  ASSERT_EQ(1u, proof->get_level());
  ASSERT_EQ(proof->get_hash(0), arr.root()->get_hash(0));
  ASSERT_TRUE(proof->get_hash(1) != arr.root()->get_hash(1));
  ASSERT_EQ(arr.root()->get_hash(0), arr.root()->get_hash(1));

  CompactArray new_arr(arr.size(), proof->virtualize({0, 1}));
  for (auto k : keys) {
    ASSERT_EQ(arr.get(k), new_arr.get(k));
  }
  test_boc_deserializer_full(proof).ensure();
  test_boc_deserializer_full(CellBuilder::create_merkle_proof(proof)).ensure();
}

TEST(Cell, MerkleProofCombineArray) {
  size_t n = 1 << 15;
  std::vector<td::uint64> data;
  for (size_t i = 0; i < n; i++) {
    data.push_back(i / 3);
  }
  CompactArray arr(data);

  td::Ref<vm::Cell> root = vm::CellBuilder::create_merkle_proof(arr.merkle_proof({}));
  td::Timer timer;
  for (size_t i = 0; i < n; i++) {
    auto new_root = vm::CellBuilder::create_merkle_proof(arr.merkle_proof({i}));
    root = vm::MerkleProof::combine_fast(root, new_root);
    if ((i - 1) % 100 == 0) {
      LOG(ERROR) << timer;
      timer = {};
    }
  }

  CompactArray arr2(n, vm::MerkleProof::virtualize(root, 1));
  for (size_t i = 0; i < n; i++) {
    CHECK(arr.get(i) == arr2.get(i));
  }
}

TEST(Cell, MerkleProofCombineArray2) {
  auto a = vm::CellBuilder().store_long(1, 8).finalize();
  auto b = vm::CellBuilder().store_long(2, 8).finalize();
  auto c = vm::CellBuilder().store_long(3, 8).finalize();
  auto d = vm::CellBuilder().store_long(4, 8).finalize();
  auto left = vm::CellBuilder().store_ref(a).store_ref(b).finalize();
  auto right = vm::CellBuilder().store_ref(c).store_ref(d).finalize();
  auto x = vm::CellBuilder().store_ref(left).store_ref(right).finalize();
  size_t n = 18;
  //TODO: n = 100, currently TL
  for (size_t i = 0; i < n; i++) {
    x = vm::CellBuilder().store_ref(x).store_ref(x).finalize();
  }

  td::Ref<vm::Cell> root;
  auto apply_op = [&](auto op) {
    auto usage_tree = std::make_shared<CellUsageTree>();
    auto usage_cell = UsageCell::create(x, usage_tree->root_ptr());
    root = usage_cell;
    op();
    return MerkleProof::generate(root, usage_tree.get());
  };

  auto first = apply_op([&] {
    auto x = root;
    while (true) {
      auto cs = vm::load_cell_slice(x);
      if (cs.size_refs() == 0) {
        break;
      }
      x = cs.prefetch_ref(0);
    }
  });
  auto second = apply_op([&] {
    auto x = root;
    while (true) {
      auto cs = vm::load_cell_slice(x);
      if (cs.size_refs() == 0) {
        break;
      }
      x = cs.prefetch_ref(1);
    }
  });

  {
    td::Timer t;
    auto x = vm::MerkleProof::combine(first, second);
    LOG(ERROR) << "slow " << t;
  }
  {
    td::Timer t;
    auto x = vm::MerkleProof::combine_fast(first, second);
    LOG(ERROR) << "fast " << t;
  }
}

TEST(Cell, MerkleUpdateHands) {
  auto data = CellBuilder{}.store_bytes("pruned data").store_ref(CellBuilder{}.finalize()).finalize();
  auto node = CellBuilder{}.store_bytes("protected data").store_ref(data).finalize();
  auto other_node = CellBuilder{}.store_bytes("other protected data").store_ref(data).finalize();
  auto usage_tree = std::make_shared<CellUsageTree>();
  auto other_usage_tree = std::make_shared<CellUsageTree>();
  auto usage_cell = UsageCell::create(node, usage_tree->root_ptr());
  auto child = CellSlice(vm::NoVm(), usage_cell).prefetch_ref(0);
  auto new_node = CellBuilder{}.store_bytes("new data").store_ref(child).finalize();
  auto new_child = CellSlice(vm::NoVm(), new_node).prefetch_ref(0);
  auto update = MerkleUpdate::generate(usage_cell, new_node, usage_tree.get());

  MerkleUpdate::may_apply(node, update).ensure();
  MerkleUpdate::validate(update).ensure();
  auto x = MerkleUpdate::apply(node, update);
  ASSERT_TRUE(serialize_boc(new_node) == serialize_boc(x));

  MerkleUpdate::may_apply(other_node, update).ensure_error();
  ASSERT_TRUE(MerkleUpdate::apply(other_node, update).is_null());
  auto other_update = CellBuilder::create_merkle_update(CellBuilder::create_pruned_branch(other_node, 1),
                                                        CellBuilder::create_pruned_branch(new_node, 1));
  MerkleUpdate::may_apply(node, other_update).ensure_error();
  MerkleUpdate::validate(other_update).ensure_error();
  ASSERT_TRUE(MerkleUpdate::apply(other_node, other_update).is_null());
  auto bad_update = CellBuilder::create_merkle_update(CellBuilder::create_pruned_branch(new_node, 1),
                                                      CellBuilder::create_pruned_branch(other_node, 1));
  CHECK(MerkleUpdate::combine(update, bad_update).is_null());
}

TEST(Cell, MerkleUpdateArray) {
  // create simple array
  size_t n = 1 << 20;
  std::vector<td::uint64> data;
  for (size_t i = 0; i < n; i++) {
    data.push_back(i / 3);
  }
  CompactArray arr(data);
  auto root = arr.root();
  auto usage_tree = std::make_shared<CellUsageTree>();
  auto usage_cell = UsageCell::create(root, usage_tree->root_ptr());
  arr = CompactArray(n, usage_cell);
  arr.set(n / 2, 0);
  arr.set(n / 2 + 1, 1);
  arr.set(n / 2 + 2, 2414221111);
  arr.set(n / 2 + 3, 2);

  auto update = MerkleUpdate::generate(usage_cell, arr.root(), usage_tree.get());
  CellStorageStat stat;
  stat.compute_used_storage(update, false);
  ASSERT_EQ(stat.cells, 81u);
  //CellSlice(NoVm(), update).print_rec(std::cerr);

  check_merkle_update(root, arr.root(), update);
}

TEST(Cell, MerkleUpdateCombineArray) {
  size_t n = 1 << 10;
  std::vector<td::uint64> data;
  for (size_t i = 0; i < n; i++) {
    data.push_back(i / 3);
  }
  CompactArray arr(data);
  auto from = arr.root();
  std::shared_ptr<CellUsageTree> usage_tree;
  Ref<Cell> usage_cell;

  std::vector<Ref<Cell>> updates;

  auto apply_op = [&](auto op) {
    auto A = arr.root();
    usage_tree = std::make_shared<CellUsageTree>();
    usage_cell = UsageCell::create(arr.root(), usage_tree->root_ptr());
    arr = CompactArray(n, usage_cell);
    op();
    updates.push_back(MerkleUpdate::generate(A, arr.root(), usage_tree.get()));
  };

  auto combine_all = [&]() {
    while (updates.size() > 1) {
      size_t i = updates.size() - 2;
      updates[i] = MerkleUpdate::combine(updates[i], updates[i + 1]);
      updates.pop_back();
      CellStorageStat stat;
      stat.compute_used_storage(updates[i], false);
    }
  };
  auto validate = [&](size_t size) {
    combine_all();
    check_merkle_update(from, arr.root(), updates.at(0));
    CellStorageStat stat;
    stat.compute_used_storage(updates[0], false);
    if (size != 0) {
      ASSERT_EQ(size, stat.cells);
    }
  };
  apply_op([] {});
  validate(3);
  apply_op([] {});
  apply_op([] {});
  apply_op([] {});
  validate(3);

  apply_op([&] {
    for (size_t i = 0; i < n; i++) {
      arr.set(i, i / 3 + 10);
    }
  });
  apply_op([&] {
    for (size_t i = 0; i < n; i++) {
      arr.set(i, i / 3);
    }
  });
  validate(3);

  for (size_t i = 0; i + 1 < n; i++) {
    apply_op([&] {
      arr.set(i, i / 3 + 1);
      if (i != 0) {
        arr.set(i - 1, (i - 1) / 3);
      }
    });
  }

  validate(41);
}

}  // namespace vm

class BenchBocSerializerImport : public td::Benchmark {
 public:
  BenchBocSerializerImport() {
    std::vector<td::uint64> v(array_size);
    td::Random::Xorshift128plus rnd{123};
    for (auto &x : v) {
      x = rnd();
    }
    arr = vm::CompactArray(v);
    //serialization_ = td::BufferSlice(boc.serialize_to_string(15));
  }
  std::string get_description() const override {
    return "BenchBocSerializer";
  }

  void run(int n) override {
    for (int i = 0; i < n; i++) {
      vm::BagOfCells boc;
      boc.add_root(arr.root());
      boc.import_cells().ensure();
    }
  }

 private:
  td::BufferSlice serialization_;
  static constexpr td::uint32 array_size = 1024;
  vm::CompactArray arr{1};
};

class BenchBocSerializerSerialize : public td::Benchmark {
 public:
  BenchBocSerializerSerialize() {
    std::vector<td::uint64> v(array_size);
    td::Random::Xorshift128plus rnd{123};
    for (auto &x : v) {
      x = rnd();
    }
    arr = vm::CompactArray(v);
    boc.add_root(arr.root());
    boc.import_cells().ensure();
  }
  std::string get_description() const override {
    return "BenchBocSerializer";
  }

  void run(int n) override {
    for (int i = 0; i < n; i++) {
      boc.serialize_to_string(31);
    }
  }

 private:
  td::BufferSlice serialization_;
  static constexpr td::uint32 array_size = 1024;
  vm::CompactArray arr{1};
  vm::BagOfCells boc;
};

struct BenchBocDeserializerConfig {
  enum BlobType { File, Memory, FileMemoryMap, RocksDb } blob_type;
  int k{100};
  enum Mode { Prefix, Range, Random } mode{Random};
  bool with_index{true};
  int threads_n{1};
};

td::StringBuilder &operator<<(td::StringBuilder &sb, const BenchBocDeserializerConfig &config) {
  sb << "load from ";
  switch (config.blob_type) {
    case BenchBocDeserializerConfig::File:
      sb << "file";
      break;
    case BenchBocDeserializerConfig::Memory:
      sb << "memory";
      break;
    case BenchBocDeserializerConfig::FileMemoryMap:
      sb << "file mmap";
      break;
    case BenchBocDeserializerConfig::RocksDb:
      sb << "rocksdb";
      break;
  }
  sb << td::tag("k", config.k) << " ";
  switch (config.mode) {
    case BenchBocDeserializerConfig::Prefix:
      sb << "prefix";
      break;
    case BenchBocDeserializerConfig::Range:
      sb << "range";
      break;
    case BenchBocDeserializerConfig::Random:
      sb << "random";
      break;
  }
  sb << " " << (config.with_index ? "with" : "without") << " index";
  sb << " " << config.threads_n << " threads";
  return sb;
}

template <class DeserializerT>
class BenchBocDeserializer : public td::Benchmark {
 public:
  BenchBocDeserializer(std::string name, BenchBocDeserializerConfig config) : name_(std::move(name)), config_(config) {
    td::PerfWarningTimer perf("A", 1);
    fast_array_ = vm::FastCompactArray(array_size);
    td::Random::Xorshift128plus rnd{123};
    for (td::uint32 i = 0; i < array_size; i++) {
      auto val = rnd();
      fast_array_.set(i, val);
    }
    vm::CompactArray arr(fast_array_.as_span());
    auto db_path = "serialization_rocksdb";
    if (config.blob_type == BenchBocDeserializerConfig::RocksDb) {
      {
        td::RocksDb::destroy(td::Slice(db_path)).ensure();
        auto db = vm::TonDbImpl::open(td::Slice(db_path)).move_as_ok();
        auto txn = db->begin_transaction();
        auto smt = txn->begin_smartcontract();
        SCOPE_EXIT {
          db->commit_transaction(std::move(txn));
        };
        SCOPE_EXIT {
          txn->commit_smartcontract(std::move(smt));
        };
        smt->set_root(arr.root());
      }
      db_ = vm::TonDbImpl::open(td::Slice(db_path)).move_as_ok();
    } else {
      serialization_ = td::BufferSlice(serialize_boc(
          arr.root(), vm::BagOfCells::WithIntHashes | vm::BagOfCells::WithTopHash |
                          (config.with_index ? vm::BagOfCells::WithIndex | vm::BagOfCells::WithCacheBits : 0)));

      if (config.blob_type == BenchBocDeserializerConfig::File ||
          config.blob_type == BenchBocDeserializerConfig::FileMemoryMap) {
        td::unlink("serialization").ignore();
        td::write_file("serialization", serialization_.as_slice()).ensure();
      }
    }
    root_ = arr.root();
  }
  std::string get_description() const override {
    return PSTRING() << "BocDeserializer " << name_ << " " << config_;
  }

  vm::Ref<vm::Cell> load_root() {
    if (config_.blob_type == BenchBocDeserializerConfig::RocksDb) {
      auto txn = db_->begin_transaction();
      auto smt = txn->begin_smartcontract();
      SCOPE_EXIT {
        db_->abort_transaction(std::move(txn));
      };
      SCOPE_EXIT {
        txn->commit_smartcontract(std::move(smt));
      };
      LOG(ERROR) << "load root from rocksdb";
      return smt->get_root();
    }
    auto blob = [&] {
      switch (config_.blob_type) {
        case BenchBocDeserializerConfig::File:
          return td::FileBlobView::create("serialization").move_as_ok();
        case BenchBocDeserializerConfig::Memory:
          return td::BufferSliceBlobView::create(serialization_.clone());
        case BenchBocDeserializerConfig::FileMemoryMap:
          return td::FileMemoryMappingBlobView::create("serialization").move_as_ok();
        default:
          UNREACHABLE();
      }
      UNREACHABLE();
    }();
    auto boc_deserializer = DeserializerT::create(std::move(blob)).move_as_ok();
    ASSERT_EQ(1u, boc_deserializer->get_root_count().move_as_ok());
    return boc_deserializer->get_root_cell(0).move_as_ok();
  }

  void run(int n) override {
    td::Random::Xorshift128plus rnd{123};

    std::vector<td::thread> threads;
    //TODO: use config.k
    auto K = config_.k == 0 ? n : config_.k;
    td::Stage stage;
    vm::Ref<vm::Cell> root;
    for (int t = 0; t < config_.threads_n; t++) {
      threads.emplace_back([&, seed = rnd(), thread_i = t] {
        for (int round_i = 0; round_i < n / K; round_i++) {
          if (thread_i == 0) {
            root = load_root();
          }
          stage.wait(config_.threads_n * (2 * round_i + 1));

          vm::CompactArray array(array_size, root);
          td::Random::Xorshift128plus rnd{seed};
          td::uint64 start_pos =
              config_.mode == BenchBocDeserializerConfig::Range ? array_size / config_.threads_n * thread_i : 0;
          for (int k = 0; k < K; k++) {
            auto pos = start_pos;
            switch (config_.mode) {
              case BenchBocDeserializerConfig::Prefix:
              case BenchBocDeserializerConfig::Range:
                pos = (pos + k) % array_size;
                break;
              case BenchBocDeserializerConfig::Random:
                pos = rnd() % array_size;
                break;
            }
            ASSERT_EQ(fast_array_.get(td::narrow_cast<std::size_t>(pos)), array.get(td::narrow_cast<std::size_t>(pos)));
          }
          stage.wait(config_.threads_n * (2 * round_i + 2));
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
  }

 private:
  std::string name_;
  td::BufferSlice serialization_;
  BenchBocDeserializerConfig config_;
  static constexpr td::uint32 array_size = 1024 * 1024;
  vm::FastCompactArray fast_array_{array_size};
  vm::Ref<vm::Cell> root_;
  vm::TonDb db_;
};

TEST(TonDb, BenchBocSerializerImport) {
  if (0) {
    BenchBocSerializerImport b;
    while (true) {
      td::bench_n(b, 1000000);
    }
  }
  td::bench(BenchBocSerializerImport());
}
TEST(TonDb, BenchBocSerializerSerialize) {
  td::bench(BenchBocSerializerSerialize());
}

template <class DeserializerT>
void bench_deserializer(std::string name, bool full) {
  using Config = BenchBocDeserializerConfig;
  if (full) {
    for (auto k : {1, 10, 100}) {
      for (auto with_index : {false, true}) {
        for (auto mode : {Config::Prefix, Config::Range, Config::Random}) {
          for (auto blob_type : {Config::Memory, Config::File, Config::FileMemoryMap}) {
            BenchBocDeserializerConfig config;
            config.k = k;
            config.with_index = with_index;
            config.mode = mode;
            config.blob_type = blob_type;
            td::bench(BenchBocDeserializer<DeserializerT>(name, config));
          }
        }
      }
    }
  } else {
    td::bench(BenchBocDeserializer<DeserializerT>(name, BenchBocDeserializerConfig()));
  }
}
template <class DeserializerT>
void bench_deserializer_threads(std::string name) {
  using Config = BenchBocDeserializerConfig;
  for (auto threads_n : {1, 4, 16}) {
    //for (auto threads_n : {16}) {
    //for (auto with_index : {false, true}) {
    //for (auto mode : {BenchBocDeserializerConfig::Prefix, BenchBocDeserializerConfig::Range,
    //BenchBocDeserializerConfig::Random}) {
    //for (auto from_file : {false, true}) {
    BenchBocDeserializerConfig config;
    config.threads_n = threads_n;
    config.k = 0;
    config.with_index = true;
    config.mode = Config::Random;
    config.mode = Config::Range;
    config.mode = Config::Prefix;
    config.blob_type = Config::Memory;
    td::bench(BenchBocDeserializer<DeserializerT>(name, config));
    //td::bench_n(BenchBocThreadsDeserializer<DeserializerT>(name, config), 1000000);
    //}
    //}
    //}
  }
}

TEST(TonDb, BenchBocThreadsDeserializerSimple) {
  //td::bench_n(BenchBocDeserializer<vm::StaticBagOfCellsDbLazy>("simple", BenchBocDeserializerConfig()), 1000000);
  //std::exit(0);
  bench_deserializer_threads<vm::StaticBagOfCellsDbLazy>("simple");
}
TEST(TonDb, BenchBocDeserializerSimple) {
  //td::bench_n(BenchBocDeserializer<vm::StaticBagOfCellsDbLazy>("simple", BenchBocDeserializerConfig()), 1000000);
  //std::exit(0);
  bench_deserializer<vm::StaticBagOfCellsDbLazy>("simple", false);
}
TEST(TonDb, BenchBocDeserializerBaseline) {
  //td::bench_n(BenchBocDeserializer<vm::StaticBagOfCellsDbBaseline>("baseline", BenchBocDeserializerConfig()), 1000000);
  //std::exit(0);
  bench_deserializer<vm::StaticBagOfCellsDbBaseline>("baseline", false);
}
TEST(TonDb, BenchBocDeserializerRocksDb) {
  //td::bench_n(BenchBocDeserializer<vm::StaticBagOfCellsDbBaseline>("baseline", BenchBocDeserializerConfig()), 1000000);
  //std::exit(0);
  auto config = BenchBocDeserializerConfig();
  config.blob_type = BenchBocDeserializerConfig::RocksDb;
  config.threads_n = 4;
  config.k = 0;
  td::bench(BenchBocDeserializer<vm::StaticBagOfCellsDbBaseline>("rockdb", config));
}

TEST(TonDb, CompactArray) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR));
  td::Slice db_path = "compact_array_db";
  td::RocksDb::destroy(db_path).ensure();

  td::Random::Xorshift128plus rnd(123);

  auto next_array_size = [&rnd]() {
    static std::vector<size_t> array_sizes = {1, 2, 4, 10, 37, 100, 1000, 10000};
    return array_sizes[rnd() % array_sizes.size()];
  };

  vm::CompactArray array(2);
  vm::FastCompactArray fast_array(2);
  auto next_pos = [&] { return static_cast<size_t>(rnd() % array.size()); };

  auto db = vm::TonDbImpl::open(db_path).move_as_ok();
  auto txn = db->begin_transaction();
  auto smt = txn->begin_smartcontract();
  SCOPE_EXIT {
    db->commit_transaction(std::move(txn));
  };
  SCOPE_EXIT {
    txn->commit_smartcontract(std::move(smt));
  };

  auto flush_to_db = [&] {
    if (rnd() % 10 != 0) {
      return;
    }
    bool restart_db = rnd() % 20 == 0;
    bool reload_array = rnd() % 5 == 0;
    smt->set_root(array.root());
    txn->commit_smartcontract(std::move(smt));
    db->commit_transaction(std::move(txn));
    if (restart_db) {
      db->clear_cache();
      //db.reset();
      //db = vm::TonDbImpl::open(db_path).move_as_ok();
    }
    txn = db->begin_transaction();
    smt = txn->begin_smartcontract();
    smt->validate_meta().ensure();
    ASSERT_EQ(smt->get_root()->get_hash(), array.root()->get_hash());
    if (reload_array) {
      auto size = array.size();
      array = vm::CompactArray(size, smt->get_root());
    }
  };

  auto do_validate = [&](size_t pos) { ASSERT_EQ(array.get(pos), fast_array.get(pos)); };
  auto validate = [&] { do_validate(next_pos()); };
  auto validate_full = [&] {
    for (size_t pos = 0; pos < array.size(); pos++) {
      do_validate(pos);
    }
  };

  auto set_value = [&] {
    auto pos = static_cast<size_t>(rnd() % array.size());
    auto value = rnd() % 3;
    array.set(pos, value);
    fast_array.set(pos, value);
  };

  auto reset_array = [&] {
    auto size = next_array_size();
    array = vm::CompactArray(size);
    fast_array = vm::FastCompactArray(size);
  };

  td::RandomSteps steps({{reset_array, 1}, {set_value, 1000}, {validate, 10}, {validate_full, 2}, {flush_to_db, 1}});
  for (size_t t = 0; t < 100000; t++) {
    if (t % 10000 == 0) {
      LOG(ERROR) << t;
    }
    steps.step(rnd);
  }
};

TEST(TonDb, CompactArrayOld) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR));
  using namespace vm;
  //auto kv = std::make_unique<MemoryKeyValue>();
  td::RocksDb::destroy("ttt").ensure();

  auto ton_db = vm::TonDbImpl::open("ttt").move_as_ok();

  //auto storage = std::make_unique<CellStorage>(kv.get());

  size_t array_size = 1000;
  std::string hash;
  td::Random::Xorshift128plus rnd(123);
  FastCompactArray fast_array(array_size);
  {
    auto txn = ton_db->begin_transaction();
    SCOPE_EXIT {
      ton_db->commit_transaction(std::move(txn));
    };
    auto smart = txn->begin_smartcontract();
    SCOPE_EXIT {
      txn->commit_smartcontract(std::move(smart));
    };
    CompactArray arr(array_size);
    arr.set(array_size / 2, 124);
    fast_array.set(array_size / 2, 124);
    //for (size_t i = 0; i < array_size; i++) {
    //int x = rnd() % 2;
    //arr.set(i, x);
    //fast_array.set(i, x);
    //}
    smart->set_root(arr.root());
    LOG(ERROR) << smart->get_root()->get_hash().to_hex();
  }
  //LOG(ERROR) << "OK";

  for (int i = 0; i < 100; i++) {
    if (i % 10 == 9) {
      //LOG(ERROR) << ton_db->stat();
      ton_db.reset();
      ton_db = vm::TonDbImpl::open("ttt").move_as_ok();
    }
    auto txn = ton_db->begin_transaction();
    SCOPE_EXIT {
      ton_db->commit_transaction(std::move(txn));
    };
    auto smart = txn->begin_smartcontract();
    //smart->validate_meta();
    SCOPE_EXIT {
      txn->commit_smartcontract(std::move(smart));
    };
    if (i % 1000 == 0) {
      LOG(ERROR) << "i = " << i;
    }
    CompactArray arr(array_size, smart->get_root());
    auto key = static_cast<size_t>(rnd() % array_size);
    auto value = rnd() % 2;
    arr.set(key, value);
    fast_array.set(key, value);
    smart->set_root(arr.root());
    //LOG(ERROR) << storage->size();
  }
  {
    auto txn = ton_db->begin_transaction();
    SCOPE_EXIT {
      ton_db->abort_transaction(std::move(txn));
    };
    auto smart = txn->begin_smartcontract();
    SCOPE_EXIT {
      txn->abort_smartcontract(std::move(smart));
    };

    CompactArray arr(array_size, smart->get_root());
    for (size_t i = 0; i < array_size; i++) {
      ASSERT_EQ(fast_array.get(i), arr.get(i));
    }
  }
}

TEST(TonDb, StackOverflow) {
  try {
    td::Ref<vm::Cell> cell = vm::CellBuilder().finalize();
    for (int i = 0; i < 10000000; i++) {
      vm::CellBuilder cb;
      cb.store_ref(std::move(cell));
      cell = cb.finalize();
    }
    LOG(ERROR) << "A";
    vm::test_boc_deserializer<vm::StaticBagOfCellsDbBaseline>({cell}, 31);
    LOG(ERROR) << "B";
    vm::test_boc_deserializer<vm::StaticBagOfCellsDbLazy>({cell}, 31);
    LOG(ERROR) << "C";
  } catch (...) {
  }

  struct A : public td::CntObject {
    explicit A(td::Ref<A> next) : next(next) {
    }
    td::Ref<A> next;
  };
  {
    td::Ref<A> head;
    for (int i = 0; i < 10000000; i++) {
      td::Ref<A> new_head = td::Ref<A>(true, std::move(head));
      head = std::move(new_head);
    }
  }
}

TEST(TonDb, BocRespectsUsageCell) {
  td::Random::Xorshift128plus rnd(123);
  auto cell = vm::gen_random_cell(20, rnd, true);
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(cell, usage_tree->root_ptr());
  auto serialization = serialize_boc(usage_cell);
  auto proof = vm::MerkleProof::generate(cell, usage_tree.get());
  auto virtualized_proof = vm::MerkleProof::virtualize(proof, 1);
  auto serialization_of_virtualized_cell = serialize_boc(virtualized_proof);
  ASSERT_STREQ(serialization, serialization_of_virtualized_cell);
}

TEST(UsageTree, ThreadSafe) {
  size_t test_n = 100;
  td::Random::Xorshift128plus rnd(123);
  for (size_t test_i = 0; test_i < test_n; test_i++) {
    auto cell = vm::gen_random_cell(rnd.fast(2, 100), rnd, false);
    auto usage_tree = std::make_shared<vm::CellUsageTree>();
    auto usage_cell = vm::UsageCell::create(cell, usage_tree->root_ptr());
    std::ptrdiff_t threads_n = 1;  // TODO: when CellUsageTree is thread safe, change it to 4
    auto barrier = std::barrier{threads_n};
    std::vector<std::thread> threads;
    std::vector<vm::CellExplorer::Exploration> explorations(threads_n);
    for (std::ptrdiff_t i = 0; i < threads_n; i++) {
      threads.emplace_back([&, i = i]() {
        barrier.arrive_and_wait();
        explorations[i] = vm::CellExplorer::random_explore(usage_cell, rnd);
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    auto proof = vm::MerkleProof::generate(cell, usage_tree.get());
    auto virtualized_proof = vm::MerkleProof::virtualize(proof, 1);
    for (auto &exploration : explorations) {
      auto new_exploration = vm::CellExplorer::explore(virtualized_proof, exploration.ops);
      ASSERT_EQ(exploration.log, new_exploration.log);
    }
  }
}

/*
vm::DynamicBagOfCellsDb::Stats test_dynamic_boc_respects_usage_cell(vm::BocOptions options) {
  td::Random::Xorshift128plus rnd(options.seed);
  auto cell = vm::gen_random_cell(20, rnd, true);
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(cell, usage_tree->root_ptr());

  auto kv = std::make_shared<td::MemoryKeyValue>();
  auto dboc = options.create_dboc(kv.get(), {});
  dboc->set_loader(std::make_unique<vm::CellLoader>(kv));
  dboc->inc(usage_cell);
  {
    options.prepare_commit(*dboc);
    vm::CellStorer cell_storer(*kv);
    dboc->commit(cell_storer);
  }

  auto proof = vm::MerkleProof::generate(cell, usage_tree.get());
  auto virtualized_proof = vm::MerkleProof::virtualize(proof, 1);
  auto serialization_of_virtualized_cell = serialize_boc(virtualized_proof);
  auto serialization = serialize_boc(cell);
  ASSERT_STREQ(serialization, serialization_of_virtualized_cell);
  vm::DynamicBagOfCellsDb::Stats stats;
  return stats;
}

TEST(TonDb, DynamicBocRespectsUsageCell) {
  vm::with_all_boc_options(test_dynamic_boc_respects_usage_cell, 20, true);
}
*/

TEST(TonDb, LargeBocSerializer) {
  td::Random::Xorshift128plus rnd{123};
  size_t n = 1000000;
  std::vector<td::uint64> data(n);
  std::iota(data.begin(), data.end(), 0);
  vm::CompactArray arr(data);
  auto root = arr.root();
  std::string path = "serialization";
  td::unlink(path).ignore();
  auto fd = td::FileFd::open(path, td::FileFd::Flags::Create | td::FileFd::Flags::Truncate | td::FileFd::Flags::Write)
                .move_as_ok();
  std_boc_serialize_to_file(root, fd, 31);
  fd.close();
  auto a = td::read_file_str(path).move_as_ok();

  auto kv = std::make_shared<td::MemoryKeyValue>();
  auto dboc = vm::DynamicBagOfCellsDb::create();
  dboc->set_loader(std::make_unique<vm::CellLoader>(kv));
  dboc->inc(root);
  dboc->prepare_commit();
  vm::CellStorer cell_storer(*kv);
  dboc->commit(cell_storer);
  dboc->set_loader(std::make_unique<vm::CellLoader>(kv));
  td::unlink(path).ignore();
  fd = td::FileFd::open(path, td::FileFd::Flags::Create | td::FileFd::Flags::Truncate | td::FileFd::Flags::Write)
           .move_as_ok();
  boc_serialize_to_file_large(dboc->get_cell_db_reader(), root->get_hash(), fd, 31);
  fd.close();
  auto b = td::read_file_str(path).move_as_ok();

  auto a_cell = vm::deserialize_boc(td::BufferSlice(a));
  auto b_cell = vm::deserialize_boc(td::BufferSlice(b));
  ASSERT_EQ(a_cell->get_hash(), b_cell->get_hash());
}

TEST(TonDb, DoNotMakeListsPrunned) {
  auto cell = vm::CellBuilder().store_bytes("abc").finalize();
  auto is_prunned = [&](const td::Ref<vm::Cell> &cell) { return true; };
  auto proof = vm::MerkleProof::generate(cell, is_prunned);
  auto virtualized_proof = vm::MerkleProof::virtualize(proof, 1);
  ASSERT_TRUE(virtualized_proof->get_virtualization() == 0);
}

TEST(TonDb, CellStat) {
  td::Random::Xorshift128plus rnd(123);
  bool with_prunned_branches = true;
  for (int i = 0; i < 1000; i++) {
    auto A = vm::gen_random_cell(100, rnd, with_prunned_branches);
    td::Ref<vm::Cell> B, AB, B_proof;
    std::shared_ptr<vm::CellUsageTree> usage_tree;
    std::tie(B, AB, usage_tree) = gen_merkle_update(A, rnd, with_prunned_branches);
    B_proof = vm::CellSlice(vm::NoVm(), AB).prefetch_ref(1);

    vm::CellStorageStat stat;
    stat.add_used_storage(B);

    vm::NewCellStorageStat new_stat;
    new_stat.add_cell({});
    new_stat.add_cell(B);
    ASSERT_EQ(stat.cells, new_stat.get_stat().cells);
    ASSERT_EQ(stat.bits, new_stat.get_stat().bits);

    vm::CellStorageStat proof_stat;
    proof_stat.add_used_storage(B_proof);

    vm::NewCellStorageStat new_proof_stat;
    new_proof_stat.add_proof(B, usage_tree.get());
    CHECK(new_proof_stat.get_stat().cells == 0);
    CHECK(new_proof_stat.get_proof_stat().cells <= proof_stat.cells);
    //CHECK(new_proof_stat.get_proof_stat().cells + new_proof_stat.get_proof_stat().external_refs >= proof_stat.cells);

    vm::NewCellStorageStat new_all_stat;
    new_all_stat.add_cell_and_proof(B, usage_tree.get());
    CHECK(new_proof_stat.get_proof_stat() == new_all_stat.get_proof_stat());
    CHECK(new_stat.get_stat() == new_all_stat.get_stat());

    stat.add_used_storage(A);
    auto AB_stat = new_stat.get_stat() + const_cast<vm::NewCellStorageStat &>(new_stat).tentative_add_cell(A);
    new_stat.add_cell(A);
    CHECK(AB_stat == new_stat.get_stat());
    ASSERT_EQ(stat.cells, new_stat.get_stat().cells);
    ASSERT_EQ(stat.bits, new_stat.get_stat().bits);

    CHECK(usage_tree.use_count() == 1);
    usage_tree.reset();
    td::Ref<vm::Cell> C, BC, C_proof;
    std::shared_ptr<vm::CellUsageTree> usage_tree_B;
    std::tie(C, BC, usage_tree_B) = gen_merkle_update(B, rnd, with_prunned_branches);
    C_proof = vm::CellSlice(vm::NoVm(), BC).prefetch_ref(1);

    auto BC_proof_stat = new_proof_stat.get_proof_stat() + new_proof_stat.tentative_add_proof(C, usage_tree_B.get());
    new_proof_stat.add_proof(C, usage_tree_B.get());
    CHECK(BC_proof_stat == new_proof_stat.get_proof_stat());
  }
}

struct String {
  String() {
    total_strings.add(1);
  }
  String(std::string str) : str(std::move(str)) {
    total_strings.add(1);
  }
  ~String() {
    total_strings.add(-1);
  }
  static td::ThreadSafeCounter total_strings;
  std::string str;
};

td::ThreadSafeCounter String::total_strings;
TEST(Ref, AtomicRef) {
  struct Node {
    td::AtomicRefLockfree<td::Cnt<String>> name_;
    char pad[64];
  };

  int threads_n = 10;
  std::vector<Node> nodes(threads_n);
  std::vector<td::thread> threads(threads_n);
  for (auto &thread : threads) {
    thread = td::thread([&] {
      for (int i = 0; i < 1000000; i++) {
        auto &node = nodes[td::Random::fast(0, threads_n / 3 - 1)];
        auto name = node.name_.load();
        if (name.not_null()) {
          CHECK(name->str == "one" || name->str == "twotwo");
        }
        if (td::Random::fast(0, 5) == 0) {
          auto new_string = td::Ref<td::Cnt<String>>{true, td::Random::fast(0, 1) == 0 ? "one" : "twotwo"};
          node.name_.store(std::move(new_string));
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  nodes.clear();
  LOG(ERROR) << String::total_strings.sum();
}

//TEST(Tmp, Boc) {
//LOG(ERROR) << "A";
//auto data = td::read_file("boc");
//LOG(ERROR) << "B";
//auto cell = vm::deserialize_boc(data.move_as_ok().as_slice());
//vm::CellStorageStat stat;
//stat.add_used_storage(cell, false);
//LOG(ERROR) << stat.cells;
////LOG(ERROR) << "C";
////auto new_data = vm::serialize_boc(cell);
////LOG(ERROR) << "D";
//vm::test_boc_deserializer<vm::StaticBagOfCellsDbLazy>({cell}, 31);
//}
