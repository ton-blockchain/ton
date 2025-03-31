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

#include "openssl/digest.hpp"
#include "vm/cells/DataCell.h"

namespace vm {

namespace {

class CellChecker {
 public:
  CellChecker(bool is_special, td::Slice data, int bit_length, td::Span<Ref<Cell>> refs)
      : is_special_(is_special)
      , refs_(refs)
      , refs_cnt_(static_cast<int>(refs.size()))
      , data_(data)
      , bit_length_(bit_length) {
  }

  td::Status check_and_compute_level_info() {
    // First, we figure out what is the type of the cell.
    type_ = Cell::SpecialType::Ordinary;

    if (is_special_) {
      if (bit_length_ < 8) {
        return td::Status::Error("Not enough data for a special cell");
      }

      type_ = static_cast<Cell::SpecialType>(read_byte(0));
      if (type_ == Cell::SpecialType::Ordinary) {
        return td::Status::Error("Invalid special cell type");
      }
    }

    // Next, we populate everything except for virtualization and hashes. `check_*` functions also
    // perform type-specific checks.
    switch (type_) {
      case Cell::SpecialType::Ordinary:
        TRY_STATUS(check_ordinary_cell());
        break;
      case Cell::SpecialType::PrunnedBranch:
        TRY_STATUS(check_pruned_branch());
        break;
      case Cell::SpecialType::Library:
        TRY_STATUS(check_library());
        break;
      case Cell::SpecialType::MerkleProof:
        TRY_STATUS(check_merkle_proof());
        break;
      case Cell::SpecialType::MerkleUpdate:
        TRY_STATUS(check_merkle_update());
        break;
      default:
        return td::Status::Error("Invalid special cell type");
    }

    // Afterwards, we do some common checks and compute virtualization level.
    if (*std::max_element(depth_.begin(), depth_.end()) > CellTraits::max_depth) {
      return td::Status::Error("Depth is too big");
    }

    for (int i = 0; i < refs_cnt_; ++i) {
      virtualization_ = std::max(virtualization_, refs_[i]->get_virtualization());
    }
    if (virtualization_ > std::numeric_limits<td::uint8>::max()) {
      return td::Status::Error("Virtualization is too big to be stored in vm::DataCell");
    }

    // And finally, we compute cell hashes.
    // NOTE: Hash computation algorithm is not described correctly (or at all) in the documentation.
    int last_computed_hash = -1;

    for (int i = 0; i <= max_level; ++i) {
      if (!level_mask_.is_significant(i + 1) && i != max_level) {
        continue;
      }

      compute_hash(i, last_computed_hash);
      for (int j = last_computed_hash + 1; j < i; ++j) {
        hash_[j] = hash_[i];
      }
      last_computed_hash = i;
    }

    return {};
  }

  // Getters for computed values
  Cell::SpecialType type() const {
    return type_;
  }

  Cell::LevelMask level_mask() const {
    return level_mask_;
  }

  td::uint8 virtualization() const {
    return static_cast<td::uint8>(virtualization_);
  }

  std::array<td::uint16, 4> const& depths() const {
    return depth_;
  }

  std::array<CellHash, 4> const& hashes() const {
    return hash_;
  }

 private:
  static constexpr int max_level = CellTraits::max_level;

  static constexpr int hash_bytes = CellTraits::hash_bytes;
  static_assert(hash_bytes == sizeof(CellHash));

  static constexpr int depth_bytes = CellTraits::depth_bytes;
  static_assert(depth_bytes == 2);

  td::uint8 read_byte(size_t i) {
    return data_[i];
  }

  td::Status check_ordinary_cell() {
    for (int i = 0; i < refs_cnt_; ++i) {
      level_mask_ = level_mask_.apply_or(refs_[i]->get_level_mask());

      for (int j = 0; j <= max_level; ++j) {
        depth_[j] = std::max(depth_[j], refs_[i]->get_depth(j));
      }
    }

    if (refs_cnt_ != 0) {
      for (auto& depth : depth_) {
        ++depth;
      }
    }

    return {};
  }

