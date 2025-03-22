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

#include "vm/cells/DataCell.h"

namespace vm {
struct PrunnedCellInfo {
  Cell::LevelMask level_mask;
  td::Slice hash;
  td::Slice depth;
};

template <class ExtraT>
class PrunnedCell final : public Cell {
 public:
  ExtraT& get_extra() {
    return extra_;
  }
  const ExtraT& get_extra() const {
    return extra_;
  }

  void operator delete(PrunnedCell* ptr, std::destroying_delete_t) {
    bool allocated_in_arena = ptr->info_.allocated_in_arena_;
    ptr->~PrunnedCell();
    if (!allocated_in_arena) {
      ::operator delete(ptr);
    }
  }

  static td::Result<Ref<PrunnedCell<ExtraT>>> create(const PrunnedCellInfo& prunned_cell_info, ExtraT&& extra) {
    auto allocator = [](size_t bytes) { return ::operator new(bytes); };
    return create(allocator, true, prunned_cell_info, std::move(extra));
  }

  template <typename AllocatorFunc>
  static td::Result<Ref<PrunnedCell<ExtraT>>> create(AllocatorFunc&& allocator, bool should_free,
                                                     const PrunnedCellInfo& prunned_cell_info, ExtraT&& extra) {
    auto level_mask = prunned_cell_info.level_mask;
    if (level_mask.get_level() > max_level) {
      return td::Status::Error("Level is too big");
    }
    Info info(level_mask);

    auto storage = allocator(sizeof(PrunnedCell) + info.get_storage_size());
    auto* result = new (storage) PrunnedCell{info, std::move(extra)};
    result->info_.allocated_in_arena_ = !should_free;
    TRY_STATUS(result->init(prunned_cell_info));
    return Ref<PrunnedCell<ExtraT>>(result, typename Ref<PrunnedCell<ExtraT>>::acquire_t{});
  }

  LevelMask get_level_mask() const override {
    return LevelMask(info_.level_mask_);
  }

 protected:
  static constexpr auto max_storage_size = (max_level + 1) * (hash_bytes + sizeof(td::uint16));
  struct Info {
    Info(LevelMask level_mask) {
      level_mask_ = level_mask.get_mask() & 7;
      hash_count_ = level_mask.get_hashes_count() & 7;
    }
    unsigned char level_mask_ : 3;
    unsigned char hash_count_ : 3;
    unsigned char allocated_in_arena_ : 1;
    size_t get_hashes_offset() const {
      return 0;
    }
    size_t get_depth_offset() const {
      return get_hashes_offset() + hash_bytes * hash_count_;
    }
    size_t get_storage_size() const {
      return get_depth_offset() + sizeof(td::uint16) * hash_count_;
    }
    const Hash* get_hashes(const char* storage) const {
      return reinterpret_cast<const Hash*>(storage + get_hashes_offset());
    }
    Hash* get_hashes(char* storage) const {
      return reinterpret_cast<Hash*>(storage + get_hashes_offset());
    }
    const td::uint16* get_depth(const char* storage) const {
      return reinterpret_cast<const td::uint16*>(storage + get_depth_offset());
    }
    td::uint16* get_depth(char* storage) const {
      return reinterpret_cast<td::uint16*>(storage + get_depth_offset());
    }
  };

  Info info_;
  ExtraT extra_;

  td::Status init(const PrunnedCellInfo& prunned_cell_info) {
    auto& new_hash = prunned_cell_info.hash;
    auto* hash = info_.get_hashes(trailer_);
    size_t n = prunned_cell_info.level_mask.get_hashes_count();
    CHECK(new_hash.size() == n * hash_bytes);
    for (td::uint32 i = 0; i < n; i++) {
      hash[i].as_slice().copy_from(new_hash.substr(i * Cell::hash_bytes, Cell::hash_bytes));
    }

    auto& new_depth = prunned_cell_info.depth;
    CHECK(new_depth.size() == n * depth_bytes);
    auto* depth = info_.get_depth(trailer_);
    for (td::uint32 i = 0; i < n; i++) {
      depth[i] = DataCell::load_depth(new_depth.substr(i * Cell::depth_bytes, Cell::depth_bytes).ubegin());
      if (depth[i] > max_depth) {
        return td::Status::Error("Depth is too big");
      }
    }
    return td::Status::OK();
  }

  explicit PrunnedCell(Info info, ExtraT&& extra) : info_(info), extra_(std::move(extra)) {
  }
  td::uint32 get_virtualization() const override {
    return 0;
  }
  CellUsageTree::NodePtr get_tree_node() const override {
    return {};
  }
  bool is_loaded() const override {
    return false;
  }

 private:
  const Hash do_get_hash(td::uint32 level) const override {
    return info_.get_hashes(trailer_)[get_level_mask().apply(level).get_hash_i()];
  }

  td::uint16 do_get_depth(td::uint32 level) const override {
    return info_.get_depth(trailer_)[get_level_mask().apply(level).get_hash_i()];
  }

  td::Status set_data_cell(Ref<DataCell> &&data_cell) const override {
    return td::Status::OK();
  }

  td::Result<LoadedCell> load_cell() const override {
    return td::Status::Error("Can't load prunned branch");
  }

  char trailer_[];
};
}  // namespace vm
