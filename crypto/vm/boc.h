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
#include "td/utils/CancellationToken.h"

#include <set>
#include <map>
#include "vm/db/DynamicBagOfCellsDb.h"
#include "vm/cells.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/HashMap.h"
#include "td/utils/HashSet.h"
#include "td/utils/Time.h"
#include "td/utils/Timer.h"
#include "td/utils/port/FileFd.h"

namespace vm {
using td::Ref;

class NewCellStorageStat {
 public:
  NewCellStorageStat() {
  }

  struct Stat {
    Stat() {
    }
    Stat(td::uint64 cells_, td::uint64 bits_, td::uint64 internal_refs_ = 0, td::uint64 external_refs_ = 0)
        : cells(cells_), bits(bits_), internal_refs(internal_refs_), external_refs(external_refs_) {
    }
    td::uint64 cells{0};
    td::uint64 bits{0};
    td::uint64 internal_refs{0};
    td::uint64 external_refs{0};

    auto key() const {
      return std::make_tuple(cells, bits, internal_refs, external_refs);
    }
    bool operator==(const Stat& other) const {
      return key() == other.key();
    }
    Stat(const Stat& other) = default;
    Stat& operator=(const Stat& other) = default;
    Stat& operator+=(const Stat& other) {
      cells += other.cells;
      bits += other.bits;
      internal_refs += other.internal_refs;
      external_refs += other.external_refs;
      return *this;
    }
    Stat operator+(const Stat& other) const {
      return Stat{cells + other.cells, bits + other.bits, internal_refs + other.internal_refs,
                  external_refs + other.external_refs};
    }
    bool fits_uint32() const {
      return !((cells | bits | internal_refs | external_refs) >> 32);
    }
    void set_zero() {
      cells = bits = internal_refs = external_refs = 0;
    }
  };

  Stat get_stat() const {
    return stat_;
  }

  Stat get_proof_stat() const {
    return proof_stat_;
  }

  Stat get_total_stat() const {
    return stat_ + proof_stat_;
  }

  void add_cell(Ref<Cell> cell);
  void add_proof(Ref<Cell> cell, const CellUsageTree* usage_tree);
  void add_cell_and_proof(Ref<Cell> cell, const CellUsageTree* usage_tree);
  Stat tentative_add_cell(Ref<Cell> cell) const;
  Stat tentative_add_proof(Ref<Cell> cell, const CellUsageTree* usage_tree) const;
  void set_zero() {
    stat_.set_zero();
    proof_stat_.set_zero();
  }

 private:
  const CellUsageTree* usage_tree_;
  td::HashSet<vm::Cell::Hash> seen_;
  Stat stat_;
  td::HashSet<vm::Cell::Hash> proof_seen_;
  Stat proof_stat_;
  const NewCellStorageStat* parent_{nullptr};

  void dfs(Ref<Cell> cell, bool need_stat, bool need_proof_stat);
};

struct CellStorageStat {
  unsigned long long cells;
  unsigned long long bits;
  unsigned long long public_cells;
  struct CellInfo {
    td::uint32 max_merkle_depth = 0;
  };
  td::HashMap<vm::Cell::Hash, CellInfo> seen;
  CellStorageStat() : cells(0), bits(0), public_cells(0) {
  }
  explicit CellStorageStat(unsigned long long limit_cells)
      : cells(0), bits(0), public_cells(0), limit_cells(limit_cells) {
  }
  void clear_seen() {
    seen.clear();
  }
  void clear() {
    cells = bits = public_cells = 0;
    clear_limit();
    clear_seen();
  }
  void clear_limit() {
    limit_cells = std::numeric_limits<unsigned long long>::max();
    limit_bits = std::numeric_limits<unsigned long long>::max();
  }
  td::Result<CellInfo> compute_used_storage(Ref<vm::CellSlice> cs_ref, bool kill_dup = true,
                                            unsigned skip_count_root = 0);
  td::Result<CellInfo> compute_used_storage(const CellSlice& cs, bool kill_dup = true, unsigned skip_count_root = 0);
  td::Result<CellInfo> compute_used_storage(CellSlice&& cs, bool kill_dup = true, unsigned skip_count_root = 0);
  td::Result<CellInfo> compute_used_storage(Ref<vm::Cell> cell, bool kill_dup = true, unsigned skip_count_root = 0);

  td::Result<CellInfo> add_used_storage(Ref<vm::CellSlice> cs_ref, bool kill_dup = true, unsigned skip_count_root = 0);
  td::Result<CellInfo> add_used_storage(const CellSlice& cs, bool kill_dup = true, unsigned skip_count_root = 0);
  td::Result<CellInfo> add_used_storage(CellSlice&& cs, bool kill_dup = true, unsigned skip_count_root = 0);
  td::Result<CellInfo> add_used_storage(Ref<vm::Cell> cell, bool kill_dup = true, unsigned skip_count_root = 0);

