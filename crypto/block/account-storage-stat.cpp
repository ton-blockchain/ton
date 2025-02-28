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

td::Result<AccountStorageStat::CellInfo> AccountStorageStat::add_root(const Ref<vm::Cell>& cell) {
  roots_.push_back(cell);
  return add_cell(cell);
}

td::Status AccountStorageStat::remove_root(const Ref<vm::Cell>& cell) {
  auto it = std::find_if(roots_.begin(), roots_.end(),
                         [&](const Ref<vm::Cell>& c) { return c->get_hash() == cell->get_hash(); });
  if (it == roots_.end()) {
    return td::Status::Error(PSTRING() << "no such root " << cell->get_hash().to_hex());
  }
  roots_.erase(it);
  return remove_cell(cell);
}

td::Result<AccountStorageStat::CellInfo> AccountStorageStat::replace_roots(std::vector<Ref<vm::Cell>> new_roots) {
  std::vector<Ref<vm::Cell>> old_roots = roots_;
  td::uint32 max_merkle_depth = 0;
  for (const Ref<vm::Cell>& root : new_roots) {
    if (root.is_null()) {
      continue;
    }
    TRY_RESULT(info, add_root(root));
    max_merkle_depth = std::max(max_merkle_depth, info.max_merkle_depth);
  }
  for (const Ref<vm::Cell>& root : old_roots) {
    TRY_STATUS(remove_root(root));
  }
  return CellInfo{max_merkle_depth};
}

td::Result<AccountStorageStat::CellInfo> AccountStorageStat::add_cell(const Ref<vm::Cell>& cell) {
  Entry& e = get_entry(cell);
  ++e.refcnt;
  if (e.refcnt == 0) {
    return td::Status::Error(PSTRING() << "cell " << cell->get_hash().to_hex() << ": refcnt overflow");
  }
  if (e.refcnt != 1) {
    update_dict(e);
    return CellInfo{e.max_merkle_depth};
  }
  td::uint32 max_merkle_depth = 0;
  vm::CellSlice cs{vm::NoVm{}, cell};
  ++total_cells_;
  total_bits_ += cs.size();
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
  update_dict(e2);
  return CellInfo{max_merkle_depth};
}

td::Status AccountStorageStat::remove_cell(const Ref<vm::Cell>& cell) {
  Entry& e = get_entry(cell);
  if (e.refcnt == 0) {
    return td::Status::Error(PSTRING() << "cell " << cell->get_hash().to_hex() << " is not in the dict");
  }
  --e.refcnt;
  update_dict(e);
  if (e.refcnt != 0) {
    return td::Status::OK();
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  if (total_cells_ == 0 || total_bits_ < cs.size()) {
    return td::Status::Error("total_cell/total_bits becomes negative");
  }
  --total_cells_;
  total_bits_ -= cs.size();
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_STATUS(remove_cell(cs.prefetch_ref(i)));
  }
  return td::Status::OK();
}

bool AccountStorageStat::Entry::serialize(vm::CellBuilder& cb) const {
  return cb.store_long_bool(refcnt, 32) && cb.store_long_bool(max_merkle_depth, 2);
}

void AccountStorageStat::Entry::fetch(Ref<vm::CellSlice> cs) {
  if (cs.is_null()) {
    refcnt = max_merkle_depth = 0;
  } else {
    refcnt = (td::uint32)cs.write().fetch_ulong(32);
    max_merkle_depth = (td::uint32)cs.write().fetch_ulong(2);
  }
}

AccountStorageStat::Entry& AccountStorageStat::get_entry(const Ref<vm::Cell>& cell) {
  return cache_.apply(cell->get_hash().as_slice(), [&](Entry& e) {
    if (e.inited) {
      return;
    }
    e.inited = true;
    e.hash = cell->get_hash();
    e.fetch(dict_.lookup(e.hash.as_bitslice()));
  });
}

void AccountStorageStat::update_dict(const Entry& e) {
  if (e.refcnt == 0) {
    dict_.lookup_delete(e.hash.as_bitslice());
  } else {
    vm::CellBuilder cb;
    CHECK(e.serialize(cb));
    dict_.set_builder(e.hash.as_bitslice(), cb);
  }
}

}  // namespace block