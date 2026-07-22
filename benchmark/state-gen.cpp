/*
    bench-state-gen — offline basechain state generator (benchmark/DESIGN.md).

    Subcommands:
      gen         synthesize the state and write a celldb-format RocksDB + manifest.json
      root-only   run derivation + cell synthesis without writing the DB; print root hash
      self-test   correctness backbone (streaming-dict equivalence, TLB validity,
                  celldb round-trip, external message check, python cross-check hash)
      checkpoint  RocksDB checkpoint (hardlink copy) of an existing celldb
*/
#include <array>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <unistd.h>

#include "auto/tl/ton_api.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "common/checksum.h"
#include "common/io.hpp"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/sst_file_writer.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/checkpoint.h"
#include "td/db/RocksDb.h"
#include "td/utils/HashSet.h"
#include "td/utils/OptionParser.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Time.h"
#include "td/utils/Timer.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/misc.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/path.h"
#include "td/utils/port/thread.h"
#include "tl-utils/tl-utils.hpp"
#include "ton/ton-tl.hpp"
#include "vm/db/CellStorage.h"
#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/dict.h"

#include "common.h"

namespace bench {
namespace {

constexpr td::int32 kRefcnt = 1 << 30;

// Expected empty-state root hash for gen_utime=1700000000 (self-test e).
// Produced by the python tontester zerostate builder: a script replicating
// zerostate.py base_state (global_id=-777, wc0 full shard, seqno 0, empty
// accounts/queues, zero balances) with now_time=1700000000 printed this hash;
// the python and C++ values matched on 2026-06-12.
const char *kEmptyStateRootHashHex = "a6a1063927dd1fec6960f194d331835702b32f2dcac02711cd33938abd5917d1";

struct Config {
  td::Bits256 seed = td::sha256_bits256("tonbench-default-seed");
  td::uint64 num_v5 = 1000000;
  td::uint64 num_ballast = 0;
  int ballast_cells = 17;
  td::uint32 wallet_id = 0;
  td::uint32 gen_utime = 0;                   // 0 → now()
  Uint128 v5_balance = 100'000'000'000ULL;    // 100 TON
  Uint128 jw_balance = 1'000'000'000ULL;      // 1 TON
  Uint128 minter_balance = 1'000'000'000ULL;  // 1 TON
  Uint128 jw_jetton_balance = 1'000'000'000'000'000ULL;
  std::string contracts_dir = "benchmark/contracts";
  std::string out_dir;
  std::string tmp_dir;
  int threads = static_cast<int>(td::thread::hardware_concurrency());
  // Bundle ~bundle_depth key-bit levels of the accounts dict (plus the leaf's
  // account/data cells) into one celldb record; 0 disables bundling.
  int bundle_depth = 5;
  bool overwrite = false;
  td::uint64 run_batch_bytes = 512ULL << 20;
  td::uint64 sst_chunk_bytes = 512ULL << 20;

  Uint128 total_supply() const {
    return jw_jetton_balance * num_v5;
  }
  td::uint64 num_accounts() const {
    return 2 * num_v5 + num_ballast + (num_v5 > 0 ? 1 : 0);
  }
};

// ---------------------------------------------------------------------------
// Progress counters
// ---------------------------------------------------------------------------

struct Progress {
  std::atomic<td::uint64> accounts{0};
  std::atomic<td::uint64> cells{0};
  std::atomic<td::uint64> run_bytes{0};
  std::atomic<td::uint64> merged_bytes{0};
  std::atomic<const char *> phase{"init"};
};

class ProgressPrinter {
 public:
  explicit ProgressPrinter(Progress &progress) : progress_(progress) {
    thread_ = td::thread([this] { run(); });
  }
  ~ProgressPrinter() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  void run() {
    auto start = td::Timestamp::now();
    td::uint64 prev_accounts = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      cv_.wait_for(lock, std::chrono::seconds(3));
      if (stop_) {
        break;
      }
      auto accounts = progress_.accounts.load();
      auto elapsed = td::Timestamp::now().at() - start.at();
      LOG(INFO) << "[" << progress_.phase.load() << "] accounts=" << accounts << " (" << (accounts - prev_accounts) / 3
                << "/s) cells=" << progress_.cells.load()
                << " run_bytes=" << td::format::as_size(progress_.run_bytes.load())
                << " merged_bytes=" << td::format::as_size(progress_.merged_bytes.load()) << " elapsed=" << elapsed
                << "s";
      prev_accounts = accounts;
    }
  }
  Progress &progress_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  td::thread thread_;
};

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

// root-only / self-test: count cells, discard contents
class CountingSink : public CellSink {
 public:
  explicit CountingSink(Progress *progress = nullptr) : progress_(progress) {
  }
  void emit(const Ref<vm::DataCell> &cell) override {
    count_++;
    if (progress_ != nullptr) {
      progress_->cells.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void emit_raw(const td::Bits256 &hash, std::string value) override {
    bundles_++;
  }
  td::uint64 count() const {
    return count_;
  }
  td::uint64 bundles() const {
    return bundles_;
  }

 private:
  Progress *progress_;
  td::uint64 count_{0};
  td::uint64 bundles_{0};
};

// Registry of sorted run files produced by all sinks
class RunRegistry {
 public:
  explicit RunRegistry(std::string dir) : dir_(std::move(dir)) {
  }
  std::string next_path() {
    auto idx = counter_.fetch_add(1);
    return PSTRING() << dir_ << "/run-" << idx;
  }
  void register_run(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    runs_.push_back(std::move(path));
  }
  std::vector<std::string> runs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return runs_;
  }

 private:
  std::string dir_;
  std::atomic<td::uint64> counter_{0};
  std::mutex mutex_;
  std::vector<std::string> runs_;
};

// Buffers (hash, serialized value) records, sorts ~run_batch_bytes batches in
// RAM by hash and spills them to run files. Run record: [hash:32][len:u32][value].
class RunFileSink : public CellSink {
 public:
  RunFileSink(RunRegistry &registry, Progress &progress, td::uint64 batch_bytes)
      : registry_(registry), progress_(progress), batch_bytes_(batch_bytes) {
  }
  ~RunFileSink() {
    CHECK(recs_.empty());  // flush() must be called explicitly
  }
  void emit(const Ref<vm::DataCell> &cell) override {
    emit_raw(td::Bits256{cell->get_hash().bits()}, vm::CellStorer::serialize_value(kRefcnt, cell, false));
  }
  void emit_raw(const td::Bits256 &hash, std::string value) override {
    Rec rec;
    rec.hash = hash;
    rec.offset = buf_.size();
    rec.len = td::narrow_cast<td::uint32>(value.size());
    recs_.push_back(rec);
    buf_.append(value);
    progress_.cells.fetch_add(1, std::memory_order_relaxed);
    if (buf_.size() >= batch_bytes_) {
      flush();
    }
  }
  void flush() {
    if (recs_.empty()) {
      return;
    }
    std::sort(recs_.begin(), recs_.end(),
              [](const Rec &a, const Rec &b) { return memcmp(a.hash.data(), b.hash.data(), 32) < 0; });
    auto path = registry_.next_path();
    std::FILE *f = std::fopen(path.c_str(), "wb");
    LOG_CHECK(f != nullptr) << "cannot create run file " << path;
    std::vector<char> io_buf(1 << 22);
    setvbuf(f, io_buf.data(), _IOFBF, io_buf.size());
    for (const auto &rec : recs_) {
      CHECK(std::fwrite(rec.hash.data(), 1, 32, f) == 32);
      CHECK(std::fwrite(&rec.len, 1, 4, f) == 4);
      CHECK(std::fwrite(buf_.data() + rec.offset, 1, rec.len, f) == rec.len);
    }
    CHECK(std::fclose(f) == 0);
    progress_.run_bytes.fetch_add(buf_.size() + recs_.size() * 36, std::memory_order_relaxed);
    registry_.register_run(std::move(path));
    recs_.clear();
    buf_.clear();
    buf_.shrink_to_fit();
  }

