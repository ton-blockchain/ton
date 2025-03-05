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
#include "account-storage-stat.h"

namespace block {

AccountStorageStat::AccountStorageStat() : AccountStorageStat({}, {}, 0, 0) {
}

AccountStorageStat::AccountStorageStat(Ref<vm::Cell> dict_root, std::vector<Ref<vm::Cell>> roots,
                                       td::uint64 total_cells, td::uint64 total_bits)
    : dict_(std::move(dict_root), 256), roots_(std::move(roots)), total_cells_(total_cells), total_bits_(total_bits) {
}

AccountStorageStat::AccountStorageStat(const AccountStorageStat& other)
    : AccountStorageStat(other.dict_.get_root_cell(), other.roots_, other.total_cells_, other.total_bits_) {
}

AccountStorageStat::AccountStorageStat(AccountStorageStat&& other)
    : AccountStorageStat(other.dict_.get_root_cell(), std::move(other.roots_), other.total_cells_, other.total_bits_) {
  cache_ = std::move(other.cache_);
}

AccountStorageStat& AccountStorageStat::operator=(const AccountStorageStat& other) {
  dict_ = other.dict_;
  total_cells_ = other.total_cells_;
  total_bits_ = other.total_bits_;
  roots_ = other.roots_;
  cache_ = {};
  return *this;
}

AccountStorageStat& AccountStorageStat::operator=(AccountStorageStat&& other) {
  dict_ = std::move(other.dict_);
  total_cells_ = other.total_cells_;
  total_bits_ = other.total_bits_;
  roots_ = std::move(other.roots_);
  cache_ = std::move(other.cache_);
  return *this;
}

td::Result<AccountStorageStat::CellInfo> AccountStorageStat::replace_roots(std::vector<Ref<vm::Cell>> new_roots) {
  std::erase_if(new_roots, [](const Ref<vm::Cell>& c) { return c.is_null(); });
  auto cmp = [](const Ref<vm::Cell>& c1, const Ref<vm::Cell>& c2) { return c1->get_hash() < c2->get_hash(); };
  std::sort(new_roots.begin(), new_roots.end(), cmp);
  std::sort(roots_.begin(), roots_.end(), cmp);
  std::vector<Ref<vm::Cell>> to_add, to_del;
  std::set_difference(new_roots.begin(), new_roots.end(), roots_.begin(), roots_.end(), std::back_inserter(to_add),
                      cmp);
  std::set_difference(roots_.begin(), roots_.end(), new_roots.begin(), new_roots.end(), std::back_inserter(to_del),
                      cmp);

  td::uint32 max_merkle_depth = 0;
  for (const Ref<vm::Cell>& root : to_add) {
    TRY_RESULT(info, add_cell(root));
    max_merkle_depth = std::max(max_merkle_depth, info.max_merkle_depth);
  }
  for (const Ref<vm::Cell>& root : to_del) {
    TRY_STATUS(remove_cell(root));
  }

  roots_ = std::move(new_roots);
  td::Status S = td::Status::OK();
  cache_.for_each([&](Entry& e) {
    if (S.is_ok()) {
      S = commit_entry(e);
    }
  });
  TRY_STATUS(std::move(S));
  return CellInfo{max_merkle_depth};
}

void AccountStorageStat::add_hint(const td::HashSet<vm::CellHash>& hint) {
  td::HashSet<vm::CellHash> visited;
  std::function<void(const Ref<vm::Cell>&, bool)> dfs = [&](const Ref<vm::Cell>& cell, bool is_root) {
    if (!visited.insert(cell->get_hash()).second) {
      return;
    }
    Entry& e = get_entry(cell);
    e.exists = e.exists_known = true;
    if (is_root) {
      fetch_entry(e).ignore();
      if (e.max_merkle_depth && e.max_merkle_depth.value() != 0) {
        return;
      }
    }
    if (hint.contains(cell->get_hash())) {
      bool spec;
      vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
      for (unsigned i = 0; i < cs.size_refs(); ++i) {
        dfs(cs.prefetch_ref(i), false);
      }
    }
  };
  for (const Ref<vm::Cell>& root : roots_) {
    dfs(root, true);
  }
}

td::Result<AccountStorageStat::CellInfo> AccountStorageStat::add_cell(const Ref<vm::Cell>& cell) {
  Entry& e = get_entry(cell);
  if (!e.exists_known || e.refcnt_diff < 0) {
    TRY_STATUS(fetch_entry(e));
  }
  ++e.refcnt_diff;
  if (e.exists || e.refcnt_diff > 1 || (e.refcnt && e.refcnt.value() + e.refcnt_diff != 1)) {
    if (!e.max_merkle_depth) {
      TRY_STATUS(fetch_entry(e));
      if (!e.max_merkle_depth) {
        return td::Status::Error(PSTRING() << "unexpected unknown Merkle depth of cell " << cell->get_hash());
      }
    }
    return CellInfo{e.max_merkle_depth.value()};
  }

  td::uint32 max_merkle_depth = 0;
  bool spec;
  vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_RESULT(info, add_cell(cs.prefetch_ref(i)));
    max_merkle_depth = std::max(max_merkle_depth, info.max_merkle_depth);
  }
  if (cs.special_type() == vm::CellTraits::SpecialType::MerkleProof ||
      cs.special_type() == vm::CellTraits::SpecialType::MerkleUpdate) {
    ++max_merkle_depth;
  }
  max_merkle_depth = std::min(max_merkle_depth, MERKLE_DEPTH_LIMIT);
  Entry& e2 = get_entry(cell);
  e2.max_merkle_depth = max_merkle_depth;
  return CellInfo{max_merkle_depth};
}