  td::Status check_pruned_branch() {
    if (refs_cnt_ != 0) {
      return td::Status::Error("Pruned branch cannot have references");
    }
    if (bit_length_ < 16) {
      return td::Status::Error("Length mismatch in a pruned branch");
    }

    level_mask_ = Cell::LevelMask{read_byte(1)};
    if (level_mask_.get_level() == 0 || level_mask_.get_level() > max_level) {
      return td::Status::Error("Invalid level mask in a pruned branch");
    }

    int hashes_count = level_mask_.get_hash_i();
    auto expected_byte_size = 2 + hashes_count * (hash_bytes + depth_bytes);

    if (bit_length_ != static_cast<int>(expected_byte_size * 8)) {
      return td::Status::Error("Length mismatch in a pruned branch");
    }

    // depth[max_level] = 0;

    for (int i = max_level; i--;) {
      if (level_mask_.is_significant(i + 1)) {
        int hashes_before = level_mask_.apply(i).get_hash_i();
        auto offset = 2 + hashes_count * hash_bytes + hashes_before * depth_bytes;
        depth_[i] = DataCell::load_depth(data_.ubegin() + offset);
      } else {
        depth_[i] = depth_[i + 1];
      }
    }

    return {};
  }

  td::Status check_library() {
    if (refs_cnt_ != 0) {
      return td::Status::Error("Library cell cannot have references");
    }
    if (bit_length_ != 8 * (1 + hash_bytes)) {
      return td::Status::Error("Length mismatch in a library cell");
    }

    return {};
  }

  td::Status check_merkle_child(int child_idx, int hash_offset, int depth_offset) {
    CellHash stored_hash;
    std::memcpy(&stored_hash, data_.begin() + hash_offset, hash_bytes);
    if (stored_hash != refs_[child_idx]->get_hash(0)) {
      return td::Status::Error("Invalid hash in a Merkle proof or update");
    }

    td::uint16 stored_depth = DataCell::load_depth(data_.ubegin() + depth_offset);
    if (stored_depth != refs_[child_idx]->get_depth(0)) {
      return td::Status::Error("Invalid depth in a Merkle proof or update");
    }

    for (int i = 0; i <= max_level; ++i) {
      depth_[i] = std::max<td::uint16>(depth_[i], refs_[child_idx]->get_depth(i + 1) + 1);
    }

    return {};
  }

  td::Status check_merkle_proof() {
    if (refs_cnt_ != 1) {
      return td::Status::Error("Merkle proof must have exactly one reference");
    }
    if (bit_length_ != 8 * (1 + hash_bytes + depth_bytes)) {
      return td::Status::Error("Length mismatch in a Merkle proof");
    }

    TRY_STATUS(check_merkle_child(0, 1, 1 + hash_bytes));

    level_mask_ = refs_[0]->get_level_mask().shift_right();

    return {};
  }

  td::Status check_merkle_update() {
    if (refs_cnt_ != 2) {
      return td::Status::Error("Merkle update must have exactly two references");
    }
    if (bit_length_ != 8 * (1 + (hash_bytes + depth_bytes) * 2)) {
      return td::Status::Error("Length mismatch in a Merkle update");
    }

    TRY_STATUS(check_merkle_child(0, 1, 1 + 2 * hash_bytes));
    TRY_STATUS(check_merkle_child(1, 1 + hash_bytes, 1 + 2 * hash_bytes + 2));

    level_mask_ = refs_[0]->get_level_mask().apply_or(refs_[1]->get_level_mask()).shift_right();

    return {};
  }