 private:
  struct Rec {
    td::Bits256 hash;
    td::uint64 offset;
    td::uint32 len;
  };
  RunRegistry &registry_;
  Progress &progress_;
  td::uint64 batch_bytes_;
  std::vector<Rec> recs_;
  std::string buf_;
};

// ---------------------------------------------------------------------------
// Phase 1: parallel derivation into 256 bucket files
// ---------------------------------------------------------------------------

enum class AccountType : td::uint8 { W5 = 0, JW = 1, Ballast = 2, Minter = 3 };

#pragma pack(push, 1)
struct DeriveRecord {
  unsigned char addr[32];
  td::uint8 type;
  td::uint64 index;
  unsigned char payload[32];  // W5: pubkey; JW: owner (w5) address; else zero
};
#pragma pack(pop)
static_assert(sizeof(DeriveRecord) == 73, "DeriveRecord must be packed");

class BucketWriter {
 public:
  explicit BucketWriter(const std::string &dir) {
    for (int b = 0; b < 256; b++) {
      auto path = bucket_path(dir, b);
      files_[b] = std::fopen(path.c_str(), "wb");
      LOG_CHECK(files_[b] != nullptr) << "cannot create bucket file " << path;
    }
  }
  ~BucketWriter() {
    for (auto *f : files_) {
      if (f != nullptr) {
        std::fclose(f);
      }
    }
  }
  void close() {
    for (auto &f : files_) {
      CHECK(std::fclose(f) == 0);
      f = nullptr;
    }
  }
  static std::string bucket_path(const std::string &dir, int bucket) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02x", bucket);
    return PSTRING() << dir << "/bucket-" << buf << ".rec";
  }
  void write(td::Span<DeriveRecord> records, int bucket) {
    std::lock_guard<std::mutex> lock(mutex_[bucket]);
    CHECK(std::fwrite(records.data(), sizeof(DeriveRecord), records.size(), files_[bucket]) == records.size());
  }

 private:
  std::array<std::FILE *, 256> files_{};
  std::array<std::mutex, 256> mutex_;
};

// Per-worker buffered writer
class BucketBuffer {
 public:
  explicit BucketBuffer(BucketWriter &writer) : writer_(writer) {
    for (auto &b : buf_) {
      b.reserve(kFlushAt);
    }
  }
  ~BucketBuffer() {
    flush_all();
  }
  void add(const DeriveRecord &rec) {
    int bucket = rec.addr[0];
    buf_[bucket].push_back(rec);
    if (buf_[bucket].size() >= kFlushAt) {
      writer_.write(buf_[bucket], bucket);
      buf_[bucket].clear();
    }
  }
  void flush_all() {
    for (int b = 0; b < 256; b++) {
      if (!buf_[b].empty()) {
        writer_.write(buf_[b], b);
        buf_[b].clear();
      }
    }
  }

