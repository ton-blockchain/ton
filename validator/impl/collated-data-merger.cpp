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
#include "vm/cells/ExtCell.h"

#include "collated-data-merger.h"

namespace ton::validator {

class CollatedDataMergerExtCellLoader {
 public:
  static td::Result<Ref<vm::DataCell>> load_data_cell(const vm::Cell &cell, const td::Unit &extra) {
    throw vm::VmVirtError{};
  }
};

using CollatedDataMergerExtCell = vm::ExtCell<td::Unit, CollatedDataMergerExtCellLoader>;

void CollatedDataMerger::get_cells(std::vector<vm::CellHash> hashes,
                                   td::Promise<td::HashMap<vm::CellHash, Ref<vm::Cell>>> promise) {
  td::HashMap<vm::CellHash, Ref<vm::Cell>> result;
  for (const vm::CellHash &hash : hashes) {
    auto it = cells_.find(hash);
    if (it != cells_.end() && it->second.cell.not_null()) {
      result[hash] = it->second.cell;
    }
  }
  promise.set_value(std::move(result));
}

void CollatedDataMerger::add_cells(Ref<vm::Cell> cell) {
  auto &info = cells_[cell->get_hash()];
  if (info.visited) {
    return;
  }
  info.visited = true;
  auto loaded_cell = cell->load_cell().move_as_ok();
  CHECK(loaded_cell.virt.get_virtualization() == 0);
  auto data_cell = std::move(loaded_cell.data_cell);
  info.set_cell(data_cell);

  for (unsigned i = 0; i < data_cell->size_refs(); ++i) {
    add_cells(data_cell->get_ref(i));
  }
  bool is_prunned_branch = data_cell->special_type() == vm::CellTraits::SpecialType::PrunnedBranch;
  for (unsigned level = 0; level < cell->get_level(); ++level) {
    vm::CellHash hash = cell->get_hash(level);
    auto &info2 = cells_[hash];
    if (info2.cell.not_null() && (is_prunned_branch || !info2.prunned)) {
      continue;
    }
    if (is_prunned_branch) {
      vm::detail::LevelMask level_mask = data_cell->get_level_mask().apply(level);
      td::uint8 hashes[(1 + vm::Cell::max_level) * 32];
      td::uint8 depths[(1 + vm::Cell::max_level) * 2];
      size_t n = 0;
      for (td::uint32 i = 0; i <= level; ++i) {
        if (level_mask.is_significant(i)) {
          memcpy(hashes + n * 32, data_cell->get_hash(i).as_slice().data(), 32);
          vm::DataCell::store_depth(depths + n * 2, data_cell->get_depth(i));
          ++n;
        }
      }
      info2.cell = CollatedDataMergerExtCell::create(vm::PrunnedCellInfo{.level_mask = level_mask,
                                                                         .hash = td::Slice(hashes, n * 32),
                                                                         .depth = td::Slice(depths, n * 2)},
                                                     td::Unit{})
                       .move_as_ok();
      info2.prunned = true;
      CHECK(info2.cell->get_hash() == hash)
    } else {
      vm::CellBuilder cb;
      cb.store_bits(data_cell->get_data(), data_cell->size());
      unsigned child_level = level + (data_cell->special_type() == vm::CellTraits::SpecialType::MerkleProof ||
                                              data_cell->special_type() == vm::CellTraits::SpecialType::MerkleUpdate
                                          ? 1
                                          : 0);
      for (unsigned i = 0; i < data_cell->size_refs(); ++i) {
        Ref<vm::Cell> child = data_cell->get_ref(i);
        vm::CellHash child_hash = child->get_hash(child_level);
        auto it = cells_.find(child_hash);
        CHECK(it != cells_.end());
        cb.store_ref(it->second.cell);
      }
      info2.set_cell(cb.finalize(data_cell->is_special()));
      CHECK(info2.cell->get_hash() == hash)
    }
  }
}

void CollatedDataMerger::add_block_candidate(BlockIdExt block_id, Ref<vm::Cell> root,
                                             std::vector<Ref<vm::Cell>> collated_roots,
                                             td::Promise<td::RealCpuTimer::Time> promise) {
  td::RealCpuTimer timer;
  SCOPE_EXIT {
    promise.set_value(timer.elapsed_both());
  };
  if (!blocks_.insert(block_id).second) {
    return;
  }
  add_cells(std::move(root));
  for (auto &c : collated_roots) {
    add_cells(std::move(c));
  }
  LOG(INFO) << "Added block " << block_id.to_str() << " in " << timer.elapsed_real()
            << " s, total cells = " << cells_.size();
}

void CollatedDataMerger::add_block_candidate_data(BlockIdExt block_id, td::BufferSlice data,
                                                  td::BufferSlice collated_data,
                                                  td::Promise<td::RealCpuTimer::Time> promise) {
  td::RealCpuTimer timer;
  SCOPE_EXIT {
    promise.set_value(timer.elapsed_both());
  };
  if (!blocks_.insert(block_id).second) {
    return;
  }
  auto r_root = vm::std_boc_deserialize(data);
  if (r_root.is_error()) {
    LOG(WARNING) << "Failed to deserialize block data for " << block_id.to_str() << " : " << r_root.error();
    return;
  }
  auto r_collated_roots = vm::std_boc_deserialize_multi(collated_data, max_collated_data_roots, true);
  if (r_collated_roots.is_error()) {
    LOG(WARNING) << "Failed to deserialize collated data for " << block_id.to_str() << " : "
                 << r_collated_roots.error();
    return;
  }
  add_cells(r_root.move_as_ok());
  for (auto &c : r_collated_roots.move_as_ok()) {
    add_cells(std::move(c));
  }
  LOG(INFO) << "Added block " << block_id.to_str() << " in " << timer.elapsed_real() << " s";
}

void CollatedDataMerger::CellInfo::set_cell(const Ref<vm::DataCell> &new_cell) {
  if (cell.is_null()) {
    cell = new_cell;
    prunned = false;
    return;
  }
  if (prunned) {
    auto ext_cell = dynamic_cast<const CollatedDataMergerExtCell *>(cell.get());
    CHECK(ext_cell != nullptr);
    ext_cell->set_inner_cell(new_cell).ensure();
    prunned = false;
  }
}

td::Status CollatedDataDeduplicator::add_block_candidate(BlockSeqno seqno, td::Slice block_data,
                                                         td::Slice collated_data) {
  td::Timer timer;
  TRY_RESULT(root, vm::std_boc_deserialize(block_data));
  TRY_RESULT(collated_roots, vm::std_boc_deserialize_multi(collated_data, max_collated_data_roots, true));
  std::lock_guard lock{mutex_};
  td::HashSet<vm::CellHash> visited;
  std::function<void(const Ref<vm::Cell> &)> dfs = [&](const Ref<vm::Cell> &cell) {
    if (!visited.insert(cell->get_hash()).second) {
      return;
    }
    vm::CellSlice cs{vm::NoVm{}, cell};
    for (unsigned i = cs.special_type() == vm::CellTraits::SpecialType::PrunnedBranch ? cell->get_level() : 0;
         i <= cell->get_level(); ++i) {
      auto [it, inserted] = cells_.emplace(cell->get_hash(i), seqno);
      if (!inserted) {
        it->second = std::min(it->second, seqno);
      }
    }
    for (unsigned i = 0; i < cs.size_refs(); ++i) {
      dfs(cs.prefetch_ref(i));
    }
  };
  dfs(root);
  for (const Ref<vm::Cell> &c : collated_roots) {
    dfs(c);
  }
  LOG(INFO) << "Added block " << seqno << " in " << timer.elapsed() << " s, total cells = " << cells_.size();
  return td::Status::OK();
}

bool CollatedDataDeduplicator::cell_exists(const vm::CellHash &hash, BlockSeqno seqno) {
  std::lock_guard lock{mutex_};
  auto it = cells_.find(hash);
  return it != cells_.end() && it->second < seqno;
}

}  // namespace ton::validator