  void compute_hash(int level, int last_computed_hash) {
    if (level != max_level && type_ == Cell::SpecialType::PrunnedBranch) {
      int hashes_before = level_mask_.apply(level).get_hash_i();
      auto offset = 2 + hashes_before * hash_bytes;
      std::memcpy(&hash_[level], data_.begin() + offset, hash_bytes);
      return;
    }

    static_assert(2 + CellTraits::max_bytes + CellTraits::max_refs * (hash_bytes + depth_bytes) <= 512);
    char data_to_hash[512];
    int pointer = 0;

    auto add_byte_to_hash = [&](char byte) { data_to_hash[pointer++] = byte; };

    auto add_slice_to_hash = [&](td::Slice slice) {
      std::memcpy(data_to_hash + pointer, slice.data(), slice.size());
      pointer += slice.size();
    };

    auto d1 = refs_cnt_ + (is_special_ << 3) + (level_mask_.apply(level).get_mask() << 5);
    add_byte_to_hash(static_cast<td::uint8>(d1));
    auto d2 = (bit_length_ >> 3 << 1) + ((bit_length_ & 7) != 0);
    add_byte_to_hash(static_cast<td::uint8>(d2));

    if (last_computed_hash != -1 && type_ != Cell::SpecialType::PrunnedBranch) {
      add_slice_to_hash(hash_[last_computed_hash].as_slice());
    } else {
      add_slice_to_hash(data_.substr(0, bit_length_ / 8));
      // If we are not byte-aligned, some bit gymnastics is required to correctly pad the last byte.
      if (bit_length_ % 8 != 0) {
        td::uint8 last_byte = data_[bit_length_ / 8];
        last_byte >>= 7 - bit_length_ % 8;
        last_byte |= 1;
        last_byte <<= 7 - bit_length_ % 8;
        add_byte_to_hash(last_byte);
      }
    }

    bool is_merkle_node = type_ == Cell::SpecialType::MerkleUpdate || type_ == Cell::SpecialType::MerkleProof;
    auto child_level = (is_merkle_node ? std::min(max_level, level + 1) : level);

    for (int i = 0; i < refs_cnt_; ++i) {
      auto depth = refs_[i]->get_depth(child_level);
      add_byte_to_hash((depth >> 8) & 255);
      add_byte_to_hash((depth >> 0) & 255);
    }

    for (int i = 0; i < refs_cnt_; ++i) {
      add_slice_to_hash(refs_[i]->get_hash(child_level).as_slice());
    }

    digest::SHA256 hasher;
    hasher.feed(data_to_hash, pointer);
    hasher.extract(hash_[level].as_slice());
  }

  bool is_special_;
  Cell::SpecialType type_;
  td::Span<Ref<Cell>> refs_;
  int refs_cnt_;
  td::Slice data_;
  int bit_length_;