 private:
  static constexpr size_t kFlushAt = 64;
  BucketWriter &writer_;
  std::array<std::vector<DeriveRecord>, 256> buf_;
};

struct GenContext {
  explicit GenContext(const Config &cfg_) : cfg(cfg_) {
  }
  const Config &cfg;
  ContractSet contracts;
  td::Bits256 minter_addr{};
  // shared stand-ins (cells emitted once globally)
  Ref<vm::Cell> w5_code_standin, jw_code_standin, minter_code_standin, ballast_code_standin, empty_cell_standin;
  Ref<vm::DataCell> ballast_code, empty_cell;
  // storage_used per account shape
  StorageUsedStat w5_used, jw_used, ballast_used, minter_used;
};

td::Result<GenContext> make_gen_context(const Config &cfg) {
  TRY_RESULT(contracts, load_contracts(cfg.contracts_dir));
  GenContext ctx{cfg};
  ctx.contracts = std::move(contracts);
  ctx.ballast_code = build_ballast_code();
  ctx.empty_cell = build_empty_cell();
  ctx.w5_code_standin = make_standin(ctx.contracts.w5_code);
  ctx.jw_code_standin = make_standin(ctx.contracts.jw_code);
  ctx.minter_code_standin = make_standin(ctx.contracts.minter_code);
  ctx.ballast_code_standin = make_standin(ctx.ballast_code);
  ctx.empty_cell_standin = make_standin(ctx.empty_cell);

  auto minter_data = build_minter_data(cfg.total_supply(), ctx.empty_cell, ctx.contracts.minter_code);
  ctx.minter_addr = td::Bits256{build_state_init(ctx.contracts.minter_code, minter_data)->get_hash().bits()};

  // sample accounts (index 0) to compute the per-shape storage stats
  TRY_RESULT(sample, derive_wallet(cfg.seed, 0, cfg.wallet_id, ctx.minter_addr, ctx.contracts));
  auto w5_data = build_w5_data(sample.pubkey, cfg.wallet_id);
  std::vector<Ref<vm::Cell>> w5_roots{ctx.contracts.w5_code, w5_data};
  ctx.w5_used = compute_account_storage_used(cfg.v5_balance, w5_roots);
  auto jw_data = build_jw_data(cfg.jw_jetton_balance, sample.w5_addr, ctx.minter_addr, ctx.contracts.jw_code);
  std::vector<Ref<vm::Cell>> jw_roots{ctx.contracts.jw_code, jw_data};
  ctx.jw_used = compute_account_storage_used(cfg.jw_balance, jw_roots);
  auto ballast_chain = build_ballast_chain(tagged_sha256(cfg.seed, "bl", 0), cfg.ballast_cells);
  std::vector<Ref<vm::Cell>> ballast_roots{ctx.ballast_code, ballast_chain[0]};
  ctx.ballast_used = compute_account_storage_used(cfg.jw_balance, ballast_roots);
  std::vector<Ref<vm::Cell>> minter_roots{ctx.contracts.minter_code, minter_data};
  ctx.minter_used = compute_account_storage_used(cfg.minter_balance, minter_roots);
  return std::move(ctx);
}

void derive_phase(const GenContext &ctx, BucketWriter &writer, Progress &progress) {
  const auto &cfg = ctx.cfg;
  progress.phase.store("phase1-derive");
  constexpr td::uint64 kChunk = 4096;
  std::atomic<td::uint64> next_v5{0};
  std::atomic<td::uint64> next_ballast{0};
  auto worker = [&] {
    BucketBuffer buf(writer);
    while (true) {
      auto begin = next_v5.fetch_add(kChunk);
      if (begin >= cfg.num_v5) {
        break;
      }
      auto end = std::min(begin + kChunk, cfg.num_v5);
      for (td::uint64 i = begin; i < end; i++) {
        auto wallet = derive_wallet(cfg.seed, i, cfg.wallet_id, ctx.minter_addr, ctx.contracts).move_as_ok();
        DeriveRecord w5{};
        td::MutableSlice(w5.addr, 32).copy_from(wallet.w5_addr.as_slice());
        w5.type = static_cast<td::uint8>(AccountType::W5);
        w5.index = i;
        td::MutableSlice(w5.payload, 32).copy_from(wallet.pubkey.as_slice());
        buf.add(w5);
        DeriveRecord jw{};
        td::MutableSlice(jw.addr, 32).copy_from(wallet.jw_addr.as_slice());
        jw.type = static_cast<td::uint8>(AccountType::JW);
        jw.index = i;
        td::MutableSlice(jw.payload, 32).copy_from(wallet.w5_addr.as_slice());
        buf.add(jw);
        progress.accounts.fetch_add(2, std::memory_order_relaxed);
      }
    }
    while (true) {
      auto begin = next_ballast.fetch_add(kChunk);
      if (begin >= cfg.num_ballast) {
        break;
      }
      auto end = std::min(begin + kChunk, cfg.num_ballast);
      for (td::uint64 i = begin; i < end; i++) {
        DeriveRecord rec{};
        auto addr = tagged_sha256(cfg.seed, "bl", i);
        td::MutableSlice(rec.addr, 32).copy_from(addr.as_slice());
        rec.type = static_cast<td::uint8>(AccountType::Ballast);
        rec.index = i;
        buf.add(rec);
        progress.accounts.fetch_add(1, std::memory_order_relaxed);
      }
    }
    buf.flush_all();
  };
  std::vector<td::thread> threads;
  for (int t = 0; t < ctx.cfg.threads; t++) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }
  if (cfg.num_v5 > 0) {
    BucketBuffer buf(writer);
    DeriveRecord rec{};
    td::MutableSlice(rec.addr, 32).copy_from(ctx.minter_addr.as_slice());
    rec.type = static_cast<td::uint8>(AccountType::Minter);
    buf.add(rec);
    buf.flush_all();
    progress.accounts.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// Phase 2: per-bucket streaming subtree build
// ---------------------------------------------------------------------------

// Rebuild the account's cells from a derivation record; emits everything below
// the Account cell, returns the Account cell stand-in + balance. With a tracker,
// the Account cell and its data-root cell are retained so the dict-leaf bundle
// can inline them (shared code cells and deeper ballast chain cells are not
// retained and stay external references).
std::pair<Ref<vm::Cell>, Uint128> build_account_cells(const GenContext &ctx, const DeriveRecord &rec, CellSink &sink,
                                                      BundleTracker *tracker) {
  const auto &cfg = ctx.cfg;
  auto emit_retain_standin = [&](const Ref<vm::DataCell> &cell) {
    sink.emit(cell);
    if (tracker != nullptr) {
      tracker->retain(cell);
    }
    return make_standin(cell);
  };
  td::Bits256 addr;
  addr.as_slice().copy_from(td::Slice(rec.addr, 32));
  td::Bits256 payload;
  payload.as_slice().copy_from(td::Slice(rec.payload, 32));
  Ref<vm::Cell> data;
  Ref<vm::Cell> code;
  Uint128 balance = 0;
  const StorageUsedStat *used = nullptr;
  switch (static_cast<AccountType>(rec.type)) {
    case AccountType::W5:
      data = emit_retain_standin(build_w5_data(payload, cfg.wallet_id));
      code = ctx.w5_code_standin;
      balance = cfg.v5_balance;
      used = &ctx.w5_used;
      break;
    case AccountType::JW:
      data = emit_retain_standin(build_jw_data(cfg.jw_jetton_balance, payload, ctx.minter_addr, ctx.jw_code_standin));
      code = ctx.jw_code_standin;
      balance = cfg.jw_balance;
      used = &ctx.jw_used;
      break;
    case AccountType::Ballast: {
      auto chain = build_ballast_chain(addr, cfg.ballast_cells);
      for (size_t i = 1; i < chain.size(); i++) {
        sink.emit(chain[i]);
      }
      data = emit_retain_standin(chain[0]);
      code = ctx.ballast_code_standin;
      balance = cfg.jw_balance;
      used = &ctx.ballast_used;
      break;
    }
    case AccountType::Minter:
      data = emit_retain_standin(build_minter_data(cfg.total_supply(), ctx.empty_cell_standin, ctx.jw_code_standin));
      code = ctx.minter_code_standin;
      balance = cfg.minter_balance;
      used = &ctx.minter_used;
      break;
    default:
      LOG(FATAL) << "bad account type " << rec.type;
  }
  auto account = build_account(addr, balance, std::move(code), std::move(data), *used, cfg.gen_utime);
  return {emit_retain_standin(account), balance};
}

// Process one bucket file: sort records, build account cells + dict subtree.
DictNode build_bucket(const GenContext &ctx, const std::string &bucket_file, CellSink &sink, BundleTracker *tracker,
                      Progress &progress) {
  auto r_data = td::read_file(bucket_file);
  LOG_CHECK(r_data.is_ok()) << "cannot read " << bucket_file << ": " << r_data.error();
  auto data = r_data.move_as_ok();
  CHECK(data.size() % sizeof(DeriveRecord) == 0);
  size_t n = data.size() / sizeof(DeriveRecord);
  std::vector<DeriveRecord> records(n);
  if (n > 0) {
    memcpy(records.data(), data.data(), data.size());
  }
  data = {};
  std::sort(records.begin(), records.end(),
            [](const DeriveRecord &a, const DeriveRecord &b) { return memcmp(a.addr, b.addr, 32) < 0; });
  ShardAccountsStreamBuilder builder(sink, tracker);
  for (const auto &rec : records) {
    auto [account, balance] = build_account_cells(ctx, rec, sink, tracker);
    td::Bits256 addr;
    addr.as_slice().copy_from(td::Slice(rec.addr, 32));
    builder.add_account(addr, std::move(account), balance);
    progress.accounts.fetch_add(1, std::memory_order_relaxed);
  }
  return builder.finish();
}

// ---------------------------------------------------------------------------
// Phase 3: k-way merge of run files → SST chunks → RocksDB ingest
// ---------------------------------------------------------------------------

class RunCursor {
 public:
  explicit RunCursor(const std::string &path) {
    f_ = std::fopen(path.c_str(), "rb");
    LOG_CHECK(f_ != nullptr) << "cannot open run file " << path;
    io_buf_.resize(1 << 22);
    setvbuf(f_, io_buf_.data(), _IOFBF, io_buf_.size());
    advance();
  }
  ~RunCursor() {
    if (f_ != nullptr) {
      std::fclose(f_);
    }
  }
  RunCursor(const RunCursor &) = delete;
  bool ok() const {
    return ok_;
  }
  const td::Bits256 &hash() const {
    return hash_;
  }
  const std::string &value() const {
    return value_;
  }
  void advance() {
    auto got = std::fread(hash_.as_slice().begin(), 1, 32, f_);
    if (got == 0) {
      ok_ = false;
      return;
    }
    CHECK(got == 32);
    td::uint32 len;
    CHECK(std::fread(&len, 1, 4, f_) == 4);
    value_.resize(len);
    CHECK(std::fread(value_.data(), 1, len, f_) == len);
    ok_ = true;
  }

 private:
  std::FILE *f_{nullptr};
  std::vector<char> io_buf_;
  td::Bits256 hash_;
  std::string value_;
  bool ok_{false};
};

rocksdb::Options make_celldb_options() {
  rocksdb::Options options;
  options.create_if_missing = true;
  rocksdb::BlockBasedTableOptions table_options;
  // mirrors td::RocksDb::open() (tddb/td/db/RocksDb.cpp) with bloom filters on
  table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
  table_options.index_type = rocksdb::BlockBasedTableOptions::IndexType::kTwoLevelIndexSearch;
  table_options.partition_filters = true;
  table_options.cache_index_and_filter_blocks = true;
  table_options.pin_l0_filter_and_index_blocks_in_cache = true;
  options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
  options.compression = rocksdb::kNoCompression;
  return options;
}

struct MergeStats {
  td::uint64 distinct_cells{0};
  td::uint64 value_bytes{0};
  std::vector<std::string> sst_files;
};

MergeStats merge_runs_to_sst(const std::vector<std::string> &runs, const std::string &sst_dir,
                             td::uint64 sst_chunk_bytes, Progress &progress) {
  progress.phase.store("phase3-merge");
  std::vector<std::unique_ptr<RunCursor>> cursors;
  for (const auto &path : runs) {
    auto cur = std::make_unique<RunCursor>(path);
    if (cur->ok()) {
      cursors.push_back(std::move(cur));
    }
  }
  auto cmp = [&](size_t a, size_t b) {
    int c = memcmp(cursors[a]->hash().data(), cursors[b]->hash().data(), 32);
    if (c != 0) {
      return c > 0;  // min-heap
    }
    return a > b;
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> heap(cmp);
  for (size_t i = 0; i < cursors.size(); i++) {
    heap.push(i);
  }

  MergeStats stats;
  auto options = make_celldb_options();
  std::unique_ptr<rocksdb::SstFileWriter> writer;
  td::uint64 chunk_bytes = 0;
  auto open_writer = [&] {
    auto path = PSTRING() << sst_dir << "/chunk-" << stats.sst_files.size() << ".sst";
    writer = std::make_unique<rocksdb::SstFileWriter>(rocksdb::EnvOptions(), options);
    auto status = writer->Open(path);
    LOG_CHECK(status.ok()) << "SstFileWriter::Open " << path << ": " << status.ToString();
    stats.sst_files.push_back(path);
    chunk_bytes = 0;
  };
  auto close_writer = [&] {
    if (writer != nullptr) {
      auto status = writer->Finish();
      LOG_CHECK(status.ok()) << "SstFileWriter::Finish: " << status.ToString();
      writer = nullptr;
    }
  };

  bool have_prev = false;
  td::Bits256 prev_hash;
  std::string prev_value;
  auto emit_kv = [&](const td::Bits256 &hash, const std::string &value) {
    if (writer == nullptr || chunk_bytes >= sst_chunk_bytes) {
      close_writer();
      open_writer();
    }
    auto status = writer->Put(rocksdb::Slice(hash.as_slice().data(), 32), value);
    LOG_CHECK(status.ok()) << "SstFileWriter::Put: " << status.ToString();
    chunk_bytes += 32 + value.size();
    stats.distinct_cells++;
    stats.value_bytes += value.size();
    progress.merged_bytes.fetch_add(32 + value.size(), std::memory_order_relaxed);
  };
  auto is_bundle_value = [](const std::string &value) {
    return value.size() >= 4 && td::as<td::int32>(value.data()) == vm::CellStorer::kBundleTag;
  };
  while (!heap.empty()) {
    auto idx = heap.top();
    heap.pop();
    auto &cur = *cursors[idx];
    if (have_prev && cur.hash() == prev_hash) {
      if (cur.value() != prev_value) {
        // A bundle-root hash carries both its plain record (emitted at cell
        // materialization) and its bundle record (emitted at close-out); the
        // bundle wins. Anything else is a genuine collision.
        bool prev_bundle = is_bundle_value(prev_value);
        bool cur_bundle = is_bundle_value(cur.value());
        LOG_CHECK(prev_bundle != cur_bundle) << "hash collision with different values: " << prev_hash.to_hex();
        if (cur_bundle) {
          prev_value = cur.value();
        }
      }
    } else {
      if (have_prev) {
        emit_kv(prev_hash, prev_value);
      }
      prev_hash = cur.hash();
      prev_value = cur.value();
      have_prev = true;
    }
    cur.advance();
    if (cur.ok()) {
      heap.push(idx);
    }
  }
  if (have_prev) {
    emit_kv(prev_hash, prev_value);
  }
  close_writer();
  return stats;
}

void write_celldb(const std::string &celldb_path, const MergeStats &stats, const td::Bits256 &root_hash,
                  const td::Bits256 &file_hash) {
  auto options = make_celldb_options();
  rocksdb::DB *raw_db = nullptr;
  auto status = rocksdb::DB::Open(options, celldb_path, &raw_db);
  LOG_CHECK(status.ok()) << "rocksdb::DB::Open " << celldb_path << ": " << status.ToString();
  std::unique_ptr<rocksdb::DB> db(raw_db);

  if (!stats.sst_files.empty()) {
    rocksdb::IngestExternalFileOptions ifo;
    ifo.move_files = true;
    status = db->IngestExternalFile(stats.sst_files, ifo);
    LOG_CHECK(status.ok()) << "IngestExternalFile: " << status.ToString();
  }

  // Meta entries, exactly as validator/db/celldb.cpp get_key()/set_block() write them.
  ton::BlockIdExt block_id{0, 0x8000000000000000ULL, 0, ton::RootHash{root_hash}, ton::FileHash{file_hash}};
  auto z = ton::get_tl_object_sha_bits256(ton::create_tl_block_id(block_id));
  ton::BlockIdExt empty_block_id{ton::workchainInvalid, 0, 0, ton::RootHash::zero(), ton::FileHash::zero()};

  auto desczero_value = ton::create_serialize_tl_object<ton::ton_api::db_celldb_value>(
      ton::create_tl_block_id(empty_block_id), z, z, td::Bits256::zero());
  auto desc_value = ton::create_serialize_tl_object<ton::ton_api::db_celldb_value>(
      ton::create_tl_block_id(block_id), td::Bits256::zero(), td::Bits256::zero(), root_hash);
  std::string desc_key = PSTRING() << "desc" << z;

  rocksdb::WriteOptions wo;
  status = db->Put(wo, "desczero", rocksdb::Slice(desczero_value.as_slice().data(), desczero_value.size()));
  LOG_CHECK(status.ok()) << "Put desczero: " << status.ToString();
  status = db->Put(wo, desc_key, rocksdb::Slice(desc_value.as_slice().data(), desc_value.size()));
  LOG_CHECK(status.ok()) << "Put " << desc_key << ": " << status.ToString();
  status = db->Flush(rocksdb::FlushOptions());
  LOG_CHECK(status.ok()) << "Flush: " << status.ToString();
  status = db->Close();
  LOG_CHECK(status.ok()) << "Close: " << status.ToString();
}

// ---------------------------------------------------------------------------
// Full pipeline
// ---------------------------------------------------------------------------

struct GenResult {
  td::Bits256 root_hash;
  td::Bits256 file_hash;
  Uint128 total_balance{0};
  td::uint64 emitted_cells{0};
  td::uint64 distinct_cells{0};
  td::uint64 value_bytes{0};
  Manifest manifest;
};

td::Result<GenResult> run_pipeline(Config cfg, bool write_db) {
  td::Timer timer;
  if (cfg.gen_utime == 0) {
    cfg.gen_utime = static_cast<td::uint32>(td::Clocks::system());
  }
  if (cfg.threads <= 0) {
    cfg.threads = 1;
  }
  if (write_db) {
    if (cfg.out_dir.empty()) {
      return td::Status::Error("--out-dir is required for gen");
    }
    if (cfg.tmp_dir.empty()) {
      cfg.tmp_dir = cfg.out_dir + "/tmp";
    }
  } else if (cfg.tmp_dir.empty()) {
    cfg.tmp_dir = PSTRING() << "/tmp/bench-state-gen." << getpid();
  }
  std::string celldb_path = cfg.out_dir + "/celldb";
  std::string sst_dir = cfg.tmp_dir + "/sst";
  std::string bucket_dir = cfg.tmp_dir + "/buckets";
  std::string run_dir = cfg.tmp_dir + "/runs";
  if (write_db) {
    if (td::stat(celldb_path).is_ok()) {
      if (!cfg.overwrite) {
        return td::Status::Error(PSTRING() << celldb_path << " already exists (use --overwrite)");
      }
      TRY_STATUS(td::rmrf(celldb_path));
    }
    TRY_STATUS(td::mkpath(celldb_path + "/"));
    TRY_STATUS(td::rmrf(celldb_path));
  }
  for (auto &dir : {bucket_dir, run_dir, sst_dir}) {
    if (td::stat(dir).is_ok()) {
      TRY_STATUS(td::rmrf(dir));
    }
    TRY_STATUS(td::mkpath(dir + "/"));
  }

  TRY_RESULT(ctx, make_gen_context(cfg));
  Progress progress;
  ProgressPrinter printer(progress);

  // ---- phase 1 ----
  {
    BucketWriter writer(bucket_dir);
    derive_phase(ctx, writer, progress);
    writer.close();
  }
  LOG(INFO) << "phase 1 (derivation) done in " << timer.elapsed() << "s";

  // ---- phase 2 ----
  progress.phase.store("phase2-build");
  progress.accounts.store(0);
  RunRegistry registry(run_dir);
  std::array<DictNode, 256> pendings;
  // Retained cells whose bundle root lies above the bucket root (handed over to
  // the top-level builder's tracker); empty when bundling is off.
  std::array<std::vector<Ref<vm::DataCell>>, 256> bundle_leftovers;
  {
    std::atomic<int> next_bucket{0};
    auto worker = [&] {
      std::unique_ptr<RunFileSink> run_sink;
      std::unique_ptr<CountingSink> count_sink;
      CellSink *sink;
      if (write_db) {
        run_sink = std::make_unique<RunFileSink>(registry, progress, cfg.run_batch_bytes);
        sink = run_sink.get();
      } else {
        count_sink = std::make_unique<CountingSink>(&progress);
        sink = count_sink.get();
      }
      while (true) {
        int bucket = next_bucket.fetch_add(1);
        if (bucket >= 256) {
          break;
        }
        std::unique_ptr<BundleTracker> tracker;
        if (cfg.bundle_depth > 0) {
          tracker = std::make_unique<BundleTracker>(*sink, cfg.bundle_depth, kRefcnt);
        }
        pendings[bucket] =
            build_bucket(ctx, BucketWriter::bucket_path(bucket_dir, bucket), *sink, tracker.get(), progress);
        if (tracker != nullptr) {
          bundle_leftovers[bucket] = tracker->take_retained();
        }
      }
      if (run_sink != nullptr) {
        run_sink->flush();
      }
    };
    std::vector<td::thread> threads;
    for (int t = 0; t < cfg.threads; t++) {
      threads.emplace_back(worker);
    }
    for (auto &t : threads) {
      t.join();
    }
  }

  // ---- top 8 levels + header (main thread) ----
  GenResult res;
  {
    std::unique_ptr<CellSink> main_sink;
    RunFileSink *main_run_sink = nullptr;
    if (write_db) {
      auto s = std::make_unique<RunFileSink>(registry, progress, cfg.run_batch_bytes);
      main_run_sink = s.get();
      main_sink = std::move(s);
    } else {
      main_sink = std::make_unique<CountingSink>(&progress);
    }
    // shared cells, emitted once globally
    if (cfg.num_v5 > 0) {
      emit_subtree(*main_sink, ctx.contracts.w5_code);
      emit_subtree(*main_sink, ctx.contracts.jw_code);
      emit_subtree(*main_sink, ctx.contracts.minter_code);
      main_sink->emit(ctx.empty_cell);
    }
    if (cfg.num_ballast > 0) {
      main_sink->emit(ctx.ballast_code);
    }
    std::unique_ptr<BundleTracker> top_tracker;
    if (cfg.bundle_depth > 0) {
      top_tracker = std::make_unique<BundleTracker>(*main_sink, cfg.bundle_depth, kRefcnt);
      for (auto &leftovers : bundle_leftovers) {
        top_tracker->adopt(std::move(leftovers));
      }
    }
    ShardAccountsStreamBuilder top_builder(*main_sink, top_tracker.get());
    for (int b = 0; b < 256; b++) {
      if (pendings[b].type != DictNode::Type::Empty) {
        top_builder.add_subtree(std::move(pendings[b]));
      }
    }
    auto root_node = top_builder.finish();
    Ref<vm::Cell> dict_root;
    if (root_node.type != DictNode::Type::Empty) {
      res.total_balance = root_node.balance;
      dict_root = materialize_dict_node(*main_sink, root_node, 0, top_tracker.get());
      if (top_tracker != nullptr) {
        // the dict root is always a bundle root
        top_tracker->close_out(dict_root->get_hash());
      }
    }
    if (top_tracker != nullptr) {
      // every dict node / account cell must have been claimed by exactly one bundle
      LOG_CHECK(top_tracker->retained_count() == 0)
          << "bundle tracker leak: " << top_tracker->retained_count() << " cells never claimed";
    }
    auto state_root = build_shard_state_root(*main_sink, dict_root, res.total_balance, cfg.gen_utime);
    res.root_hash = td::Bits256{state_root->get_hash().bits()};
    res.file_hash = fake_file_hash(res.root_hash);
    if (main_run_sink != nullptr) {
      main_run_sink->flush();
    }
  }
  res.emitted_cells = progress.cells.load();
  LOG(INFO) << "phase 2 (cell build) done in " << timer.elapsed() << "s, root hash " << res.root_hash.to_hex();

  res.manifest.seed = cfg.seed;
  res.manifest.gen_utime = cfg.gen_utime;
  res.manifest.root_hash = res.root_hash;
  res.manifest.file_hash = res.file_hash;
  res.manifest.total_balance = res.total_balance;
  res.manifest.num_v5 = cfg.num_v5;
  res.manifest.num_ballast = cfg.num_ballast;
  res.manifest.ballast_cells = cfg.ballast_cells;
  res.manifest.wallet_id = cfg.wallet_id;
  res.manifest.w5_code_hash = td::Bits256{ctx.contracts.w5_code->get_hash().bits()};
  res.manifest.jw_code_hash = td::Bits256{ctx.contracts.jw_code->get_hash().bits()};
  res.manifest.minter_addr = ctx.minter_addr;
  res.manifest.v5_balance = cfg.v5_balance;
  res.manifest.jw_balance = cfg.jw_balance;
  res.manifest.jw_jetton_balance = cfg.jw_jetton_balance;
  res.manifest.celldb_path = celldb_path;

  if (!write_db) {
    TRY_STATUS(td::rmrf(cfg.tmp_dir));
    return res;
  }

  // ---- phase 3 ----
  auto stats = merge_runs_to_sst(registry.runs(), sst_dir, cfg.sst_chunk_bytes, progress);
  res.distinct_cells = stats.distinct_cells;
  res.value_bytes = stats.value_bytes;
  LOG(INFO) << "phase 3 (merge) done in " << timer.elapsed() << "s: " << stats.distinct_cells << " cells, "
            << td::format::as_size(stats.value_bytes) << " of values, " << stats.sst_files.size() << " sst chunks";
  progress.phase.store("phase3-ingest");
  write_celldb(celldb_path, stats, res.root_hash, res.file_hash);

  TRY_STATUS(td::write_file(cfg.out_dir + "/manifest.json", res.manifest.to_json()));

  // verify: the DB opens and the root cell loads via vm::CellLoader
  {
    TRY_RESULT(kv, td::RocksDb::open(celldb_path));
    auto boc = vm::DynamicBagOfCellsDb::create();
    boc->set_loader(std::make_unique<vm::CellLoader>(std::shared_ptr<td::KeyValueReader>(kv.snapshot().release())));
    TRY_RESULT(root, boc->load_root(res.root_hash.as_slice()));
    CHECK(root->get_hash().as_slice() == res.root_hash.as_slice());
  }

  // cleanup run/bucket files
  TRY_STATUS(td::rmrf(bucket_dir));
  TRY_STATUS(td::rmrf(run_dir));
  TRY_STATUS(td::rmrf(sst_dir));

  LOG(INFO) << "gen done in " << timer.elapsed() << "s; root_hash=" << res.root_hash.to_hex()
            << " file_hash=" << res.file_hash.to_hex() << " total_balance=" << u128_to_dec(res.total_balance);
  return res;
}

// ---------------------------------------------------------------------------
// checkpoint subcommand
// ---------------------------------------------------------------------------

td::Status do_checkpoint(const std::string &src, const std::string &dst) {
  auto options = make_celldb_options();
  options.create_if_missing = false;
  rocksdb::DB *raw_db = nullptr;
  auto status = rocksdb::DB::Open(options, src, &raw_db);
  if (!status.ok()) {
    return td::Status::Error(PSTRING() << "cannot open " << src << ": " << status.ToString());
  }
  std::unique_ptr<rocksdb::DB> db(raw_db);
  rocksdb::Checkpoint *raw_cp = nullptr;
  status = rocksdb::Checkpoint::Create(db.get(), &raw_cp);
  if (!status.ok()) {
    return td::Status::Error(PSTRING() << "Checkpoint::Create: " << status.ToString());
  }
  std::unique_ptr<rocksdb::Checkpoint> cp(raw_cp);
  status = cp->CreateCheckpoint(dst);
  if (!status.ok()) {
    return td::Status::Error(PSTRING() << "CreateCheckpoint " << dst << ": " << status.ToString());
  }
  LOG(INFO) << "checkpoint " << src << " -> " << dst << " done";
  return td::Status::OK();
}

// ---------------------------------------------------------------------------
// self-test
// ---------------------------------------------------------------------------

struct TestAccount {
  td::Bits256 addr;
  Ref<vm::DataCell> account;  // real (loadable) cells
  Uint128 balance{0};
};

// Mixed-type accounts with the real builders; pseudorandom addrs/keys.
std::vector<TestAccount> make_test_accounts(const GenContext &ctx, size_t count) {
  const auto &cfg = ctx.cfg;
  std::vector<TestAccount> res(count);
  for (size_t i = 0; i < count; i++) {
    auto &acc = res[i];
    acc.addr = tagged_sha256(cfg.seed, "ta", i);
    Ref<vm::Cell> code, data;
    const StorageUsedStat *used = nullptr;
    switch (i % 4) {
      case 0:
        code = ctx.contracts.w5_code;
        data = build_w5_data(tagged_sha256(cfg.seed, "tk", i), cfg.wallet_id);
        acc.balance = cfg.v5_balance;
        used = &ctx.w5_used;
        break;
      case 1:
        code = ctx.contracts.jw_code;
        data = build_jw_data(cfg.jw_jetton_balance, tagged_sha256(cfg.seed, "to", i), ctx.minter_addr,
                             ctx.contracts.jw_code);
        acc.balance = cfg.jw_balance;
        used = &ctx.jw_used;
        break;
      case 2:
        code = ctx.ballast_code;
        data = build_ballast_chain(acc.addr, cfg.ballast_cells)[0];
        acc.balance = cfg.jw_balance;
        used = &ctx.ballast_used;
        break;
      default:
        code = ctx.contracts.minter_code;
        data = build_minter_data(cfg.total_supply(), ctx.empty_cell, ctx.contracts.jw_code);
        acc.balance = cfg.minter_balance;
        used = &ctx.minter_used;
        break;
    }
    acc.account = build_account(acc.addr, acc.balance, std::move(code), std::move(data), *used, cfg.gen_utime);
  }
  std::sort(res.begin(), res.end(), [](const TestAccount &a, const TestAccount &b) { return a.addr < b.addr; });
  return res;
}

Ref<vm::CellSlice> shard_account_value(const Ref<vm::Cell> &account) {
  vm::CellBuilder cb;
  cb.store_ref(account);
  cb.store_bits_same(256, false);
  cb.store_long(0, 64);
  return cb.as_cellslice_ref();
}

void self_test_streaming(const GenContext &ctx) {
  for (size_t size : {0, 1, 2, 3, 17, 1000, 50000}) {
    auto accounts = make_test_accounts(ctx, size);

    // reference: vm::AugmentedDictionary with block::tlb::aug_ShardAccounts
    vm::AugmentedDictionary ref_dict{256, block::tlb::aug_ShardAccounts};
    Uint128 total_balance = 0;
    for (auto &acc : accounts) {
      CHECK(ref_dict.set(acc.addr.bits(), 256, shard_account_value(acc.account), vm::Dictionary::SetMode::Add));
      total_balance += acc.balance;
    }
    auto ref_root = ref_dict.get_root_cell();

    // streaming, single builder
    CountingSink sink;
    ShardAccountsStreamBuilder builder(sink);
    for (auto &acc : accounts) {
      builder.add_account(acc.addr, make_standin(acc.account), acc.balance);
    }
    auto node = builder.finish();
    Ref<vm::Cell> stream_root;
    if (node.type != DictNode::Type::Empty) {
      CHECK(node.balance == total_balance);
      stream_root = materialize_dict_node(sink, node, 0);
    }

    // streaming, bucketed by addr[0] + top-level combine (exercises label re-rooting)
    std::array<DictNode, 256> pendings;
    {
      size_t i = 0;
      for (int b = 0; b < 256; b++) {
        ShardAccountsStreamBuilder bucket_builder(sink);
        while (i < accounts.size() && accounts[i].addr.as_slice().ubegin()[0] == static_cast<unsigned char>(b)) {
          bucket_builder.add_account(accounts[i].addr, make_standin(accounts[i].account), accounts[i].balance);
          i++;
        }
        pendings[b] = bucket_builder.finish();
      }
      CHECK(i == accounts.size());
    }
    ShardAccountsStreamBuilder top_builder(sink);
    for (int b = 0; b < 256; b++) {
      if (pendings[b].type != DictNode::Type::Empty) {
        top_builder.add_subtree(std::move(pendings[b]));
      }
    }
    auto top_node = top_builder.finish();
    Ref<vm::Cell> bucketed_root;
    if (top_node.type != DictNode::Type::Empty) {
      bucketed_root = materialize_dict_node(sink, top_node, 0);
    }

    if (size == 0) {
      CHECK(ref_root.is_null() && stream_root.is_null() && bucketed_root.is_null());
    } else {
      CHECK(ref_root.not_null() && stream_root.not_null() && bucketed_root.not_null());
      LOG_CHECK(stream_root->get_hash() == ref_root->get_hash())
          << "size " << size << ": streaming root " << stream_root->get_hash().to_hex() << " != reference "
          << ref_root->get_hash().to_hex();
      LOG_CHECK(bucketed_root->get_hash() == ref_root->get_hash())
          << "size " << size << ": bucketed root " << bucketed_root->get_hash().to_hex() << " != reference "
          << ref_root->get_hash().to_hex();
    }

    // wrapper cell (HashmapAugE) — checks root extra (total balance) too
    auto ref_wrap = ref_dict.get_wrapped_dict_root();
    auto our_wrap = build_shard_accounts_cell(stream_root, total_balance);
    LOG_CHECK(our_wrap->get_hash() == ref_wrap->get_hash()) << "size " << size << ": ShardAccounts wrapper differs";

    // TLB validity of the assembled state (real cells, same hashes as streaming)
    CountingSink hsink;
    auto state_root = build_shard_state_root(hsink, ref_dict.get_root_cell(), total_balance, ctx.cfg.gen_utime);
    LOG_CHECK(block::gen::t_ShardStateUnsplit.validate_ref(10000000, state_root))
        << "size " << size << ": ShardStateUnsplit TLB validation failed";
    if (size > 0) {
      LOG_CHECK(block::gen::t_Account.validate_ref(10000000, accounts[0].account))
          << "size " << size << ": Account TLB validation failed";
    }
    LOG(INFO) << "self-test (a/b) size " << size << ": OK"
              << (size ? PSTRING() << " (root " << stream_root->get_hash().to_hex() << ")" : "");
  }
}

class StandinCellCreator : public vm::ExtCellCreator {
 public:
  td::Result<Ref<vm::Cell>> ext_cell(vm::Cell::LevelMask level_mask, td::Slice hash, td::Slice depth) override {
    TRY_RESULT(cell, StandinCell::create(vm::PrunnedCellInfo{level_mask, hash, depth}, StandinExtra{}));
    return Ref<vm::Cell>(std::move(cell));
  }
};

// Walk the accounts dictionary from the state root down to wallet `addr`'s
// account data cell, loading by hash exactly where an ExtCell boundary would
// force a DB read. Returns {db_loads, cells_visited}.
std::pair<td::uint64, td::uint64> count_descent_loads(vm::CellLoader &loader, vm::ExtCellCreator &creator,
                                                      const td::Bits256 &root_hash, const td::Bits256 &addr) {
  td::uint64 loads = 0;
  td::uint64 cells = 0;
  auto resolve = [&](const Ref<vm::Cell> &cell) -> Ref<vm::DataCell> {
    cells++;
    if (cell->is_loaded()) {
      return cell->load_cell().move_as_ok().data_cell;
    }
    loads++;
    auto r = loader.load(cell->get_hash().as_slice(), true, creator).move_as_ok();
    LOG_CHECK(r.status == vm::CellLoader::LoadResult::Ok) << "missing cell " << cell->get_hash().to_hex();
    return r.cell();
  };
  // the state root itself
  loads++;
  cells++;
  auto root = loader.load(root_hash.as_slice(), true, creator).move_as_ok().cell();
  auto accounts_wrap = resolve(root->get_ref(1));  // ^ShardAccounts
  vm::CellSlice wrap_cs(vm::NoVm{}, accounts_wrap);
  CHECK(wrap_cs.fetch_ulong(1) == 1);  // ahme_root$1
  auto cur = resolve(wrap_cs.fetch_ref());
  int pos = 0;
  while (true) {
    vm::CellSlice cs(vm::NoVm{}, cur);
    int rem = 256 - pos;
    int k = 32 - td::count_leading_zeroes32(rem);
    int len;
    if (cs.fetch_ulong(1) == 0) {  // hml_short$0: unary length
      len = 0;
      while (cs.fetch_ulong(1) == 1) {
        len++;
      }
      cs.advance(len);
    } else if (cs.fetch_ulong(1) == 0) {  // hml_long$10
      len = static_cast<int>(cs.fetch_ulong(k));
      cs.advance(len);
    } else {  // hml_same$11
      cs.advance(1);
      len = static_cast<int>(cs.fetch_ulong(k));
    }
    pos += len;
    CHECK(pos <= 256);
    if (pos == 256) {
      break;  // leaf
    }
    bool bit = addr.bits()[pos];
    pos++;
    auto left = cs.fetch_ref();
    auto right = cs.fetch_ref();
    cur = resolve(bit ? right : left);
  }
  // leaf: extra:DepthBalanceInfo then account:^Account; Account ref1 = data root
  vm::CellSlice leaf_cs(vm::NoVm{}, cur);
  auto account = resolve(leaf_cs.fetch_ref());
  CHECK(account->get_refs_cnt() == 2);
  resolve(account->get_ref(1));
  return {loads, cells};
}

td::Bits256 self_test_celldb(const Config &base_cfg, int bundle_depth) {
  Config cfg = base_cfg;
  cfg.num_v5 = 2000;
  cfg.num_ballast = 100;
  cfg.bundle_depth = bundle_depth;
  cfg.out_dir = PSTRING() << "/tmp/bench-state-gen-selftest." << getpid();
  cfg.tmp_dir = cfg.out_dir + "/tmp";
  cfg.overwrite = true;
  cfg.run_batch_bytes = 8 << 20;  // exercise multi-run merge
  cfg.sst_chunk_bytes = 4 << 20;  // exercise multi-chunk sst
  SCOPE_EXIT {
    td::rmrf(cfg.out_dir).ignore();
  };
  auto res = run_pipeline(cfg, true).move_as_ok();
  CHECK(res.distinct_cells > 0);

  auto kv = td::RocksDb::open(cfg.out_dir + "/celldb").move_as_ok();
  std::shared_ptr<td::KeyValueReader> reader(kv.snapshot().release());
  vm::CellLoader loader(reader);
  StandinCellCreator creator;

  // full traversal: load every cell by hash, verify hashes + refcnt; for bundle
  // records additionally verify the materialized slab byte-matches the
  // standalone plain records of the same cells
  td::uint64 visited = 0;
  td::uint64 bundle_records = 0;
  td::uint64 bundled_cells = 0;
  td::HashSet<vm::CellHash> seen;
  std::vector<td::Bits256> stack{res.root_hash};
  while (!stack.empty()) {
    auto hash = stack.back();
    stack.pop_back();
    if (!seen.insert(vm::CellHash::from_slice(hash.as_slice())).second) {
      continue;
    }
    auto load_res = loader.load(hash.as_slice(), true, creator).move_as_ok();
    LOG_CHECK(load_res.status == vm::CellLoader::LoadResult::Ok) << "missing cell " << hash.to_hex();
    CHECK(load_res.refcnt() == kRefcnt);
    auto cell = load_res.cell();
    CHECK(cell->get_hash().as_slice() == hash.as_slice());
    visited++;
    for (unsigned i = 0; i < cell->get_refs_cnt(); i++) {
      stack.push_back(td::Bits256{cell->get_ref(i)->get_hash().bits()});
    }
    if (load_res.stored_bundle_) {
      bundle_records++;
      // walk the materialized slab (loaded children; ext cells mark the cut)
      td::HashSet<vm::CellHash> slab_seen;
      std::vector<Ref<vm::DataCell>> slab_stack{cell};
      while (!slab_stack.empty()) {
        auto parent = std::move(slab_stack.back());
        slab_stack.pop_back();
        for (unsigned i = 0; i < parent->get_refs_cnt(); i++) {
          auto child = parent->get_ref(i);
          if (!child->is_loaded() || !slab_seen.insert(child->get_hash()).second) {
            continue;
          }
          auto in_bundle = child->load_cell().move_as_ok().data_cell;
          bundled_cells++;
          // interior slab cells must also exist as standalone plain records
          auto standalone_res = loader.load(child->get_hash().as_slice(), true, creator).move_as_ok();
          LOG_CHECK(standalone_res.status == vm::CellLoader::LoadResult::Ok)
              << "missing standalone record for bundled cell " << child->get_hash().to_hex();
          CHECK(!standalone_res.stored_bundle_);
          auto standalone = standalone_res.cell();
          CHECK(standalone->get_hash() == in_bundle->get_hash());
          CHECK(standalone->get_bits() == in_bundle->get_bits());
          CHECK(memcmp(standalone->get_data(), in_bundle->get_data(), (in_bundle->get_bits() + 7) / 8) == 0);
          CHECK(standalone->get_refs_cnt() == in_bundle->get_refs_cnt());
          for (unsigned r = 0; r < in_bundle->get_refs_cnt(); r++) {
            CHECK(standalone->get_ref(r)->get_hash() == in_bundle->get_ref(r)->get_hash());
          }
          slab_stack.push_back(std::move(in_bundle));
        }
      }
    }
  }
  LOG_CHECK(visited == res.distinct_cells) << "traversed " << visited << " cells, db has " << res.distinct_cells;
  if (bundle_depth == 0) {
    CHECK(bundle_records == 0);
  } else {
    CHECK(bundle_records > 0);
    CHECK(bundled_cells > bundle_records);  // slabs actually contain inlined cells
  }

  // dictionary descent cost: with bundling, descending to an account's data cell
  // must take far fewer DB reads than the number of cells on the path
  {
    auto ctx = make_gen_context(cfg).move_as_ok();
    td::uint64 total_loads = 0;
    td::uint64 total_cells = 0;
    for (td::uint64 i = 0; i < 8; i++) {
      auto wallet = derive_wallet(cfg.seed, i, cfg.wallet_id, ctx.minter_addr, ctx.contracts).move_as_ok();
      auto [loads, cells] = count_descent_loads(loader, creator, res.root_hash, wallet.w5_addr);
      total_loads += loads;
      total_cells += cells;
    }
    if (bundle_depth == 0) {
      CHECK(total_loads == total_cells);
    } else {
      LOG_CHECK(2 * total_loads < total_cells)
          << "bundled descent too expensive: " << total_loads << " loads for " << total_cells << " cells";
    }
    LOG(INFO) << "self-test (c) descent cost (bundle_depth=" << bundle_depth << "): " << total_loads << " loads / "
              << total_cells << " cells over 8 descents";
  }

  // V2 dynamic BoC reader (the validator's celldb path): full DFS through the
  // shared cache, ext cells at bundle cuts load transparently
  {
    auto boc = vm::DynamicBagOfCellsDb::create_v2({.extra_threads = 0});
    boc->set_loader(std::make_unique<vm::CellLoader>(reader));
    auto root = boc->load_root(res.root_hash.as_slice()).move_as_ok();
    td::uint64 v2_visited = 0;
    td::HashSet<vm::CellHash> v2_seen;
    std::vector<Ref<vm::Cell>> v2_stack{root};
    while (!v2_stack.empty()) {
      auto cell = std::move(v2_stack.back());
      v2_stack.pop_back();
      if (!v2_seen.insert(cell->get_hash()).second) {
        continue;
      }
      auto loaded = cell->load_cell().move_as_ok().data_cell;
      CHECK(loaded->get_hash() == cell->get_hash());
      v2_visited++;
      for (unsigned i = 0; i < loaded->get_refs_cnt(); i++) {
        v2_stack.push_back(loaded->get_ref(i));
      }
    }
    LOG_CHECK(v2_visited == res.distinct_cells)
        << "V2 traversal visited " << v2_visited << " cells, db has " << res.distinct_cells;
  }

  // meta entries
  ton::BlockIdExt block_id{0, 0x8000000000000000ULL, 0, ton::RootHash{res.root_hash}, ton::FileHash{res.file_hash}};
  auto z = ton::get_tl_object_sha_bits256(ton::create_tl_block_id(block_id));
  std::string value;
  CHECK(reader->get("desczero", value).move_as_ok() == td::KeyValue::GetStatus::Ok);
  auto desczero = ton::fetch_tl_object<ton::ton_api::db_celldb_value>(td::BufferSlice{value}, true).move_as_ok();
  CHECK(desczero->block_id_->workchain_ == ton::workchainInvalid);
  CHECK(desczero->prev_ == z && desczero->next_ == z);
  CHECK(desczero->root_hash_.is_zero());
  CHECK(reader->get(PSTRING() << "desc" << z, value).move_as_ok() == td::KeyValue::GetStatus::Ok);
  auto desc = ton::fetch_tl_object<ton::ton_api::db_celldb_value>(td::BufferSlice{value}, true).move_as_ok();
  CHECK(desc->block_id_->workchain_ == 0);
  CHECK(static_cast<td::uint64>(desc->block_id_->shard_) == 0x8000000000000000ULL);
  CHECK(desc->block_id_->seqno_ == 0);
  CHECK(desc->block_id_->root_hash_ == res.root_hash);
  CHECK(desc->block_id_->file_hash_ == res.file_hash);
  CHECK(desc->prev_.is_zero() && desc->next_.is_zero());
  CHECK(desc->root_hash_ == res.root_hash);

  // TLB validity of the on-disk state, loading through the DB
  {
    auto boc = vm::DynamicBagOfCellsDb::create();
    boc->set_loader(std::make_unique<vm::CellLoader>(reader));
    auto root = boc->load_root(res.root_hash.as_slice()).move_as_ok();
    LOG_CHECK(block::gen::t_ShardStateUnsplit.validate_ref(10000000, root)) << "on-disk state failed TLB validation";
  }

  // manifest round-trip
  auto manifest_str = td::read_file_str(cfg.out_dir + "/manifest.json").move_as_ok();
  auto manifest = Manifest::from_json(manifest_str).move_as_ok();
  CHECK(manifest.root_hash == res.root_hash);
  CHECK(manifest.file_hash == res.file_hash);
  CHECK(manifest.total_balance == res.total_balance);
  CHECK(manifest.num_v5 == cfg.num_v5 && manifest.num_ballast == cfg.num_ballast);
  CHECK(manifest.seed == cfg.seed);

  LOG(INFO) << "self-test (c) celldb round-trip (bundle_depth=" << bundle_depth << "): OK (" << visited << " cells, "
            << bundle_records << " bundles, root " << res.root_hash.to_hex() << ")";
  return res.root_hash;
}

void self_test_external(const GenContext &ctx) {
  const auto &cfg = ctx.cfg;
  Manifest manifest;
  manifest.seed = cfg.seed;
  manifest.wallet_id = cfg.wallet_id;
  manifest.minter_addr = ctx.minter_addr;
  manifest.jw_jetton_balance = cfg.jw_jetton_balance;
  auto msg = build_signed_external(cfg.seed, 0, 1, manifest, ctx.contracts).move_as_ok();
  LOG_CHECK(block::gen::t_Message_Any.validate_ref(1000000, msg)) << "external message failed TLB validation";

  auto wallet0 = derive_wallet(cfg.seed, 0, cfg.wallet_id, ctx.minter_addr, ctx.contracts).move_as_ok();
  auto cs = vm::load_cell_slice(msg);
  CHECK(cs.fetch_ulong(2) == 0b10);  // ext_in_msg_info$10
  CHECK(cs.fetch_ulong(2) == 0);     // src:addr_none
  CHECK(cs.fetch_ulong(3) == 0b100);
  CHECK(cs.fetch_ulong(8) == 0);
  td::Bits256 dest;
  CHECK(cs.fetch_bits_to(dest.bits(), 256));
  CHECK(dest == wallet0.w5_addr);
  CHECK(cs.fetch_ulong(4) == 0);  // import_fee
  CHECK(cs.fetch_ulong(1) == 0);  // init:nothing
  CHECK(cs.fetch_ulong(1) == 0);  // body inline
  // body: 130 bits + ref(c5) + 512-bit signature
  CHECK(cs.size() == 130 + 512 && cs.size_refs() == 1);
  vm::CellBuilder unsigned_cb;
  CHECK(cs.fetch_ulong(32) == 0x7369676E);
  unsigned_cb.store_long(0x7369676E, 32);
  unsigned_cb.store_long(static_cast<long long>(cs.fetch_ulong(32)), 32);  // wallet_id
  auto valid_until = cs.fetch_ulong(32);
  CHECK(valid_until == 0xFFFFFFFE);
  unsigned_cb.store_long(static_cast<long long>(valid_until), 32);
  unsigned_cb.store_long(static_cast<long long>(cs.fetch_ulong(32)), 32);  // seqno
  CHECK(cs.fetch_ulong(1) == 1);
  unsigned_cb.store_long(1, 1);
  unsigned_cb.store_ref(cs.fetch_ref());
  CHECK(cs.fetch_ulong(1) == 0);
  unsigned_cb.store_long(0, 1);
  auto unsigned_cell = unsigned_cb.finalize_novm();
  unsigned char sig[64];
  CHECK(cs.fetch_bits_to(td::BitPtr{sig}, 512));
  CHECK(cs.empty_ext());

  auto pubkey = td::Ed25519::PublicKey::from_slice(wallet0.pubkey.as_slice()).move_as_ok();
  pubkey.verify_signature(unsigned_cell->get_hash().as_slice(), td::Slice(sig, 64)).ensure();
  LOG(INFO) << "self-test (d) external message: OK";
}

void self_test_empty_root(const Config &base_cfg) {
  Config cfg = base_cfg;
  cfg.num_v5 = 0;
  cfg.num_ballast = 0;
  cfg.gen_utime = 1700000000;
  cfg.tmp_dir = PSTRING() << "/tmp/bench-state-gen-selftest-e." << getpid();
  auto res = run_pipeline(cfg, false).move_as_ok();
  CHECK(res.total_balance == 0);
  td::Bits256 expected;
  CHECK(expected.from_hex(td::Slice(kEmptyStateRootHashHex)) == 256);
  LOG_CHECK(res.root_hash == expected) << "empty-state root hash " << res.root_hash.to_hex() << " != python-derived "
                                       << expected.to_hex();
  LOG(INFO) << "self-test (e) python cross-check (empty state root): OK";
}

td::Status do_self_test(const Config &base_cfg) {
  Config cfg = base_cfg;
  cfg.gen_utime = cfg.gen_utime ? cfg.gen_utime : 1700000000;
  TRY_RESULT(ctx, make_gen_context(cfg));
  self_test_streaming(ctx);
  self_test_external(ctx);
  auto root_plain = self_test_celldb(cfg, 0);
  auto root_bundled = self_test_celldb(cfg, 5);
  // bundling is a storage-layer change only: the state root must not move
  CHECK(root_plain == root_bundled);
  self_test_empty_root(cfg);
  LOG(INFO) << "self-test: ALL OK";
  return td::Status::OK();
}

}  // namespace
}  // namespace bench

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  bench::Config cfg;
  std::string src, dst;

