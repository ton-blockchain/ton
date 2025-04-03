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
*/
#include "td/utils/Time.h"
#include "td/utils/Timer.h"

#include <map>
#include "vm/boc.h"
#include "vm/boc-writers.h"
#include "vm/cellslice.h"
#include "td/utils/misc.h"

namespace vm {

namespace {
// LargeBocSerializer implements serialization of the bag of cells in the standard way
// (equivalent to the implementation in crypto/vm/boc.cpp)
// Changes in this file may require corresponding changes in boc.cpp
class LargeBocSerializer {
 public:
  using Hash = Cell::Hash;
  constexpr static int load_batch_size = 4'000'000;

  explicit LargeBocSerializer(std::shared_ptr<CellDbReader> reader) : reader(std::move(reader)) {
  }

  void set_logger(BagOfCellsLogger* logger_ptr) {
    logger_ptr_ = logger_ptr;
  }
  void add_root(Hash root);
  td::Status import_cells();
  td::Status serialize(td::FileFd& fd, int mode);

 private:
  std::shared_ptr<CellDbReader> reader;
  struct CellInfo {
    std::array<int, 4> ref_idx;
    int idx;
    unsigned short serialized_size;
    unsigned char wt;
    unsigned char hcnt : 6;
    bool should_cache : 1;
    bool is_root_cell : 1;
    CellInfo(int idx, const std::array<int, 4>& ref_list) : ref_idx(ref_list), idx(idx) {
      hcnt = 0;
      should_cache = is_root_cell = 0;
    }
    bool is_special() const {
      return !wt;
    }
    unsigned get_ref_num() const {
      for (unsigned i = 0; i < 4; ++i) {
        if (ref_idx[i] == -1) {
          return i;
        }
      }
      return 4;
    }
  };
  td::NodeHashMap<Hash, CellInfo> cells;
  std::vector<std::pair<const Hash, CellInfo>*> cell_list;
  struct RootInfo {
    RootInfo(Hash hash, int idx) : hash(hash), idx(idx) {
    }
    Hash hash;
    int idx;
  };
  std::vector<RootInfo> roots;
  int cell_count = 0, int_refs = 0, int_hashes = 0, top_hashes = 0;
  int rv_idx = 0;
  unsigned long long data_bytes = 0;

  td::Result<int> import_cell(Hash hash, int depth = 0);
  void reorder_cells();
  int revisit(int cell_idx, int force = 0);
  td::uint64 compute_sizes(int mode, int& r_size, int& o_size);

