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
#include "td/actor/coro_utils.h"
#include "td/utils/ScopeGuard.h"
#include "vm/boc.h"

#include "collator-impl.h"

namespace ton::validator {

struct Collator::CollatedData {
  std::vector<Ref<vm::Cell>> roots;
  std::vector<Ref<vm::Cell>> state_roots;
  std::vector<Ref<vm::Cell>> storage_roots;
  Ref<vm::Cell> prev_state_root;
  bool after_merge = false;
  bool new_format = false;
  bool full = false;
  vm::ProofStorageStat reads;

  struct Result {
    td::BufferSlice data;
    td::RealCpuTimer::Time proofs_time;
    td::RealCpuTimer::Time serialization_time;
  };

  td::Status build();
  static td::actor::Task<Result> finalize(CollatedData data, bool parallel);
};

bool Collator::prepare_collated_data(CollatedData& data) {
  // 1. Shard descriptions and consensus metadata.
  if (!used_shard_block_descr_.empty()) {
    auto cell = collate_shard_block_descr_set();
    if (cell.not_null()) {
      data.roots.push_back(std::move(cell));
    }
  }
  // consensus_extra_data#638eb292 flags:# gen_utime_ms:uint64 = ConsensusExtraData;
  data.roots.push_back(
      vm::CellBuilder{}.store_long(0x638eb292, 32).store_long(0, 32).store_long(now_ms_, 64).finalize_novm());
  data.full = full_collated_data_;
  if (!data.full) {
    return true;
  }
  // 2. Proofs for hashes of states.
  for (const auto& p : block_state_proofs_) {
    data.roots.push_back(p.second);
  }
  data.new_format = global_version_ >= 16;
  // 3. Previous state. Finish all read tracking before taking the snapshot.
  if (!is_masterchain()) {
    if (!prepare_proofs()) {
      return fatal_error("cannot prepare proof for collated data");
    }
    if (data.new_format) {
      for (const auto& state : prev_states) {
        data.state_roots.push_back(state->root_cell());
      }
    } else {
      data.prev_state_root = prev_state_root_pure_;
      data.after_merge = after_merge_;
    }
    state_usage_tree_->set_ignore_loads(true);
  }
  // 4. Neighbor queues; predecessor states are already included above.
  for (auto& [block_id, mpb] : neighbor_proof_builders_) {
    if (prev_block_idx(block_id) == -1) {
      data.state_roots.push_back(mpb.original_root());
    }
  }
  // 5. Account storage dictionaries. Cached roots have already been cleaned
  // of UsageCell wrappers when published to StorageStatCache.
  for (auto& [_, dict] : account_storage_dicts_) {
    if (dict.add_to_collated_data) {
      data.storage_roots.push_back(dict.mpb.original_root());
    }
  }
  if (virtual_shards_) {
    collect_virtual_proofs(data.storage_roots);
  }
  data.reads = collated_data_stat;
  return true;
}

td::Status Collator::CollatedData::build() {
  if (!full) {
    return td::Status::OK();
  }
  auto is_prunned = [&](const Ref<vm::Cell>& cell) { return !reads.is_loaded(cell->get_hash()); };
  if (!new_format) {
    std::map<td::Bits256, Ref<vm::Cell>> proofs;
    if (prev_state_root.not_null()) {
      TRY_RESULT(proof, vm::MerkleProof::generate(prev_state_root, is_prunned));
      if (after_merge) {
        auto cs = vm::load_cell_slice_special(proof);
        TRY_BOOL(cs.special_type() == vm::CellTraits::SpecialType::MerkleProof);
        cs = vm::load_cell_slice(cs.prefetch_ref(0));
        TRY_BOOL(cs.size_refs() == 2 && cs.size() == 32 && cs.prefetch_ulong(32) == 0x5f327da5U);
        for (unsigned i = 0; i < 2; ++i) {
          proofs[cs.prefetch_ref(i)->get_hash(0).bits()] = vm::CellBuilder::create_merkle_proof(cs.prefetch_ref(i));
        }
      } else {
        proofs[prev_state_root->get_hash().bits()] = std::move(proof);
      }
    }
    for (const auto& root : state_roots) {
      TRY_RESULT(proof, vm::MerkleProof::generate(root, is_prunned));
      auto [it, inserted] = proofs.emplace(root->get_hash().bits(), proof);
      if (!inserted) {
        TRY_RESULT_ASSIGN(it->second, vm::MerkleProof::combine(it->second, std::move(proof)));
      }
    }
    for (auto& [_, proof] : proofs) {
      roots.push_back(std::move(proof));
    }
    for (const auto& root : storage_roots) {
      TRY_RESULT(proof, vm::MerkleProof::generate(root, is_prunned));
      // account_storage_dict_proof#37c1e3fc proof:^Cell = AccountStorageDictProof;
      roots.push_back(vm::CellBuilder()
                          .store_long(block::gen::AccountStorageDictProof::cons_tag[0], 32)
                          .store_ref(std::move(proof))
                          .finalize_novm());
    }
    return td::Status::OK();
  }

  for (const auto& root : state_roots) {
    Ref<vm::Cell> cell;
    TRY_BOOL(
        block::gen::t_CollatedDataRootState.cell_pack_collated_data_root_state(cell, root->get_hash().as_bits256()));
    roots.push_back(std::move(cell));
  }
  for (const auto& root : storage_roots) {
    Ref<vm::Cell> cell;
    TRY_BOOL(block::gen::t_CollatedDataRootStorageDict.cell_pack_collated_data_root_storage_dict(
        cell, root->get_hash().as_bits256()));
    roots.push_back(std::move(cell));
  }
  Ref<vm::Cell> separator;
  TRY_BOOL(block::gen::t_CollatedDataSeparator.cell_pack_collated_data_separator(separator));
  roots.push_back(std::move(separator));
  td::HashMap<vm::CellHash, Ref<vm::Cell>> cache;
  std::function<Ref<vm::Cell>(const Ref<vm::Cell>&)> dfs = [&](const Ref<vm::Cell>& cell) -> Ref<vm::Cell> {
    auto it = cache.find(cell->get_hash());
    if (it != cache.end()) {
      return it->second;
    }
    if (is_prunned(cell)) {
      return cache[cell->get_hash()] = vm::CellBuilder::create_pruned_branch(cell, vm::Cell::max_level);
    }
    vm::CellSlice cs(vm::NoVm(), cell);
    vm::CellBuilder cb;
    cb.store_bits(cs.fetch_bits(cs.size()));
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      cb.store_ref(dfs(cs.prefetch_ref(i)));
    }
    auto hash_hint = [&](unsigned level, const vm::Cell::LevelMask&, vm::CellHash& hash) {
      if (level <= cell->get_level()) {
        hash = cell->get_hash(level);
        return true;
      }
      return false;
    };
    return cache[cell->get_hash()] = cb.finalize(cs.is_special(), std::move(hash_hint));
  };
  for (const auto& root : state_roots) {
    roots.push_back(dfs(root));
  }
  for (const auto& root : storage_roots) {
    roots.push_back(dfs(root));
  }
  return td::Status::OK();
}

