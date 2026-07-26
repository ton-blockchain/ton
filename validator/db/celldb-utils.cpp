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
*/
#include "block/block-auto.h"
#include "impl/accept-block.hpp"
#include "td/actor/MultiPromise.h"
#include "td/utils/HashMap.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/db/CellStorage.h"

#include "celldb-utils.h"
#include "fabric.h"

namespace ton::validator {

void calculate_permanent_celldb_update(const std::map<BlockIdExt, td::Ref<BlockData>>& blocks,
                                       std::shared_ptr<vm::DynamicBagOfCellsDb::AsyncExecutor> executor,
                                       td::Promise<std::vector<PermanentCellDbUpdate>> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  auto updates = std::make_shared<std::vector<PermanentCellDbUpdate>>();
  updates->reserve(blocks.size());
  for (auto& [_, block] : blocks) {
    executor->execute_async([block = block, updates, executor,
                             promise = std::make_shared<td::Promise<td::Unit>>(ig.get_promise())]() mutable {
      block::gen::Block::Record rec;
      if (!block::gen::unpack_cell(block->root_cell(), rec)) {
        promise->set_error(td::Status::Error("cannot unpack Block record"));
        return;
      }
      TRY_STATUS_PROMISE(*promise, vm::MerkleUpdate::validate(rec.state_update));
      bool spec;
      vm::CellSlice update_cs = vm::load_cell_slice_special(rec.state_update, spec);
      td::Ref<vm::Cell> new_state_root = update_cs.prefetch_ref(1);
      td::HashMap<vm::CellHash, int> visited;
      PermanentCellDbUpdate update{
          .block_id = block->block_id(),
          .state_root_hash = new_state_root->get_hash(0).bits(),
          .to_store = {},
      };
      std::function<void(const td::Ref<vm::Cell>&, int)> dfs = [&](const td::Ref<vm::Cell>& cell, int merkle_depth) {
        int& vis = visited[cell->get_hash()];
        if (vis & (1 << merkle_depth)) {
          return;
        }
        vis |= (1 << merkle_depth);
        vm::CellSlice cs{vm::NoVm(), cell};
        if (cs.special_type() == vm::CellTraits::SpecialType::PrunnedBranch &&
            cell->get_level() == static_cast<td::uint32>(merkle_depth + 1)) {
          return;
        }
        update.to_store.emplace_back(
            cell->get_hash(merkle_depth),
            vm::CellStorer::serialize_value(1 << 29, cell->load_cell().move_as_ok().data_cell, false, merkle_depth));
        merkle_depth = cs.child_merkle_depth(merkle_depth);
        for (unsigned i = 0; i < cs.size_refs(); ++i) {
          dfs(cs.prefetch_ref(i), merkle_depth);
        }
      };
      dfs(new_state_root, 0);
      executor->execute_sync(
          [update = std::move(update), updates = std::move(updates), promise = std::move(promise)]() mutable {
            updates->push_back(std::move(update));
            promise->set_result(td::Unit());
          });
    });
  }

  ig.add_promise([updates, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    TRY_STATUS_PROMISE(promise, R.move_as_status());
    promise.set_value(std::move(*updates));
  });
}

td::Result<RootHash> unpack_block_state_root_hash(Ref<BlockData> block) {
  block::gen::Block::Record rec;
  if (!block::gen::unpack_cell(block->root_cell(), rec)) {
    return td::Status::Error("cannot unpack Block record");
  }
  bool spec;
  vm::CellSlice update_cs = vm::load_cell_slice_special(rec.state_update, spec);
  if (update_cs.special_type() != vm::CellTraits::SpecialType::MerkleUpdate) {
    return td::Status::Error("invalid Merkle update in block");
  }
  Ref<vm::Cell> new_state_root = update_cs.prefetch_ref(1);
  return new_state_root->get_hash(0).bits();
}

td::Result<Ref<vm::Cell>> apply_block_to_prev_states(Ref<BlockData> block, std::vector<Ref<vm::Cell>> prev_roots,
                                                     vm::StoreCellHint* hint) {
  TD_PERF_COUNTER(apply_block_to_state);
  td::PerfWarningTimer t{"applyblocktostate", 0.1};
  Ref<vm::Cell> prev_root;
  if (prev_roots.size() == 1) {
    prev_root = prev_roots[0];
  } else {
    CHECK(prev_roots.size() == 2);
    CHECK(block::gen::t_ShardState.cell_pack_split_state(prev_root, prev_roots[0], prev_roots[1]));
  }
  block::gen::Block::Record rec;
  if (!block::gen::unpack_cell(block->root_cell(), rec)) {
    return td::Status::Error("failed to unpack block");
  }
  return vm::MerkleUpdate::apply(prev_root, rec.state_update, hint);
}

td::Result<td::Ref<vm::Cell>> build_next_state(Ref<BlockData> block, vm::CellDbReader& cell_db_reader,
                                               std::vector<Ref<vm::Cell>> prev_roots, vm::StoreCellHint* hint) {
  TD_PERF_COUNTER(apply_block_to_state_fast);
  td::PerfWarningTimer t{"applyblocktostatefast", 0.1};
  block::gen::Block::Record rec;
  if (!block::gen::unpack_cell(block->root_cell(), rec)) {
    return td::Status::Error("failed to unpack Block");
  }
  TRY_STATUS(vm::MerkleUpdate::validate(rec.state_update));
  vm::CellSlice merkle_update{vm::NoVm{}, rec.state_update};

  Ref<vm::Cell> prev_root;
  if (prev_roots.size() == 1) {
    prev_root = prev_roots[0];
  } else {
    CHECK(prev_roots.size() == 2);
    if (!block::gen::t_ShardState.cell_pack_split_state(prev_root, prev_roots[0], prev_roots[1])) {
      return td::Status::Error("failed to pack split state root");
    }
  }
  if (prev_root->get_hash() != merkle_update.prefetch_ref(0)->get_hash(0)) {
    return td::Status::Error("prev state root hash mismatch");
  }
  if (prev_root->get_depth() != merkle_update.prefetch_ref(0)->get_depth(0)) {
    return td::Status::Error("prev state root depth mismatch");
  }

  Ref<vm::Cell> proof_root = merkle_update.prefetch_ref(1);

  class BuildNextState {
   public:
    BuildNextState(vm::CellDbReader& cell_db_reader, vm::StoreCellHint* hint)
        : cell_db_reader_(cell_db_reader), hint_(hint) {
    }

    td::Result<Ref<vm::Cell>> dfs(Ref<vm::Cell> cell, unsigned merkle_depth) {
      vm::CellSlice cs(vm::NoVm(), cell);
      if (cs.special_type() == vm::Cell::SpecialType::PrunnedBranch) {
        if (cell->get_level() == merkle_depth + 1) {
          vm::CellHash hash = cell->get_hash(merkle_depth);
          if (hint_ != nullptr) {
            hint_->prev_state_cells.insert(hash);
          }
          auto result = cell_db_reader_.create_unloaded_cell(cell, merkle_depth);
          if (result->get_hash() != hash) {
            return td::Status::Error("prunned branch hash mismatch");
          }
          return result;
        }
        return cell;
      }
      Key key{cell->get_hash(), merkle_depth};
      if (auto it = ready_cells_.find(key); it != ready_cells_.end()) {
        return it->second;
      }

      int child_merkle_depth = cs.child_merkle_depth(merkle_depth);
      vm::CellBuilder cb;
      cb.store_bits(cs.fetch_bits(cs.size()));
      for (unsigned i = 0; i < cs.size_refs(); i++) {
        TRY_RESULT(ref, dfs(cs.prefetch_ref(i), child_merkle_depth));
        cb.store_ref(std::move(ref));
      }
      auto hash_hint = [&](unsigned level, const vm::Cell::LevelMask&, vm::CellHash& hash) {
        hash = cell->get_hash(std::min(level, merkle_depth));
        return true;
      };
      auto res = cb.finalize_novm(cs.is_special(), std::move(hash_hint));
      ready_cells_.emplace(key, res);
      return res;
    }

   private:
    vm::CellDbReader& cell_db_reader_;
    vm::StoreCellHint* hint_;

    using Key = std::pair<vm::CellHash, unsigned>;
    td::HashMap<Key, Ref<vm::Cell>> ready_cells_;
  };

  TRY_RESULT(result, BuildNextState(cell_db_reader, hint).dfs(proof_root, 0));
  if (result->get_hash() != proof_root->get_hash(0)) {
    return td::Status::Error("new state hash mismatch");
  }
  return result;
}

}  // namespace ton::validator