  BagOfCellsLogger* logger_ptr_{};
};

void LargeBocSerializer::add_root(Hash root) {
  roots.emplace_back(root, -1);
}

td::Status LargeBocSerializer::import_cells() {
  if (logger_ptr_) {
    logger_ptr_->start_stage("import_cells");
  }
  for (auto& root : roots) {
    TRY_RESULT(idx, import_cell(root.hash));
    root.idx = idx;
  }
  reorder_cells();
  CHECK(!cell_list.empty());
  if (logger_ptr_) {
    logger_ptr_->finish_stage(PSLICE() << cell_count << " cells");
  }
  return td::Status::OK();
}

td::Result<int> LargeBocSerializer::import_cell(Hash root_hash, int root_depth) {
  const int start_ind = cell_count;
  td::HashMap<Hash, std::pair<int, bool>> current_depth_hashes;

  auto existing_it = cells.find(root_hash);
  if (existing_it != cells.end()) {
    existing_it->second.should_cache = true;
  } else {
    current_depth_hashes.emplace(root_hash, std::make_pair(cell_count, false));
  }
  int current_depth = root_depth;
  int next_child_idx = cell_count + 1;
  while (!current_depth_hashes.empty()) {
    if (current_depth > Cell::max_depth) {
      return td::Status::Error("error while importing a cell into a bag of cells: cell depth too large");
    }
    
    cell_list.resize(cell_list.size() + current_depth_hashes.size());
    td::HashMap<Hash, std::pair<int, bool>> next_depth_hashes;
    auto batch_start = current_depth_hashes.begin();
    while (batch_start != current_depth_hashes.end()) {
      std::vector<td::Slice> batch_hashes;
      batch_hashes.reserve(load_batch_size);
      std::vector<std::pair<int, bool>*> batch_idxs_should_cache;
      batch_idxs_should_cache.reserve(load_batch_size);

      while (batch_hashes.size() < load_batch_size && batch_start != current_depth_hashes.end()) {
        batch_hashes.push_back(batch_start->first.as_slice());
        batch_idxs_should_cache.push_back(&batch_start->second);
        ++batch_start;
      }

      TRY_RESULT_PREFIX(loaded_results, reader->load_bulk(batch_hashes), 
                "error while importing a cell into a bag of cells: ");
      DCHECK(loaded_results.size() == batch_hashes.size());

      for (size_t i = 0; i < loaded_results.size(); ++i) {
        auto& cell = loaded_results[i];

        if (cell->get_virtualization() != 0) {
          return td::Status::Error(
            "error while importing a cell into a bag of cells: cell has non-zero virtualization level");
        }

        const auto hash = cell->get_hash();
        CellSlice cs(std::move(cell));

        DCHECK(cs.size_refs() <= 4);
        std::array<int, 4> refs{-1, -1, -1, -1};
        for (unsigned j = 0; j < cs.size_refs(); j++) {
          auto child = cs.prefetch_ref(j);
          const auto child_hash = child->get_hash();

          auto existing_global_it = cells.find(child_hash);
          if (existing_global_it != cells.end()) {
            existing_global_it->second.should_cache = true;
            refs[j] = existing_global_it->second.idx;
            continue;
          }
          auto current_depth_it = current_depth_hashes.find(child_hash);
          if (current_depth_it != current_depth_hashes.end()) {
            current_depth_it->second.second = true;
            refs[j] = current_depth_it->second.first;
            continue;
          }
          auto next_depth_it = next_depth_hashes.find(child_hash);
          if (next_depth_it != next_depth_hashes.end()) {
            next_depth_it->second.second = true;
            refs[j] = next_depth_it->second.first;
            continue;
          }
          auto res = next_depth_hashes.emplace(child_hash, std::make_pair(next_child_idx, false));
          refs[j] = next_child_idx++;
        }

        auto dc = cs.move_as_loaded_cell().data_cell;
        auto idx_should_cache = batch_idxs_should_cache[i];
        auto res = cells.emplace(hash, CellInfo(idx_should_cache->first, std::move(refs)));
        DCHECK(res.second);
        cell_list[idx_should_cache->first] = &*res.first;
        CellInfo& dc_info = res.first->second;
        dc_info.should_cache = idx_should_cache->second;
        dc_info.hcnt = static_cast<unsigned char>(dc->get_level_mask().get_hashes_count());
        DCHECK(dc_info.hcnt <= 4);
        dc_info.wt = 0; // will be calculated after traversing
        TRY_RESULT(serialized_size, td::narrow_cast_safe<unsigned short>(dc->get_serialized_size()));
        data_bytes += dc_info.serialized_size = serialized_size;
        cell_count++;
      }
      if (logger_ptr_) {
        TRY_STATUS(logger_ptr_->on_cells_processed(batch_hashes.size()));
      }
    }

    current_depth_hashes = std::move(next_depth_hashes);
    next_depth_hashes.clear();
    current_depth++;
  }
  DCHECK(next_child_idx == cell_count);
  
  for (int idx = cell_count - 1; idx >= start_ind; --idx) {
    CellInfo& cell_info = cell_list[idx]->second;

    unsigned sum_child_wt = 1;
    for (size_t j = 0; j < cell_info.ref_idx.size(); ++j) {
      int child_idx = cell_info.ref_idx[j];
      if (child_idx == -1) {
        continue;
      }
      sum_child_wt += cell_list[child_idx]->second.wt;
      ++int_refs;
    }
    cell_info.wt = static_cast<unsigned char>(std::min(0xffU, sum_child_wt));
  }

  auto root_it = cells.find(root_hash);
  DCHECK(root_it != cells.end());
  return root_it->second.idx;
}

void LargeBocSerializer::reorder_cells() {
  for (auto ptr : cell_list) {
    ptr->second.idx = -1;
  }
  int_hashes = 0;
  for (int i = cell_count - 1; i >= 0; --i) {
    CellInfo& dci = cell_list[i]->second;
    int s = dci.get_ref_num(), c = s, sum = BagOfCells::max_cell_whs - 1, mask = 0;
    for (int j = 0; j < s; ++j) {
      CellInfo& dcj = cell_list[dci.ref_idx[j]]->second;
      int limit = (BagOfCells::max_cell_whs - 1 + j) / s;
      if (dcj.wt <= limit) {
        sum -= dcj.wt;
        --c;
        mask |= (1 << j);
      }
    }
    if (c) {
      for (int j = 0; j < s; ++j) {
        if (!(mask & (1 << j))) {
          CellInfo& dcj = cell_list[dci.ref_idx[j]]->second;
          int limit = sum++ / c;
          if (dcj.wt > limit) {
            dcj.wt = (unsigned char)limit;
          }
        }
      }
    }
  }
  for (int i = 0; i < cell_count; i++) {
    CellInfo& dci = cell_list[i]->second;
    int s = dci.get_ref_num(), sum = 1;
    for (int j = 0; j < s; ++j) {
      sum += cell_list[dci.ref_idx[j]]->second.wt;
    }
    DCHECK(sum <= BagOfCells::max_cell_whs);
    if (sum <= dci.wt) {
      dci.wt = (unsigned char)sum;
    } else {
      dci.wt = 0;
      int_hashes += dci.hcnt;
    }
  }
  top_hashes = 0;
  for (auto& root_info : roots) {
    auto& cell_info = cell_list[root_info.idx]->second;
    if (cell_info.is_root_cell) {
      cell_info.is_root_cell = true;
      if (cell_info.wt) {
        top_hashes += cell_info.hcnt;
      }
    }
  }
  if (cell_count > 0) {
    rv_idx = 0;

    for (const auto& root_info : roots) {
      revisit(root_info.idx, 0);
      revisit(root_info.idx, 1);
    }
    for (const auto& root_info : roots) {
      revisit(root_info.idx, 2);
    }
    for (auto& root_info : roots) {
      root_info.idx = cell_list[root_info.idx]->second.idx;
    }

    DCHECK(rv_idx == cell_count);
    for (int i = 0; i < cell_count; ++i) {
      while (cell_list[i]->second.idx != i) {
        std::swap(cell_list[i], cell_list[cell_list[i]->second.idx]);
      }
    }
  }
}

int LargeBocSerializer::revisit(int cell_idx, int force) {
  DCHECK(cell_idx >= 0 && cell_idx < cell_count);
  CellInfo& dci = cell_list[cell_idx]->second;
  if (dci.idx >= 0) {
    return dci.idx;
  }
  if (!force) {
    // previsit
    if (dci.idx != -1) {
      // already previsited or visited
      return dci.idx;
    }
    int n = dci.get_ref_num();
    for (int j = n - 1; j >= 0; --j) {
      int child_idx = dci.ref_idx[j];
      // either previsit or visit child, depending on whether it is special
      revisit(dci.ref_idx[j], cell_list[child_idx]->second.is_special());
    }
    return dci.idx = -2;  // mark as previsited
  }
  if (force > 1) {
    // time to allocate
    auto i = dci.idx = rv_idx++;
    return i;
  }
  if (dci.idx == -3) {
    // already visited
    return dci.idx;
  }
  if (dci.is_special()) {
    // if current cell is special, previsit it first
    revisit(cell_idx, 0);
  }
  // visit children
  int n = dci.get_ref_num();
  for (int j = n - 1; j >= 0; --j) {
    revisit(dci.ref_idx[j], 1);
  }
  // allocate children
  for (int j = n - 1; j >= 0; --j) {
    dci.ref_idx[j] = revisit(dci.ref_idx[j], 2);
  }
  return dci.idx = -3;  // mark as visited (and all children processed)
}

td::uint64 LargeBocSerializer::compute_sizes(int mode, int& r_size, int& o_size) {
  using Mode = BagOfCells::Mode;
  int rs = 0, os = 0;
  if (roots.empty() || !data_bytes) {
    r_size = o_size = 0;
    return 0;
  }
  while (cell_count >= (1LL << (rs << 3))) {
    rs++;
  }
  td::uint64 hashes =
      (((mode & Mode::WithTopHash) ? top_hashes : 0) + ((mode & Mode::WithIntHashes) ? int_hashes : 0)) *
      (Cell::hash_bytes + Cell::depth_bytes);
  td::uint64 data_bytes_adj = data_bytes + (unsigned long long)int_refs * rs + hashes;
  td::uint64 max_offset = (mode & Mode::WithCacheBits) ? data_bytes_adj * 2 : data_bytes_adj;
  while (max_offset >= (1ULL << (os << 3))) {
    os++;
  }
  if (rs > 4 || os > 8) {
    r_size = o_size = 0;
    return 0;
  }
  r_size = rs;
  o_size = os;
  return data_bytes_adj;
}

td::Status LargeBocSerializer::serialize(td::FileFd& fd, int mode) {
  using Mode = BagOfCells::Mode;
  BagOfCells::Info info;
  if ((mode & Mode::WithCacheBits) && !(mode & Mode::WithIndex)) {
    return td::Status::Error("invalid flags");
  }
  auto data_bytes_adj = compute_sizes(mode, info.ref_byte_size, info.offset_byte_size);
  if (data_bytes_adj == 0) {
    return td::Status::Error("no cells to serialize");
  }
  info.valid = true;
  info.has_crc32c = mode & Mode::WithCRC32C;
  info.has_index = mode & Mode::WithIndex;
  info.has_cache_bits = mode & Mode::WithCacheBits;
  info.root_count = (int)roots.size();
  info.cell_count = cell_count;
  info.absent_count = 0;
  int crc_size = info.has_crc32c ? 4 : 0;
  info.roots_offset = 4 + 1 + 1 + 3 * info.ref_byte_size + info.offset_byte_size;
  info.index_offset = info.roots_offset + info.root_count * info.ref_byte_size;
  info.data_offset = info.index_offset;
  if (info.has_index) {
    info.data_offset += (long long)cell_count * info.offset_byte_size;
  }
  info.magic = BagOfCells::Info::boc_generic;
  info.data_size = data_bytes_adj;
  info.total_size = info.data_offset + data_bytes_adj + crc_size;
  auto res = td::narrow_cast_safe<size_t>(info.total_size);
  if (res.is_error()) {
    return td::Status::Error("bag of cells is too large");
  }

  boc_writers::FileWriter writer{fd, (size_t)info.total_size};
  auto store_ref = [&](unsigned long long value) { writer.store_uint(value, info.ref_byte_size); };
  auto store_offset = [&](unsigned long long value) { writer.store_uint(value, info.offset_byte_size); };

  writer.store_uint(info.magic, 4);

  td::uint8 byte{0};
  if (info.has_index) {
    byte |= 1 << 7;
  }
  if (info.has_crc32c) {
    byte |= 1 << 6;
  }
  if (info.has_cache_bits) {
    byte |= 1 << 5;
  }
  byte |= (td::uint8)info.ref_byte_size;
  writer.store_uint(byte, 1);

  writer.store_uint(info.offset_byte_size, 1);
  store_ref(cell_count);
  store_ref(roots.size());
  store_ref(0);
  store_offset(info.data_size);
  for (const auto& root_info : roots) {
    int k = cell_count - 1 - root_info.idx;
    DCHECK(k >= 0 && k < cell_count);
    store_ref(k);
  }
  DCHECK(writer.position() == info.index_offset);
  DCHECK((unsigned)cell_count == cell_list.size());
  if (info.has_index) {
    if (logger_ptr_) {
      logger_ptr_->start_stage("generate_index");
    }
    std::size_t offs = 0;
    for (int i = cell_count - 1; i >= 0; --i) {
      const auto& dc_info = cell_list[i]->second;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      int hash_size = 0;
      if (with_hash) {
        hash_size = (Cell::hash_bytes + Cell::depth_bytes) * dc_info.hcnt;
      }
      offs += dc_info.serialized_size + hash_size + dc_info.get_ref_num() * info.ref_byte_size;
      auto fixed_offset = offs;
      if (info.has_cache_bits) {
        fixed_offset = offs * 2 + dc_info.should_cache;
      }
      store_offset(fixed_offset);
      if (logger_ptr_) {
        TRY_STATUS(logger_ptr_->on_cells_processed(1));
      }
    }
    DCHECK(offs == info.data_size);
    if (logger_ptr_) {
      logger_ptr_->finish_stage("");
    }
  }
  DCHECK(writer.position() == info.data_offset);
  size_t keep_position = writer.position();
  if (logger_ptr_) {
    logger_ptr_->start_stage("serialize");
  }
  for (int batch_start = 0; batch_start < cell_count; batch_start += load_batch_size) {
    int batch_end = std::min(batch_start + static_cast<int>(load_batch_size), cell_count);
    
    std::vector<td::Slice> batch_hashes;
    batch_hashes.reserve(batch_end - batch_start);
    for (int i = batch_start; i < batch_end; ++i) {
      int cell_index = cell_count - 1 - i;
      batch_hashes.push_back(cell_list[cell_index]->first.as_slice());
    }
    
    TRY_RESULT(batch_cells, reader->load_bulk(std::move(batch_hashes)));
    
    for (int i = batch_start; i < batch_end; ++i) {
      int idx_in_batch = i - batch_start;
      int cell_index = cell_count - 1 - i;
      
      const auto& dc_info = cell_list[cell_index]->second;
      auto& dc = batch_cells[idx_in_batch];
      
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      unsigned char buf[256];
      int s = dc->serialize(buf, 256, with_hash);
      writer.store_bytes(buf, s);
      DCHECK(dc->size_refs() == dc_info.get_ref_num());
      unsigned ref_num = dc_info.get_ref_num();
      for (unsigned j = 0; j < ref_num; ++j) {
        int k = cell_count - 1 - dc_info.ref_idx[j];
        DCHECK(k > i && k < cell_count);
        store_ref(k);
      }
    }
    if (logger_ptr_) {
      TRY_STATUS(logger_ptr_->on_cells_processed(batch_hashes.size()));
    }
  }
  DCHECK(writer.position() - keep_position == info.data_size);
  if (info.has_crc32c) {
    unsigned crc = writer.get_crc32();
    writer.store_uint(td::bswap32(crc), 4);
  }
  DCHECK(writer.empty());
  TRY_STATUS(writer.finalize());
  if (logger_ptr_) {
    logger_ptr_->finish_stage(PSLICE() << cell_count << " cells, " << writer.position() << " bytes");
  }
  return td::Status::OK();
}
}  // namespace

td::Status std_boc_serialize_to_file_large(std::shared_ptr<CellDbReader> reader, Cell::Hash root_hash, td::FileFd& fd,
                                           int mode, td::CancellationToken cancellation_token) {
  td::Timer timer;
  CHECK(reader != nullptr)
  LargeBocSerializer serializer(reader);
  BagOfCellsLogger logger(std::move(cancellation_token));
  serializer.set_logger(&logger);
  serializer.add_root(root_hash);
  TRY_STATUS(serializer.import_cells());
  TRY_STATUS(serializer.serialize(fd, mode));
  LOG(ERROR) << "serialization took " << timer.elapsed() << "s";
  return td::Status::OK();
}

}  // namespace vm
