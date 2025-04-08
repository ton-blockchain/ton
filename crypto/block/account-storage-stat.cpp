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
    : dict_(std::move(dict_root), 256), total_cells_(total_cells), total_bits_(total_bits), roots_(std::move(roots)) {
}

AccountStorageStat::AccountStorageStat(const AccountStorageStat* parent)
    : dict_(parent->dict_)
    , dict_up_to_date_(parent->dict_up_to_date_)
    , total_cells_(parent->total_cells_)
    , total_bits_(parent->total_bits_)
    , roots_(parent->roots_)
    , parent_(parent) {
  CHECK(parent_->parent_ == nullptr);
}

td::Status AccountStorageStat::replace_roots(std::vector<Ref<vm::Cell>> new_roots, bool check_merkle_depth) {
  std::erase_if(new_roots, [](const Ref<vm::Cell>& c) { return c.is_null(); });
  if (new_roots.empty()) {
    roots_.clear();
    total_bits_ = total_cells_ = 0;
    dict_ = vm::Dictionary{256};
    cache_ = {};
    dict_up_to_date_ = true;
    parent_ = nullptr;
    return td::Status::OK();
  }

  auto cmp = [](const Ref<vm::Cell>& c1, const Ref<vm::Cell>& c2) { return c1->get_hash() < c2->get_hash(); };
  std::sort(new_roots.begin(), new_roots.end(), cmp);
  std::sort(roots_.begin(), roots_.end(), cmp);
  std::vector<Ref<vm::Cell>> to_add, to_del;
  std::set_difference(new_roots.begin(), new_roots.end(), roots_.begin(), roots_.end(), std::back_inserter(to_add),
                      cmp);
  std::set_difference(roots_.begin(), roots_.end(), new_roots.begin(), new_roots.end(), std::back_inserter(to_del),
                      cmp);
  if (to_add.empty() && to_del.empty()) {
    return td::Status::OK();
  }

  for (const Ref<vm::Cell>& root : to_add) {
    TRY_RESULT(info, add_cell(root));
    if (check_merkle_depth && info.max_merkle_depth > MAX_MERKLE_DEPTH) {
      return td::Status::Error("too big Merkle depth");
    }
  }
  for (const Ref<vm::Cell>& root : to_del) {
    TRY_STATUS(remove_cell(root));
  }

  roots_ = std::move(new_roots);
  dict_up_to_date_ = false;
  for (auto& [_, e] : cache_) {
    TRY_STATUS(finalize_entry(e));
  }
  return td::Status::OK();
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
      fetch_from_dict(e).ignore();
      if (e.max_merkle_depth && e.max_merkle_depth.value() != 0) {
        return;
      }
    }
    if (hint.contains(cell->get_hash())) {
      bool spec;
      vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
      e.size_bits = cs.size();
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
    TRY_STATUS(fetch_from_dict(e));
  }
  ++e.refcnt_diff;
  if (e.exists || e.refcnt_diff > 1 || (e.refcnt && e.refcnt.value() + e.refcnt_diff != 1)) {
    if (!e.max_merkle_depth) {
      TRY_STATUS(fetch_from_dict(e));
      if (!e.max_merkle_depth) {
        return td::Status::Error(PSTRING() << "unexpected unknown Merkle depth of cell " << cell->get_hash());
      }
    }
    return CellInfo{e.max_merkle_depth.value()};
  }

  td::uint32 max_merkle_depth = 0;
  bool spec;
  vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
  e.size_bits = cs.size();
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
    TRY_STATUS(fetch_from_dict(e));
  }
  if (!e.exists) {
    return td::Status::Error(PSTRING() << "Failed to remove cell " << cell->get_hash().to_hex()
                                       << " : does not exist in the dict");
  }
  --e.refcnt_diff;
  if (e.refcnt_diff < 0 && !e.refcnt) {
    TRY_STATUS(fetch_from_dict(e));
  }
  if (e.refcnt.value() + e.refcnt_diff != 0) {
    return td::Status::OK();
  }
  bool spec;
  vm::CellSlice cs = vm::load_cell_slice_special(cell, spec);
  e.size_bits = cs.size();
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_STATUS(remove_cell(cs.prefetch_ref(i)));
  }
  return td::Status::OK();
}

