/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once
#include "common/refcnt.hpp"
#include "vm/dict.h"
#include "ton/ton-types.h"
#include "ton/ton-shard.h"
#include "common/bitstring.h"
#include "block.h"
#include "vm/db/CellHashTable.h"

namespace block {
using td::Ref;

class AccountStorageStat {
 public:
  AccountStorageStat();
  AccountStorageStat(Ref<vm::Cell> dict_root, std::vector<Ref<vm::Cell>> roots, td::uint64 total_cells,
                     td::uint64 total_bits);
  explicit AccountStorageStat(const AccountStorageStat *parent);
  AccountStorageStat(const AccountStorageStat &other) = delete;
  AccountStorageStat(AccountStorageStat &&other) = default;
  ~AccountStorageStat() = default;

  AccountStorageStat &operator=(const AccountStorageStat &other) = delete;
  AccountStorageStat &operator=(AccountStorageStat &&other) = default;

  td::Status replace_roots(std::vector<Ref<vm::Cell>> new_roots, bool check_merkle_depth = false);
  void add_hint(const td::HashSet<vm::CellHash> &visited);

  td::uint64 get_total_cells() const {
    return total_cells_;
  }

  td::uint64 get_total_bits() const {
    return total_bits_;
  }

  td::Result<Ref<vm::Cell>> get_dict_root();

  td::Result<td::Bits256> get_dict_hash() {
    TRY_RESULT(root, get_dict_root());
    return root.is_null() ? td::Bits256::zero() : td::Bits256{root->get_hash().bits()};
  }

  void apply_child_stat(AccountStorageStat &&child);

 private:
  vm::Dictionary dict_;
  bool dict_up_to_date_ = true;
  td::uint64 total_cells_, total_bits_;
  std::vector<Ref<vm::Cell>> roots_;
  const AccountStorageStat *parent_ = nullptr;

  struct CellInfo {
    td::uint32 max_merkle_depth = 0;
  };

  td::Result<CellInfo> add_cell(const Ref<vm::Cell> &cell);
  td::Status remove_cell(const Ref<vm::Cell> &cell);

  struct Entry {
    bool inited = false;
    vm::CellHash hash;
    td::optional<unsigned> size_bits;
    bool exists_known = false;
    bool exists = false;
    td::optional<td::uint32> refcnt, max_merkle_depth;
    td::int32 refcnt_diff = 0;
    td::int32 dict_refcnt_diff = 0;
  };

  td::HashMap<vm::CellHash, Entry, std::hash<vm::CellHash>> cache_;

  Entry &get_entry(const Ref<vm::Cell> &cell);
  td::Status fetch_from_dict(Entry &e);
  td::Status finalize_entry(Entry &e);

  static constexpr td::uint32 MERKLE_DEPTH_LIMIT = 3;
  static constexpr td::uint32 MAX_MERKLE_DEPTH = 2;
};

class StorageStatCalculationContext : public td::Context<StorageStatCalculationContext> {
 public:
  explicit StorageStatCalculationContext(bool active) : active_(active) {
  }
  bool calculating_storage_stat() const {
    return active_;
  }

 private:
  bool active_ = false;
};

}  // namespace block
