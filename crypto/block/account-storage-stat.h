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
  struct CellInfo {
    td::uint32 max_merkle_depth = 0;
  };

  AccountStorageStat();
  AccountStorageStat(Ref<vm::Cell> dict_root, std::vector<Ref<vm::Cell>> roots, td::uint64 total_cells,
                     td::uint64 total_bits);
  AccountStorageStat(const AccountStorageStat &other);
  AccountStorageStat(AccountStorageStat &&other);
  ~AccountStorageStat() = default;

  AccountStorageStat &operator=(const AccountStorageStat &other);
  AccountStorageStat &operator=(AccountStorageStat &&other);

  td::uint64 get_total_cells() const {
    return total_cells_;
  }

  td::uint64 get_total_bits() const {
    return total_bits_;
  }

  Ref<vm::Cell> get_dict_root() const {
    return dict_.get_root_cell();
  }

  td::Bits256 get_dict_hash() const {
    return dict_.is_empty() ? td::Bits256::zero() : td::Bits256{dict_.get_root_cell()->get_hash().bits()};
  }

  td::Result<CellInfo> replace_roots(std::vector<Ref<vm::Cell>> hint);
  void add_hint(const td::HashSet<vm::CellHash> &visited);

 private:
  vm::Dictionary dict_;
  td::uint64 total_cells_, total_bits_;
  std::vector<Ref<vm::Cell>> roots_;

  td::Result<CellInfo> add_cell(const Ref<vm::Cell> &cell);
  td::Status remove_cell(const Ref<vm::Cell> &cell);

  struct Entry {
    bool inited = false;
    Ref<vm::Cell> cell;
    bool exists_known = false;
    bool exists = false;
    td::optional<td::uint32> refcnt, max_merkle_depth;
    td::int32 refcnt_diff = 0;

    vm::Cell::Hash key() const {
      return cell->get_hash();
    }
    bool operator<(const Entry &other) const {
      return key() < other.key();
    }
    struct Eq {
      using is_transparent = void;  // Pred to use
      bool operator()(const Entry &info, const Entry &other_info) const {
        return info.key() == other_info.key();
      }
      bool operator()(const Entry &info, td::Slice hash) const {
        return info.key().as_slice() == hash;
      }
      bool operator()(td::Slice hash, const Entry &info) const {
        return info.key().as_slice() == hash;
      }
    };
    struct Hash {
      using is_transparent = void;  // Pred to use
      using transparent_key_equal = Eq;
      size_t operator()(td::Slice hash) const {
        return cell_hash_slice_hash(hash);
      }
      size_t operator()(const Entry &info) const {
        return cell_hash_slice_hash(info.key().as_slice());
      }
    };
  };
  vm::CellHashTable<Entry> cache_;

  Entry &get_entry(const Ref<vm::Cell> &cell);
  td::Status fetch_entry(Entry &e);
  td::Status commit_entry(Entry &e);

  static constexpr td::uint32 MERKLE_DEPTH_LIMIT = 3;
};

}  // namespace block