  td::OptionParser p;
  p.set_description("bench-state-gen <gen|root-only|self-test|checkpoint> [options] (see benchmark/DESIGN.md)");
  p.add_checked_option('\0', "seed-hex", "32-byte hex seed for deterministic derivation", [&](td::Slice arg) {
    if (cfg.seed.from_hex(arg) != 256) {
      return td::Status::Error("--seed-hex must be 64 hex digits");
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "v5-count", "number of wallet-v5 accounts (each with a paired jetton wallet)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(cfg.num_v5, td::to_integer_safe<td::uint64>(arg));
                         return td::Status::OK();
                       });
  p.add_checked_option('\0', "ballast-count", "number of ballast accounts", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cfg.num_ballast, td::to_integer_safe<td::uint64>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "ballast-cells", "data cells per ballast account", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cfg.ballast_cells, td::to_integer_safe<int>(arg));
    if (cfg.ballast_cells < 1) {
      return td::Status::Error("--ballast-cells must be >= 1");
    }
    return td::Status::OK();
  });
  p.add_checked_option('\0', "gen-utime", "state generation unixtime (default: now)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cfg.gen_utime, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "wallet-id", "wallet-v5 wallet_id", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cfg.wallet_id, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('\0', "bundle-depth",
                       "bundle this many key-bit levels of the accounts dict per celldb record (default 5, 0 = off)",
                       [&](td::Slice arg) {
                         TRY_RESULT_ASSIGN(cfg.bundle_depth, td::to_integer_safe<int>(arg));
                         if (cfg.bundle_depth < 0 || cfg.bundle_depth > 16) {
                           return td::Status::Error("--bundle-depth must be in [0, 16]");
                         }
                         return td::Status::OK();
                       });
  p.add_option('\0', "contracts-dir", "directory with contract .boc files (default: benchmark/contracts)",
               [&](td::Slice arg) { cfg.contracts_dir = arg.str(); });
  p.add_option('\0', "out-dir", "output directory (celldb + manifest.json)",
               [&](td::Slice arg) { cfg.out_dir = arg.str(); });
  p.add_option('\0', "tmp-dir", "scratch directory for bucket/run files (default: <out-dir>/tmp)",
               [&](td::Slice arg) { cfg.tmp_dir = arg.str(); });
  p.add_checked_option('\0', "threads", "worker threads", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(cfg.threads, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.add_option('\0', "overwrite", "overwrite an existing celldb", [&] { cfg.overwrite = true; });
  p.add_option('\0', "src", "checkpoint source celldb", [&](td::Slice arg) { src = arg.str(); });
  p.add_option('\0', "dst", "checkpoint destination", [&](td::Slice arg) { dst = arg.str(); });
  p.add_option('v', "verbosity", "verbosity level",
               [&](td::Slice arg) { SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + td::to_integer<int>(arg)); });

  auto r_rest = p.run(argc, argv);
  if (r_rest.is_error()) {
    LOG(ERROR) << r_rest.error();
    LOG(ERROR) << p;
    return 2;
  }
  auto rest = r_rest.move_as_ok();
  if (rest.size() != 1) {
    LOG(ERROR) << p;
    return 2;
  }
  std::string command = rest[0];

  td::Status status;
  if (command == "gen" || command == "root-only") {
    auto res = bench::run_pipeline(cfg, command == "gen");
    if (res.is_ok()) {
      printf("root_hash=%s\nfile_hash=%s\ntotal_balance=%s\ncells=%" PRIu64 "\n", res.ok().root_hash.to_hex().c_str(),
             res.ok().file_hash.to_hex().c_str(), bench::u128_to_dec(res.ok().total_balance).c_str(),
             res.ok().distinct_cells ? res.ok().distinct_cells : res.ok().emitted_cells);
    } else {
      status = res.move_as_error();
    }
  } else if (command == "self-test") {
    status = bench::do_self_test(cfg);
  } else if (command == "checkpoint") {
    if (src.empty() || dst.empty()) {
      LOG(ERROR) << "checkpoint requires --src and --dst";
      return 2;
    }
    status = bench::do_checkpoint(src, dst);
  } else {
    LOG(ERROR) << "unknown command " << command;
    LOG(ERROR) << p;
    return 2;
  }
  if (status.is_error()) {
    LOG(ERROR) << command << " failed: " << status;
    return 1;
  }
  return 0;
}