td::Status AccountStorageStat::remove_cell(const Ref<vm::Cell>& cell) {
  Entry& e = get_entry(cell);
  if (!e.exists_known) {
    TRY_STATUS(fetch_entry(e));
  }
  if (!e.exists) {
    return td::Status::Error(PSTRING() << "Failed to remove cell " << cell->get_hash().to_hex()
                                       << " : does not exist in the dict");
  }
  --e.refcnt_diff;
  if (e.refcnt_diff < 0 && !e.refcnt) {
    TRY_STATUS(fetch_entry(e));
  }
  if (e.refcnt.value() + e.refcnt_diff != 0) {
    return td::Status::OK();
  }
  bool spec;
  vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_STATUS(remove_cell(cs.prefetch_ref(i)));
  }
  return td::Status::OK();
}

AccountStorageStat::Entry& AccountStorageStat::get_entry(const Ref<vm::Cell>& cell) {
  return cache_.apply(cell->get_hash().as_slice(), [&](Entry& e) {
    if (e.inited) {
      return;
    }
    e.inited = true;
    e.cell = cell;
  });
}

td::Status AccountStorageStat::fetch_entry(Entry& e) {
  if (e.exists_known && e.refcnt && (!e.exists || e.max_merkle_depth)) {
    return td::Status::OK();
  }
  auto cs = dict_.lookup(e.cell->get_hash().as_bitslice());
  if (cs.is_null()) {
    e.exists = false;
    e.refcnt = 0;
  } else {
    if (cs->size_ext() != 32 + 2) {
      return td::Status::Error(PSTRING() << "invalid record for cell " << e.cell->get_hash().to_hex());
    }
    e.exists = true;
    e.refcnt = (td::uint32)cs.write().fetch_ulong(32);
    e.max_merkle_depth = (td::uint32)cs.write().fetch_ulong(2);
    if (e.refcnt.value() == 0) {
      return td::Status::Error(PSTRING() << "invalid refcnt=0 for cell " << e.cell->get_hash().to_hex());
    }
  }
  e.exists_known = true;
  return td::Status::OK();
}

td::Status AccountStorageStat::commit_entry(Entry& e) {
  if (e.refcnt_diff == 0) {
    return td::Status::OK();
  }
  TRY_STATUS(fetch_entry(e));
  e.refcnt.value() += e.refcnt_diff;
  e.refcnt_diff = 0;
  bool spec;
  if (e.refcnt.value() == 0) {
    --total_cells_;
    total_bits_ -= vm::load_cell_slice_special(e.cell, spec).size();
    e.exists = false;
    if (dict_.lookup_delete(e.cell->get_hash().as_bitslice()).is_null()) {
      return td::Status::Error(PSTRING() << "Failed to delete entry " << e.cell->get_hash().to_hex());
    }
  } else {
    if (!e.exists) {
      ++total_cells_;
      total_bits_ += vm::load_cell_slice_special(e.cell, spec).size();
    }
    e.exists = true;
    if (!e.max_merkle_depth) {
      return td::Status::Error(PSTRING() << "Failed to store entry " << e.cell->get_hash().to_hex()
                                         << " : unknown merkle depth");
    }
    vm::CellBuilder cb;
    dict_.set_builder(e.cell->get_hash().as_bitslice(), cb);
    CHECK(cb.store_long_bool(e.refcnt.value(), 32) && cb.store_long_bool(e.max_merkle_depth.value(), 2));
    if (!dict_.set_builder(e.cell->get_hash().as_bitslice(), cb)) {
      return td::Status::Error(PSTRING() << "Failed to store entry " << e.cell->get_hash().to_hex());
    }
  }
  return td::Status::OK();
}

}  // namespace block