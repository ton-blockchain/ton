#include "large-boc-serializer.hpp"
#include "td/utils/HashMap.h"
#include "crypto/vm/cellslice.h"
#include "crypto/vm/boc.h"
#include "td/utils/crypto.h"
#include "td/utils/port/Stat.h"

namespace ton {
namespace validator {

namespace {
class LargeBocSerializer {
 public:
  using Hash = vm::Cell::Hash;

  explicit LargeBocSerializer(std::shared_ptr<vm::CellDbReader> reader) : reader(std::move(reader)) {}

  void add_root(Hash root);
  td::Status import_cells();
  td::Status serialize(td::FileFd& fd, int mode);

 private:
  std::shared_ptr<vm::CellDbReader> reader;
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
      unsigned ref_num = 4;
      while (ref_num > 0 && ref_idx[ref_num - 1] == -1) {
        --ref_num;
      }
      return ref_num;
    }
  };
  std::map<Hash, CellInfo> cells;
  std::vector<std::pair<const Hash, CellInfo>*> cell_list;
  struct RootInfo {
    RootInfo(Hash hash, int idx) : hash(hash), idx(idx) {}
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
};

void LargeBocSerializer::add_root(Hash root) {
  roots.emplace_back(root, -1);
}

td::Status LargeBocSerializer::import_cells() {
  for (auto& root : roots) {
    TRY_RESULT(idx, import_cell(root.hash));
    root.idx = idx;
  }
  reorder_cells();
  CHECK(!cell_list.empty());
  return td::Status::OK();
}

td::Result<int> LargeBocSerializer::import_cell(Hash hash, int depth) {
  if (depth > vm::Cell::max_depth) {
    return td::Status::Error("error while importing a cell into a bag of cells: cell depth too large");
  }
  auto it = cells.find(hash);
  if (it != cells.end()) {
    it->second.should_cache = true;
    return it->second.idx;
  }
  TRY_RESULT(cell, reader->load_cell(hash.as_slice()));
  if (cell->get_virtualization() != 0) {
    return td::Status::Error(
        "error while importing a cell into a bag of cells: cell has non-zero virtualization level");
  }
  vm::CellSlice cs(std::move(cell));
  std::array<int, 4> refs;
  std::fill(refs.begin(), refs.end(), -1);
  DCHECK(cs.size_refs() <= 4);
  unsigned sum_child_wt = 1;
  for (unsigned i = 0; i < cs.size_refs(); i++) {
    TRY_RESULT(ref, import_cell(cs.prefetch_ref(i)->get_hash(), depth + 1));
    refs[i] = ref;
    sum_child_wt += cell_list[ref]->second.wt;
    ++int_refs;
  }
  auto dc = cs.move_as_loaded_cell().data_cell;
  auto res = cells.emplace(hash, CellInfo(cell_count, refs));
  DCHECK(res.second);
  cell_list.push_back(&*res.first);
  CellInfo& dc_info = res.first->second;
  dc_info.wt = (unsigned char)std::min(0xffU, sum_child_wt);
  unsigned hcnt = dc->get_level_mask().get_hashes_count();
  DCHECK(hcnt <= 4);
  dc_info.hcnt = (unsigned char)hcnt;
  TRY_RESULT(serialized_size, td::narrow_cast_safe<unsigned short>(dc->get_serialized_size()));
  data_bytes += dc_info.serialized_size = serialized_size;
  return cell_count++;
}

void LargeBocSerializer::reorder_cells() {
  for (auto ptr : cell_list) {
    ptr->second.idx = -1;
  }
  int_hashes = 0;
  for (int i = cell_count - 1; i >= 0; --i) {
    CellInfo& dci = cell_list[i]->second;
    int s = dci.get_ref_num(), c = s, sum = vm::BagOfCells::max_cell_whs - 1, mask = 0;
    for (int j = 0; j < s; ++j) {
      CellInfo& dcj = cell_list[dci.ref_idx[j]]->second;
      int limit = (vm::BagOfCells::max_cell_whs - 1 + j) / s;
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
    DCHECK(sum <= vm::BagOfCells::max_cell_whs);
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
  using Mode = vm::BagOfCells::Mode;
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
      (vm::Cell::hash_bytes + vm::Cell::depth_bytes);
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

struct BufferWriter {
  BufferWriter(unsigned char* store_start, unsigned char* store_end)
      : store_start(store_start), store_ptr(store_start), store_end(store_end) {}

  size_t position() const {
    return store_ptr - store_start;
  }
  size_t remaining() const {
    return store_end - store_ptr;
  }
  void chk() const {
    DCHECK(store_ptr <= store_end);
  }
  bool empty() const {
    return store_ptr == store_end;
  }
  void store_uint(unsigned long long value, unsigned bytes) {
    unsigned char* ptr = store_ptr += bytes;
    chk();
    while (bytes) {
      *--ptr = value & 0xff;
      value >>= 8;
      --bytes;
    }
    DCHECK(!bytes);
  }
  void store_bytes(unsigned char const* data, size_t s) {
    store_ptr += s;
    chk();
    memcpy(store_ptr - s, data, s);
  }
  unsigned get_crc32() const {
    return td::crc32c(td::Slice{store_start, store_ptr});
  }

 private:
  unsigned char* store_start;
  unsigned char* store_ptr;
  unsigned char* store_end;
};

struct FileWriter {
  explicit FileWriter(td::FileFd& fd) : fd(fd) {}

  ~FileWriter() {
    flush();
  }

  size_t position() const {
    return flushed_size + writer.position();
  }
  void store_uint(unsigned long long value, unsigned bytes) {
    flush_if_needed(bytes);
    writer.store_uint(value, bytes);
  }
  void store_bytes(unsigned char const* data, size_t s) {
    flush_if_needed(s);
    writer.store_bytes(data, s);
  }
  unsigned get_crc32() const {
    unsigned char const* start = buf.data();
    unsigned char const* end = start + writer.position();
    return td::crc32c_extend(current_crc32, td::Slice(start, end));
  }

  td::Status finalize() {
    flush();
    return std::move(res);
  }

 private:
  void flush_if_needed(size_t s) {
    DCHECK(s <= BUF_SIZE);
    if (s > BUF_SIZE - writer.position()) {
      flush();
    }
  }

  void flush() {
    unsigned char* start = buf.data();
    unsigned char* end = start + writer.position();
    if (start == end) {
      return;
    }
    flushed_size += end - start;
    current_crc32 = td::crc32c_extend(current_crc32, td::Slice(start, end));
    if (res.is_ok()) {
      while (end > start) {
        auto R = fd.write(td::Slice(start, end));
        if (R.is_error()) {
          res = R.move_as_error();
          break;
        }
        size_t s = R.move_as_ok();
        start += s;
      }
    }
    writer = BufferWriter(buf.data(), buf.data() + buf.size());
  }

  td::FileFd& fd;
  size_t flushed_size = 0;
  unsigned current_crc32 = td::crc32c(td::Slice());

  static const size_t BUF_SIZE = 1 << 22;
  std::vector<unsigned char> buf = std::vector<unsigned char>(BUF_SIZE, '\0');
  BufferWriter writer = BufferWriter(buf.data(), buf.data() + buf.size());
  td::Status res = td::Status::OK();
};

td::Status LargeBocSerializer::serialize(td::FileFd& fd, int mode) {
  using Mode = vm::BagOfCells::Mode;
  vm::BagOfCells::Info info;
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
  info.magic = vm::BagOfCells::Info::boc_generic;
  info.data_size = data_bytes_adj;
  info.total_size = info.data_offset + data_bytes_adj + crc_size;
  auto res = td::narrow_cast_safe<size_t>(info.total_size);
  if (res.is_error()) {
    return td::Status::Error("bag of cells is too large");
  }

  FileWriter writer{fd};
  auto store_ref = [&](unsigned long long value) {
    writer.store_uint(value, info.ref_byte_size);
  };
  auto store_offset = [&](unsigned long long value) {
    writer.store_uint(value, info.offset_byte_size);
  };

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
    std::size_t offs = 0;
    for (int i = cell_count - 1; i >= 0; --i) {
      const auto& dc_info = cell_list[i]->second;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      int hash_size = 0;
      if (with_hash) {
        hash_size = (vm::Cell::hash_bytes + vm::Cell::depth_bytes) * dc_info.hcnt;
      }
      offs += dc_info.serialized_size + hash_size + dc_info.get_ref_num() * info.ref_byte_size;
      auto fixed_offset = offs;
      if (info.has_cache_bits) {
        fixed_offset = offs * 2 + dc_info.should_cache;
      }
      store_offset(fixed_offset);
    }
    DCHECK(offs == info.data_size);
  }
  DCHECK(writer.position() == info.data_offset);
  size_t keep_position = writer.position();
  for (int i = 0; i < cell_count; ++i) {
    auto hash = cell_list[cell_count - 1 - i]->first;
    const auto& dc_info = cell_list[cell_count - 1 - i]->second;
    TRY_RESULT(dc, reader->load_cell(hash.as_slice()));
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
  DCHECK(writer.position() - keep_position == info.data_size);
  if (info.has_crc32c) {
    unsigned crc = writer.get_crc32();
    writer.store_uint(td::bswap32(crc), 4);
  }
  return writer.finalize();
}
}

td::Status serialize_large_boc_to_file(std::shared_ptr<vm::CellDbReader> reader, vm::Cell::Hash root_hash,
                                       td::FileFd& fd, int mode) {
  CHECK(reader != nullptr)
  LargeBocSerializer serializer(reader);
  serializer.add_root(root_hash);
  TRY_STATUS(serializer.import_cells());
  return serializer.serialize(fd, mode);
}

}
}