  unsigned long long limit_cells = std::numeric_limits<unsigned long long>::max();
  unsigned long long limit_bits = std::numeric_limits<unsigned long long>::max();
};

struct VmStorageStat {
  td::uint64 cells{0}, bits{0}, refs{0}, limit;
  td::HashSet<CellHash> visited;
  VmStorageStat(td::uint64 _limit) : limit(_limit) {
  }
  bool add_storage(Ref<Cell> cell);
  bool add_storage(const CellSlice& cs);
  bool check_visited(const CellHash& cell_hash) {
    return visited.insert(cell_hash).second;
  }
  bool check_visited(const Ref<Cell>& cell) {
    return check_visited(cell->get_hash());
  }
};

class ProofStorageStat {
 public:
  void add_cell(const Ref<DataCell>& cell);
  td::uint64 estimate_proof_size() const;
 private:
  enum CellStatus {
    c_none = 0, c_prunned = 1, c_loaded = 2
  };
  td::HashMap<vm::Cell::Hash, CellStatus> cells_;
  td::uint64 proof_size_ = 0;
};

struct CellSerializationInfo {
  bool special;
  Cell::LevelMask level_mask;

  bool with_hashes;
  size_t hashes_offset;
  size_t depth_offset;

  size_t data_offset;
  size_t data_len;
  bool data_with_bits;

  size_t refs_offset;
  int refs_cnt;

  size_t end_offset;

  td::Status init(td::Slice data, int ref_byte_size);
  td::Status init(td::uint8 d1, td::uint8 d2, int ref_byte_size);
  td::Result<int> get_bits(td::Slice cell) const;

  td::Result<Ref<DataCell>> create_data_cell(td::Slice data, td::Span<Ref<Cell>> refs) const;
};

class BagOfCellsLogger {
 public:
  BagOfCellsLogger() = default;
  explicit BagOfCellsLogger(td::CancellationToken cancellation_token)
      : cancellation_token_(std::move(cancellation_token)) {
  }

  void start_stage(std::string stage) {
    log_speed_at_ = td::Timestamp::in(LOG_SPEED_PERIOD);
    processed_cells_ = 0;
    timer_ = {};
    stage_ = std::move(stage);
  }
  void finish_stage(td::Slice desc) {
    LOG(ERROR) << "serializer: " << stage_ << " took " << timer_.elapsed() << "s, " << desc;
  }
  td::Status on_cell_processed() {
    ++processed_cells_;
    if (processed_cells_ % 1000 == 0) {
      TRY_STATUS(cancellation_token_.check());
    }
    if (log_speed_at_.is_in_past()) {
      log_speed_at_ += LOG_SPEED_PERIOD;
      LOG(WARNING) << "serializer: " << stage_ << " " << (double)processed_cells_ / LOG_SPEED_PERIOD << " cells/s";
      processed_cells_ = 0;
    }
    return td::Status::OK();
  }

 private:
  std::string stage_;
  td::Timer timer_;
  td::CancellationToken cancellation_token_;
  td::Timestamp log_speed_at_;
  size_t processed_cells_ = 0;
  static constexpr double LOG_SPEED_PERIOD = 120.0;
};
class BagOfCells {
 public:
  enum { hash_bytes = vm::Cell::hash_bytes, default_max_roots = 16384 };
  enum Mode { WithIndex = 1, WithCRC32C = 2, WithTopHash = 4, WithIntHashes = 8, WithCacheBits = 16, max = 31 };
  enum { max_cell_whs = 64 };
  using Hash = Cell::Hash;
  struct Info {
    enum : td::uint32 { boc_idx = 0x68ff65f3, boc_idx_crc32c = 0xacc3a728, boc_generic = 0xb5ee9c72 };

    unsigned magic;
    int root_count;
    int cell_count;
    int absent_count;
    int ref_byte_size;
    int offset_byte_size;
    bool valid;
    bool has_index;
    bool has_roots{false};
    bool has_crc32c;
    bool has_cache_bits;
    unsigned long long roots_offset, index_offset, data_offset, data_size, total_size;
    Info() : magic(0), valid(false) {
    }
    void invalidate() {
      valid = false;
    }
    long long parse_serialized_header(const td::Slice& slice);
    unsigned long long read_int(const unsigned char* ptr, unsigned bytes);
    unsigned long long read_ref(const unsigned char* ptr) {
      return read_int(ptr, ref_byte_size);
    }
    unsigned long long read_offset(const unsigned char* ptr) {
      return read_int(ptr, offset_byte_size);
    }
    void write_int(unsigned char* ptr, unsigned long long value, int bytes);
    void write_ref(unsigned char* ptr, unsigned long long value) {
      write_int(ptr, value, ref_byte_size);
    }
    void write_offset(unsigned char* ptr, unsigned long long value) {
      write_int(ptr, value, offset_byte_size);
    }
  };