  Cell::LevelMask level_mask_;
  td::uint32 virtualization_{0};
  std::array<td::uint16, max_level + 1> depth_{};
  std::array<CellHash, max_level + 1> hash_{};
};

char* allocate_in_arena(size_t size) {
  constexpr size_t batch_size = 1 << 20;
  thread_local td::MutableSlice batch;

  auto aligned_size = (size + 7) / 8 * 8;
  if (batch.size() < size) {
    batch = td::MutableSlice(new char[batch_size], batch_size);
  }
  auto res = batch.begin();
  batch.remove_prefix(aligned_size);
  return res;
}

}  // namespace

thread_local bool DataCell::use_arena = false;

td::Result<Ref<DataCell>> DataCell::create(td::Slice data, int bit_length, td::Span<Ref<Cell>> refs, bool is_special) {
  CHECK(bit_length >= 0 && data.size() * 8 >= static_cast<size_t>(bit_length));
  if (refs.size() > CellTraits::max_refs) {
    return td::Status::Error("Too many references");
  }
  if (bit_length > CellTraits::max_bits) {
    return td::Status::Error("Too many data bits");
  }

  CellChecker checker{is_special, data, bit_length, refs};
  TRY_STATUS(checker.check_and_compute_level_info());

  auto level_info_size = sizeof(detail::LevelInfo) * (checker.level_mask().get_level() + 1);
  auto cell_size = sizeof(DataCell) + level_info_size + (bit_length + 7) / 8;

  void* storage = use_arena ? allocate_in_arena(cell_size) : ::operator new(cell_size);
  DataCell* allocated_cell = new (storage)
      DataCell{bit_length, refs.size(), checker.type(), checker.level_mask(), use_arena, checker.virtualization()};
  auto& cell = *allocated_cell;

  auto mutable_data = cell.trailer_ + level_info_size;

  std::memcpy(mutable_data, data.data(), (bit_length + 7) / 8);
  if (bit_length % 8 != 0) {
    auto& last_byte = mutable_data[bit_length / 8];
    // This is the same padding as was used in CellChecker::compute_hash above.
    last_byte >>= (7 - bit_length % 8);
    last_byte |= 1;
    last_byte <<= (7 - bit_length % 8);
  }

  auto level_info = new (cell.trailer_) detail::LevelInfo[checker.level_mask().get_level() + 1];

  for (int i = 0; i <= cell.level_; ++i) {
    level_info[i] = {
        .hash = checker.hashes()[i],
        .depth = checker.depths()[i],
    };
  }

  for (int i = 0; i < cell.refs_cnt_; ++i) {
    cell.refs_[i] = refs[i];
  }

  return Ref{allocated_cell, Ref<DataCell>::acquire_t{}};
}

DataCell::~DataCell() {
  for (size_t i = 0; i < level_ + 1; ++i) {
    level_info()[i].~LevelInfo();
  }
  get_thread_safe_counter().add(-1);
}

int DataCell::serialize(unsigned char* buff, int buff_size, bool with_hashes) const {
  int len = get_serialized_size(with_hashes);
  if (len > buff_size) {
    return 0;
  }
  buff[0] = static_cast<unsigned char>(construct_d1(max_level) | (with_hashes * 16));
  buff[1] = construct_d2();
  int hs = 0;
  if (with_hashes) {
    hs = (get_level_mask().get_hashes_count()) * (hash_bytes + depth_bytes);
    assert(len >= 2 + hs);
    std::memset(buff + 2, 0, hs);
    auto dest = td::MutableSlice(buff + 2, hs);
    auto level = get_level();
    // TODO: optimize for pruned branch
    for (unsigned i = 0; i <= level; i++) {
      if (!get_level_mask().is_significant(i)) {
        continue;
      }
      dest.copy_from(get_hash(i).as_slice());
      dest.remove_prefix(hash_bytes);
    }
    for (unsigned i = 0; i <= level; i++) {
      if (!get_level_mask().is_significant(i)) {
        continue;
      }
      store_depth(dest.ubegin(), get_depth(i));
      dest.remove_prefix(depth_bytes);
    }
    buff += hs;
    len -= hs;
  }
  std::memcpy(buff + 2, get_data(), len - 2);
  return len + hs;
}

std::string DataCell::serialize() const {
  unsigned char buff[max_serialized_bytes];
  int len = serialize(buff, sizeof(buff));
  return std::string(buff, buff + len);
}

std::string DataCell::to_hex() const {
  unsigned char buff[max_serialized_bytes];
  int len = serialize(buff, sizeof(buff));
  char hex_buff[max_serialized_bytes * 2 + 1];
  for (int i = 0; i < len; i++) {
    snprintf(hex_buff + 2 * i, sizeof(hex_buff) - 2 * i, "%02x", buff[i]);
  }
  return hex_buff;
}

DataCell::DataCell(int bit_length, size_t refs_cnt, Cell::SpecialType type, LevelMask level_mask,
                   bool allocated_in_arena, td::uint8 virtualization)
    : bit_length_(bit_length)
    , refs_cnt_(static_cast<td::uint8>(refs_cnt))
    , type_(static_cast<td::uint8>(type))
    , level_(static_cast<td::uint8>(level_mask.get_level()))
    , level_mask_(level_mask.get_mask())
    , allocated_in_arena_(allocated_in_arena)
    , virtualization_(virtualization) {
  get_thread_safe_counter().add(1);
}

}  // namespace vm