td::Result<Ref<vm::Cell>> AccountStorageStat::get_dict_root() {
  if (!dict_up_to_date_) {
    std::vector<std::pair<td::ConstBitPtr, Ref<vm::CellBuilder>>> values;
    for (auto& [_, e] : cache_) {
      if (e.dict_refcnt_diff == 0) {
        continue;
      }
      if (!e.exists_known || !e.refcnt || (e.exists && !e.max_merkle_depth)) {
        return td::Status::Error("unexpected state of storage stat");
      }
      if (e.exists) {
        Ref<vm::CellBuilder> cbr{true};
        auto& cb = cbr.write();
        CHECK(cb.store_long_bool(e.refcnt.value(), 32) && cb.store_long_bool(e.max_merkle_depth.value(), 2));
        values.emplace_back(e.hash.bits(), std::move(cbr));
      } else {
        values.emplace_back(e.hash.bits(), Ref<vm::CellBuilder>{});
      }
      e.dict_refcnt_diff = 0;
    }
    if (!dict_.multiset(values)) {
      return td::Status::Error("failed to update dictionary");
    }
    dict_up_to_date_ = true;
  }
  return dict_.get_root_cell();
}

void AccountStorageStat::apply_child_stat(AccountStorageStat&& child) {
  CHECK(parent_ == nullptr);
  if (child.parent_ == nullptr) {
    *this = std::move(child);
    return;
  }
  CHECK(child.parent_ == this);
  total_bits_ = child.total_bits_;
  total_cells_ = child.total_cells_;
  dict_ = std::move(child.dict_);
  dict_up_to_date_ = child.dict_up_to_date_;
  roots_ = std::move(child.roots_);
  for (auto& [hash, e] : child.cache_) {
    cache_[hash] = std::move(e);
  }
}

AccountStorageStat::Entry& AccountStorageStat::get_entry(const Ref<vm::Cell>& cell) {
  Entry& e = cache_[cell->get_hash()];
  if (e.inited) {
    return e;
  }
  if (parent_) {
    auto it = parent_->cache_.find(cell->get_hash());
    if (it != parent_->cache_.end()) {
      CHECK(it->second.inited);
      e = it->second;
      return e;
    }
  }
  e.inited = true;
  e.hash = cell->get_hash();
  return e;
}

td::Status AccountStorageStat::fetch_from_dict(Entry& e) {
  if (e.exists_known && e.refcnt && (!e.exists || e.max_merkle_depth)) {
    return td::Status::OK();
  }
  auto cs = dict_.lookup(e.hash.as_bitslice());
  if (cs.is_null()) {
    e.exists = false;
    e.refcnt = 0;
  } else {
    if (cs->size_ext() != 32 + 2) {
      return td::Status::Error(PSTRING() << "invalid record for cell " << e.hash.to_hex());
    }
    e.exists = true;
    e.refcnt = (td::uint32)cs.write().fetch_ulong(32);
    e.max_merkle_depth = (td::uint32)cs.write().fetch_ulong(2);
    if (e.refcnt.value() == 0) {
      return td::Status::Error(PSTRING() << "invalid refcnt=0 for cell " << e.hash.to_hex());
    }
  }
  e.exists_known = true;
  return td::Status::OK();
}

td::Status AccountStorageStat::finalize_entry(Entry& e) {
  if (e.refcnt_diff == 0) {
    return td::Status::OK();
  }
  TRY_STATUS(fetch_from_dict(e));
  e.refcnt.value() += e.refcnt_diff;
  e.dict_refcnt_diff += e.refcnt_diff;
  e.refcnt_diff = 0;
  if (e.refcnt.value() == 0) {
    if (!e.size_bits) {
      return td::Status::Error(PSTRING() << "Failed to store entry " << e.hash.to_hex() << " : unknown cell bits");
    }
    --total_cells_;
    total_bits_ -= e.size_bits.value();
    e.exists = false;
  } else {
    if (!e.exists) {
      if (!e.size_bits) {
        return td::Status::Error(PSTRING() << "Failed to store entry " << e.hash.to_hex() << " : unknown cell bits");
      }
      ++total_cells_;
      total_bits_ += e.size_bits.value();
    }
    e.exists = true;
    if (!e.max_merkle_depth) {
      return td::Status::Error(PSTRING() << "Failed to store entry " << e.hash.to_hex() << " : unknown merkle depth");
    }
  }
  return td::Status::OK();
}

}  // namespace block