td::actor::Task<Collator::CollatedData::Result> Collator::CollatedData::finalize(CollatedData data, bool parallel) {
  if (parallel) {
    co_await td::actor::yield_on(td::actor::Executor::on_scheduler());
  }
  Result result;
  {
    td::RealCpuTimer timer;
    CO_TRY(data.build());
    result.proofs_time = timer.elapsed_both();
  }
  {
    td::RealCpuTimer timer;
    result.data = CO_TRY(vm::std_boc_serialize_multi(std::move(data.roots), 2));
    result.serialization_time = timer.elapsed_both();
  }
  co_return result;
}

td::actor::Task<bool> Collator::create_block_candidate() {
  CollatedData data;
  {
    td::ScopedRealCpuTimer::Guard timer{work_timer_, &stats_.work_time.create_collated_data};
    if (!prepare_collated_data(data)) {
      co_return false;
    }
  }
  auto job = CollatedData::finalize(std::move(data), virtual_shards_ != nullptr).start_immediate();
  td::Result<td::BufferSlice> block;
  {
    td::ScopedRealCpuTimer::Guard timer{work_timer_, &stats_.work_time.create_block_candidate};
    block = vm::std_boc_serialize(new_block, 31);
  }
  td::Result<CollatedData::Result> result;
  {
    td::ScopedRealCpuTimer::Guard total_timer{work_timer_total_, nullptr};
    td::ScopedRealCpuTimer::Guard timer{work_timer_, nullptr};
    td::Timer wall;
    SCOPE_EXIT {
      stats_.work_time.total.real += wall.elapsed();
    };
    result = co_await std::move(job).wrap();
  }
  if (block.is_error()) {
    co_return fatal_error(block.move_as_error_prefix("cannot serialize block: "));
  }
  if (result.is_error()) {
    co_return fatal_error(result.move_as_error_prefix("cannot finalize collated data: "));
  }
  auto completed = result.move_as_ok();
  stats_.work_time.create_collated_data += completed.proofs_time;
  stats_.work_time.create_block_candidate += completed.serialization_time;
  if (virtual_shards_) {
    stats_.work_time.total.cpu += completed.proofs_time.cpu + completed.serialization_time.cpu;
  }
  if (!check_cancelled()) {
    co_return false;
  }
  td::ScopedRealCpuTimer::Guard timer{work_timer_, &stats_.work_time.create_block_candidate};
  co_return publish_block_candidate(block.move_as_ok(), std::move(completed.data));
}

}  // namespace ton::validator