 private:
  int cell_count{0}, root_count{0}, dangle_count{0}, int_refs{0};
  int int_hashes{0}, top_hashes{0};
  int max_depth{1024};
  Info info;
  unsigned long long data_bytes{0};
  td::HashMap<Hash, int> cells;
  struct CellInfo {
    Ref<DataCell> dc_ref;
    std::array<int, 4> ref_idx;
    unsigned char ref_num;
    unsigned char wt;
    unsigned char hcnt;
    int new_idx;
    bool should_cache{false};
    bool is_root_cell{false};
    CellInfo() : ref_num(0) {
    }
    CellInfo(Ref<DataCell> _dc) : dc_ref(std::move(_dc)), ref_num(0) {
    }
    CellInfo(Ref<DataCell> _dc, int _refs, const std::array<int, 4>& _ref_list)
        : dc_ref(std::move(_dc)), ref_idx(_ref_list), ref_num(static_cast<unsigned char>(_refs)) {
    }
    bool is_special() const {
      return !wt;
    }
  };
  std::vector<CellInfo> cell_list_;
  struct RootInfo {
    RootInfo() = default;
    RootInfo(Ref<Cell> cell, int idx) : cell(std::move(cell)), idx(idx) {
    }
    Ref<Cell> cell;
    int idx{-1};
  };
  std::vector<CellInfo> cell_list_tmp;
  std::vector<RootInfo> roots;
  std::vector<unsigned char> serialized;
  const unsigned char* index_ptr{nullptr};
  const unsigned char* data_ptr{nullptr};
  std::vector<unsigned long long> custom_index;
  BagOfCellsLogger* logger_ptr_{nullptr};

 public:
  void clear();
  int set_roots(const std::vector<td::Ref<vm::Cell>>& new_roots);
  int set_root(td::Ref<vm::Cell> new_root);
  int add_roots(const std::vector<td::Ref<vm::Cell>>& add_roots);
  int add_root(td::Ref<vm::Cell> add_root);
  td::Status import_cells() TD_WARN_UNUSED_RESULT;
  BagOfCells() = default;
  void set_logger(BagOfCellsLogger* logger_ptr) {
    logger_ptr_ = logger_ptr;
  }
  std::size_t estimate_serialized_size(int mode = 0);
  td::Status serialize(int mode = 0);
  td::string serialize_to_string(int mode = 0);
  td::Result<td::BufferSlice> serialize_to_slice(int mode = 0);
  td::Result<std::size_t> serialize_to(unsigned char* buffer, std::size_t buff_size, int mode = 0);
  td::Status serialize_to_file(td::FileFd& fd, int mode = 0);
  template <typename WriterT>
  td::Result<std::size_t> serialize_to_impl(WriterT& writer, int mode = 0);
  std::string extract_string() const;

  td::Result<long long> deserialize(const td::Slice& data, int max_roots = default_max_roots);
  td::Result<long long> deserialize(const unsigned char* buffer, std::size_t buff_size,
                                    int max_roots = default_max_roots) {
    return deserialize(td::Slice{buffer, buff_size}, max_roots);
  }
  int get_root_count() const {
    return root_count;
  }
  Ref<Cell> get_root_cell(int idx = 0) const {
    return (idx >= 0 && idx < root_count) ? roots.at(idx).cell : Ref<Cell>{};
  }

  static int precompute_cell_serialization_size(const unsigned char* cell, std::size_t len, int ref_size,
                                                int* refs_num_ptr = nullptr);

 private:
  int rv_idx;
  td::Result<int> import_cell(td::Ref<vm::Cell> cell, int depth);
  void cells_clear() {
    cell_count = 0;
    int_refs = 0;
    data_bytes = 0;
    cells.clear();
    cell_list_.clear();
  }
  td::uint64 compute_sizes(int mode, int& r_size, int& o_size);
  void reorder_cells();
  int revisit(int cell_idx, int force = 0);
  unsigned long long get_idx_entry_raw(int index);
  unsigned long long get_idx_entry(int index);
  bool get_cache_entry(int index);
  td::Result<td::Slice> get_cell_slice(int index, td::Slice data);
  td::Result<td::Ref<vm::DataCell>> deserialize_cell(int index, td::Slice data, td::Span<td::Ref<DataCell>> cells,
                                                     std::vector<td::uint8>* cell_should_cache);
};

td::Result<Ref<Cell>> std_boc_deserialize(td::Slice data, bool can_be_empty = false, bool allow_nonzero_level = false);
td::Result<td::BufferSlice> std_boc_serialize(Ref<Cell> root, int mode = 0);

td::Result<std::vector<Ref<Cell>>> std_boc_deserialize_multi(td::Slice data,
                                                             int max_roots = BagOfCells::default_max_roots);
td::Result<td::BufferSlice> std_boc_serialize_multi(std::vector<Ref<Cell>> root, int mode = 0);

td::Status std_boc_serialize_to_file(Ref<Cell> root, td::FileFd& fd, int mode = 0,
                                     td::CancellationToken cancellation_token = {});
td::Status std_boc_serialize_to_file_large(std::shared_ptr<CellDbReader> reader, Cell::Hash root_hash, td::FileFd& fd,
                                           int mode = 0, td::CancellationToken cancellation_token = {});

}  // namespace vm
