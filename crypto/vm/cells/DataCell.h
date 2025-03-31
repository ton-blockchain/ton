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

#include "td/utils/Span.h"
#include "td/utils/ThreadSafeCounter.h"
#include "vm/cells/Cell.h"

namespace vm {

namespace detail {

struct LevelInfo {
  CellHash hash;
  td::uint16 depth;
};

}  // namespace detail

class DataCell final : public Cell {
 public:
  // NB: cells created with use_arena=true are never freed
  static thread_local bool use_arena;

  static td::Result<Ref<DataCell>> create(td::Slice data, int bit_length, td::Span<Ref<Cell>> refs, bool is_special);

  static void store_depth(td::uint8* dest, td::uint16 depth) {
    td::bitstring::bits_store_long(dest, depth, depth_bits);
  }

  static td::uint16 load_depth(const td::uint8* src) {
    return td::bitstring::bits_load_ulong(src, depth_bits) & 0xffff;
  }

  void operator delete(DataCell* ptr, std::destroying_delete_t) {
    bool allocated_in_arena = ptr->allocated_in_arena_;
    ptr->~DataCell();
    if (!allocated_in_arena) {
      ::operator delete(ptr);
    }
  }

  DataCell(DataCell const&) = delete;
  DataCell(DataCell&&) = delete;

  ~DataCell();

  virtual td::Status set_data_cell(Ref<DataCell>&& data_cell) const override {
    CHECK(get_hash() == data_cell->get_hash());
    return td::Status::OK();
  }

  virtual td::Result<LoadedCell> load_cell() const override {
    return LoadedCell{
        .data_cell = Ref<DataCell>{this},
        .virt = {},
        .tree_node = {},
    };
  }

  virtual td::uint32 get_virtualization() const override {
    return virtualization_;
  }

  virtual CellUsageTree::NodePtr get_tree_node() const override {
    return {};
  }

  virtual bool is_loaded() const override {
    return true;
  }

  virtual LevelMask get_level_mask() const override {
    return LevelMask{level_mask_};
  }

  unsigned get_refs_cnt() const {
    return refs_cnt_;
  }

  unsigned get_bits() const {
    return bit_length_;
  }

  unsigned size_refs() const {
    return refs_cnt_;
  }

  unsigned size() const {
    return bit_length_;
  }

  unsigned char const* get_data() const {
    return reinterpret_cast<unsigned char const*>(trailer_ + sizeof(detail::LevelInfo) * (level_ + 1));
  }

  Ref<Cell> get_ref(unsigned idx) const {
    if (idx >= refs_cnt_) {
      return {};
    }
    return refs_[idx];
  }

  Cell* get_ref_raw_ptr(unsigned idx) const {
    DCHECK(idx < refs_cnt_);
    return const_cast<Cell*>(refs_[idx].get());
  }

  Ref<Cell> reset_ref_unsafe(unsigned idx, Ref<Cell> ref, bool check_hash = true) {
    CHECK(idx < get_refs_cnt());
    CHECK(!check_hash || refs_[idx]->get_hash() == ref->get_hash());
    return std::exchange(refs_[idx], std::move(ref));
  }

  bool is_special() const {
    return type_ != static_cast<td::uint8>(SpecialType::Ordinary);
  }

  SpecialType special_type() const {
    return static_cast<SpecialType>(type_);
  }

  int get_serialized_size(bool with_hashes = false) const {
    return ((get_bits() + 23) >> 3) +
           (with_hashes ? get_level_mask().get_hashes_count() * (hash_bytes + depth_bytes) : 0);
  }

  size_t get_storage_size() const {
    return sizeof(DataCell) + sizeof(detail::LevelInfo) * (level_ + 1) + (bit_length_ + 7) / 8;
  }

  int serialize(unsigned char* buff, int buff_size, bool with_hashes = false) const;

  std::string serialize() const;

  std::string to_hex() const;

  static td::int64 get_total_data_cells() {
    return get_thread_safe_counter().sum();
  }

  template <class StorerT>
  void store(StorerT& storer) const {
    storer.template store_binary<td::uint8>(construct_d1(max_level));
    storer.template store_binary<td::uint8>(construct_d2());
    storer.store_slice(td::Slice(get_data(), (get_bits() + 7) / 8));
  }

 private:
  static td::NamedThreadSafeCounter::CounterRef get_thread_safe_counter() {
    static auto res = td::NamedThreadSafeCounter::get_default().get_counter("DataCell");
    return res;
  }

  DataCell(int bit_length, size_t refs_cnt, Cell::SpecialType type, LevelMask level_mask, bool allocated_in_arena,
           td::uint8 virtualization);

  detail::LevelInfo const* level_info() const {
    return reinterpret_cast<detail::LevelInfo const*>(trailer_);
  }

  virtual td::uint16 do_get_depth(td::uint32 level) const override {
    return level_info()[std::min<td::uint32>(level_, level)].depth;
  }

  virtual const Hash do_get_hash(td::uint32 level) const override {
    return level_info()[std::min<td::uint32>(level_, level)].hash;
  }

  td::uint8 construct_d1(td::uint32 level) const {
    return static_cast<td::uint8>(refs_cnt_ + (is_special() << 3) + (get_level_mask().apply(level).get_mask() << 5));
  }

  td::uint8 construct_d2() const {
    return static_cast<td::uint8>(bit_length_ / 8 + (bit_length_ + 7) / 8);
  }

  unsigned bit_length_ : 11;
  unsigned refs_cnt_ : 3;
  unsigned type_ : 3;
  unsigned level_ : 3;
  unsigned level_mask_ : 3;
  unsigned allocated_in_arena_ : 1;
  unsigned virtualization_ : 8;

  std::array<Ref<Cell>, max_refs> refs_{};

  alignas(detail::LevelInfo) char trailer_[];
};

inline std::ostream& operator<<(std::ostream& os, const DataCell& c) {
  return os << c.to_hex();
}

inline CellHash as_cell_hash(const Ref<DataCell>& cell) {
  return cell->get_hash();
}

}  // namespace vm
