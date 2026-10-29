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
#include <algorithm>
#include <exception>
#include <optional>

#include "block/block-auto.h"
#include "common/bitstring.h"
#include "td/actor/coro_utils.h"
#include "td/utils/algorithm.h"
#include "ton/ton-shard.h"
#include "vm/cells/UsageCell.h"

#include "collator-impl.h"

namespace ton::validator {
namespace {

class MessageDetacher {
 public:
  td::Result<Ref<vm::Cell>> detach(Ref<vm::Cell> cell) {
    if (cell.is_null()) {
      return cell;
    }
    auto pure = cell->untrack();
    if (pure != cell) {
      return pure;
    }
    auto it = cache_.find(cell->get_hash());
    if (it != cache_.end()) {
      return it->second;
    }
    TRY_RESULT(loaded, cell->load_cell());
    auto data = loaded.data_cell;
    Ref<vm::Cell> refs[vm::Cell::max_refs];
    bool changed = false;
    for (unsigned i = 0; i < data->size_refs(); ++i) {
      TRY_RESULT(child, detach(data->get_ref(i)));
      changed |= child != data->get_ref(i);
      refs[i] = std::move(child);
    }
    if (changed) {
      TRY_RESULT(rebuilt, vm::DataCell::create({data->get_data(), (data->size() + 7) / 8}, data->size(),
                                               {refs, data->size_refs()}, data->is_special()));
      cell = rebuilt->virtualize(loaded.effective_level);
    }
    return cache_[cell->get_hash()] = std::move(cell);
  }

 private:
  td::HashMap<vm::CellHash, Ref<vm::Cell>> cache_;
};

struct EpochBudget {
  td::uint64 bytes = 0, gas = 0, collated_data = 0;
  LogicalTime lt = 0;
};

struct ReadJournal {
  struct Checkpoint {
    size_t proof = 0, usage = 0;
  };
  vm::ProofStorageStat::Recorder proof;
  vm::CellUsageTree::Recorder usage;

  void begin_epoch(vm::ProofStorageStat& stat, vm::CellUsageTree& tree) {
    proof.begin_epoch(&stat);
    usage.begin_epoch(&tree);
  }
  Checkpoint checkpoint() const {
    return {proof.checkpoint(), usage.checkpoint()};
  }
  void commit_epoch() {
    proof.commit_epoch();
    usage.commit_epoch();
  }
  void rollback(Checkpoint checkpoint) {
    proof.rollback(checkpoint.proof);
    usage.rollback(checkpoint.usage);
  }
};

}  // namespace

struct Collator::VirtualShards {
  struct Request {
    Ref<vm::Cell> message;
    StdSmcAddress address;
    bool external = false;
    bool in_msg_accounted = false;
    LogicalTime after_lt = 0, end_lt = 0;
    unsigned owner = 0;
    std::unique_ptr<block::transaction::Transaction> transaction;
    td::Status status;
  };

  struct Worker : CollatorBase {
    struct Checkpoint {
      std::unique_ptr<block::Account::Checkpoint> account;
      Ref<vm::Cell> account_dict_root;
      bool account_dict_updated = false;
      vm::NewCellStorageStat::Stat block_stat;
      td::uint64 collated_data_size = 0;
      ReadJournal::Checkpoint state_reads;
      size_t cache_updates_size = 0;
      td::optional<td::Bits256> storage;
      td::optional<ReadJournal::Checkpoint> storage_reads;
      size_t storage_updates_size = 0;
    };

    StdSmcAddress prefix = StdSmcAddress::zero();
    unsigned prefix_bits = 0;
    Ref<vm::Cell> prev_state_root_pure_;
    block::BlockLimits block_limits_;
    block::WorkchainSet workchains_;
    td::CancellationToken cancellation_token_;
    td::Timestamp timeout_;

    std::vector<Request*> pending;
    size_t prepared = 0, selected = 0;
    bool recording = false;
    double cpu = 0;
    Ref<vm::Cell> proof_root, new_proof, old_proof;
    ReadJournal state_reads;
    std::map<td::Bits256, ReadJournal> storage_reads;
    std::vector<Checkpoint> checkpoints;
    vm::NewCellStorageStat::Stat final_stat;
    td::uint64 final_storage_proof_size = 0;
    vm::NewCellStorageStat::Stat published_stat;
    td::uint64 published_collated_data_size = 0;

    explicit Worker(ShardIdFull shard);
    td::Status init(Collator& parent, unsigned index, unsigned partition_bits);
    td::Result<block::Account*> make_account(const StdSmcAddress& address);
    td::Status create_ordinary_transaction(Request& request);
    void run_epoch(EpochBudget budget);
    void begin_epoch();
    void save_checkpoint(Request& request);
    void finish_epoch();
    void rollback();
    vm::NewCellStorageStat::Stat get_block_stat() const;
    td::uint64 get_collated_data_size() const;
    void sync_limits(block::BlockLimitStatus& common);
    td::Status generate_new_proof();
    td::Status generate_old_proof();
  };

  std::vector<std::unique_ptr<Worker>> workers;
  unsigned prefix_bits = 0, partition_bits = 0;
  bool stopped = false;
  EpochBudget budget;
  std::vector<std::unique_ptr<Request>> requests;
  Request* publishing = nullptr;

  unsigned owner(const StdSmcAddress& address) const;
  void sync_limits(block::BlockLimitStatus& common);
  bool begin_publish(Request* request, block::BlockLimitStatus& common);
  void end_publish(block::BlockLimitStatus& common);
  td::Result<Request*> plan(Collator& parent, Ref<vm::Cell> message, const StdSmcAddress& address, bool external,
                            LogicalTime after_lt);
  td::actor::Task<bool> run_epoch(Collator& parent, unsigned limit_class);
  td::actor::Task<bool> finish_epoch(Collator& parent, unsigned limit_class);
  enum class Phase { RunEpoch, Rollback, Finish, NewProofs, OldProofs };
  static td::actor::Task<> run(std::shared_ptr<VirtualShards> state, unsigned i, Phase phase);
  static td::actor::Task<> run_all(Collator& parent, Phase phase);
  void split_proof_roots(Ref<vm::Cell> root, int depth, StdSmcAddress prefix);
  void clear_requests();
};
Collator::VirtualShards::Worker::Worker(ShardIdFull shard) : CollatorBase(shard) {
}

td::Status Collator::VirtualShards::Worker::init(Collator& parent, unsigned index, unsigned partition_bits) {
  now_ = parent.now_;
  start_lt = parent.start_lt;
  full_collated_data_ = parent.full_collated_data_;
  cancellation_token_ = parent.cancellation_token_;
  timeout_ = parent.timeout_;
  storage_stat_cache_ = parent.storage_stat_cache_;
  state_usage_tree_ = std::make_shared<vm::CellUsageTree>(parent.prev_state_root_pure_);
  block_limits_ = *parent.block_limits_;
  block_limits_.usage_tree = state_usage_tree_.get();
  block_limit_status_ = std::make_unique<block::BlockLimitStatus>(block_limits_);
  if (full_collated_data_) {
    state_usage_tree_->set_cell_load_callback([this](const vm::LoadedCell& cell) { on_cell_loaded(cell); });
  }
  prev_state_root_pure_ = parent.prev_state_root_pure_;
  auto prev_state_root = vm::UsageCell::create(prev_state_root_pure_, state_usage_tree_->root_ptr());
  block::gen::ShardStateUnsplit::Record state;
  if (!tlb::unpack_cell(prev_state_root, state)) {
    return td::Status::Error("cannot unpack virtual shard predecessor");
  }
  old_account_dict = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(state.accounts), 256,
                                                               block::tlb::aug_ShardAccounts);
  prefix_bits = ton::shard_prefix_length(shard_);
  if (prefix_bits) {
    prefix.bits().store_uint(shard_.shard >> (64 - prefix_bits), prefix_bits);
  }
  if (partition_bits) {
    (prefix.bits() + prefix_bits).store_uint(index, partition_bits);
  }
  prefix_bits += partition_bits;
  if (!old_account_dict->cut_prefix_subdict(prefix.cbits(), prefix_bits)) {
    return td::Status::Error("cannot split account dictionary");
  }
  account_dict =
      std::make_unique<vm::AugmentedDictionary>(old_account_dict->get_root(), 256, block::tlb::aug_ShardAccounts);
  Ref<vm::Cell> old_params;
  td::RefInt256 mc_fee, shard_fee;
  auto seed = parent.rand_seed_;
  TRY_STATUS(block::FetchConfigParams::fetch_config_params(
      *parent.config_, &old_params, &storage_prices_, &storage_phase_cfg_, &seed, &compute_phase_cfg_,
      &action_phase_cfg_, &serialize_cfg_, &mc_fee, &shard_fee, shard_.workchain, now_));
  compute_phase_cfg_.libraries = std::make_unique<vm::Dictionary>(parent.config_->get_libraries_root(), 256);
  if (action_phase_cfg_.workchains) {
    workchains_ = *action_phase_cfg_.workchains;
    action_phase_cfg_.workchains = &workchains_;
  }
  return td::Status::OK();
}

td::Result<block::Account*> Collator::VirtualShards::Worker::make_account(const StdSmcAddress& address) {
  if (td::bitstring::bits_memcmp(address.cbits(), prefix.cbits(), prefix_bits)) {
    return td::Status::Error("account belongs to another virtual shard");
  }
  return CollatorBase::make_account(address.cbits(), true, nullptr);
}

td::Status Collator::VirtualShards::Worker::create_ordinary_transaction(Request& request) {
  if (cancellation_token_) {
    return td::Status::Error(ErrorCode::cancelled, "virtual shard collation cancelled");
  }
  current_tx_storage_dict_ = nullptr;
  TRY_RESULT(account, make_account(request.address));
  TRY_RESULT_ASSIGN(request.transaction, CollatorBase::create_ordinary_transaction(request.message, account,
                                                                                   request.external, request.after_lt));
  if (request.in_msg_accounted) {
    block_limit_status_->st_stat.mark_subtree_accounted(request.message->get_hash());
  }
  request.end_lt = request.transaction->end_lt;
  return commit_transaction(*request.transaction, *account);
}

void Collator::VirtualShards::Worker::begin_epoch() {
  recording = true;
  state_reads.begin_epoch(collated_data_stat, *state_usage_tree_);
  record_storage_dict_ = [this](const td::Bits256& hash, AccountStorageDict& dict, bool created) {
    auto [it, inserted] = storage_reads.try_emplace(hash);
    auto& journal = it->second;
    if (inserted) {
      journal.begin_epoch(dict.proof_stat, dict.mpb.get_usage_tree());
    }
    auto& checkpoint = checkpoints.back();
    if (!checkpoint.storage) {
      checkpoint.storage = it->first;
      if (!created) {
        checkpoint.storage_reads = journal.checkpoint();
      }
      checkpoint.storage_updates_size = dict.storage_stat_updates.size();
    }
    CHECK(checkpoint.storage == it->first);
  };
}

void Collator::VirtualShards::Worker::save_checkpoint(Request& request) {
  auto& checkpoint = checkpoints.emplace_back();
  if (auto* account = lookup_account(request.address.cbits())) {
    checkpoint.account = std::make_unique<block::Account::Checkpoint>(account->checkpoint());
  }
  checkpoint.state_reads = state_reads.checkpoint();
  checkpoint.cache_updates_size = storage_stat_cache_update_.size();
  checkpoint.account_dict_root = account_dict->get_root_cell();
  checkpoint.account_dict_updated = account_dict_updated_accounts_.contains(request.address);
  checkpoint.block_stat = get_block_stat();
  checkpoint.collated_data_size = get_collated_data_size();
}

void Collator::VirtualShards::Worker::run_epoch(EpochBudget budget) {
  CHECK(block_limit_status_);
  finish_epoch();
  if (pending.empty()) {
    return;
  }
  begin_epoch();
  checkpoints.reserve(pending.size());
  budget.bytes += block_limit_status_->estimate_block_size();
  budget.gas += block_limit_status_->gas_used;
  budget.collated_data += block_limit_status_->collated_data_size_estimate;
  SCOPE_EXIT {
    record_storage_dict_ = {};
    current_tx_storage_dict_ = nullptr;
  };
  for (auto* request : pending) {
    if (timeout_.is_in_past()) {
      break;
    }
    save_checkpoint(*request);
    request->status = create_ordinary_transaction(*request);
    ++prepared;
    if (request->status.is_error() && request->status.error().code() != -701) {
      break;
    }
    if (block_limit_status_->estimate_block_size() >= budget.bytes || block_limit_status_->gas_used >= budget.gas ||
        block_limit_status_->collated_data_size_estimate >= budget.collated_data ||
        block_limit_status_->cur_lt >= budget.lt) {
      break;
    }
  }
}

void Collator::VirtualShards::Worker::finish_epoch() {
  if (!recording) {
    return;
  }
  record_storage_dict_ = {};
  current_tx_storage_dict_ = nullptr;
  for (auto& [hash, journal] : storage_reads) {
    journal.commit_epoch();
  }
  state_reads.commit_epoch();
  storage_reads.clear();
  checkpoints.clear();
  recording = false;
}

void Collator::VirtualShards::Worker::rollback() {
  CHECK(recording && selected < prepared && checkpoints.size() == prepared);
  std::map<StdSmcAddress, std::unique_ptr<block::Account::Checkpoint>> account_checkpoints;
  std::map<td::Bits256, Checkpoint*> storage_checkpoints;
  for (size_t i = selected; i < prepared; ++i) {
    auto& checkpoint = checkpoints[i];
    const auto& address = pending[i]->address;
    account_checkpoints.try_emplace(address, std::move(checkpoint.account));
    if (!checkpoint.account_dict_updated) {
      account_dict_updated_accounts_.erase(address);
    }
    if (checkpoint.storage) {
      storage_checkpoints.try_emplace(checkpoint.storage.unwrap(), &checkpoint);
    }
    pending[i]->transaction.reset();
  }
  for (auto& [address, checkpoint] : account_checkpoints) {
    if (checkpoint) {
      auto* account = lookup_account(address.cbits());
      CHECK(account);
      account->rollback(std::move(*checkpoint));
    } else {
      accounts.erase(address);
    }
  }
  for (auto& [hash, checkpoint] : storage_checkpoints) {
    auto it = storage_reads.find(hash);
    CHECK(it != storage_reads.end());
    if (checkpoint->storage_reads) {
      it->second.rollback(*checkpoint->storage_reads);
      account_storage_dicts_.at(hash).storage_stat_updates.resize(checkpoint->storage_updates_size);
    } else {
      it->second.commit_epoch();
      storage_reads.erase(it);
      account_storage_dicts_.erase(hash);
    }
  }
  const auto& first = checkpoints[selected];
  account_dict = std::make_unique<vm::AugmentedDictionary>(first.account_dict_root, 256, block::tlb::aug_ShardAccounts);
  state_reads.rollback(first.state_reads);
  storage_stat_cache_update_.resize(first.cache_updates_size);
  final_stat = first.block_stat;
  final_storage_proof_size = 0;
  for (const auto& [hash, dict] : account_storage_dicts_) {
    final_storage_proof_size += dict.proof_stat.estimate_proof_size();
  }
  block_limit_status_.reset();
  prepared = selected;
  finish_epoch();
}

vm::NewCellStorageStat::Stat Collator::VirtualShards::Worker::get_block_stat() const {
  if (!block_limit_status_) {
    return final_stat;
  }
  auto stat = block_limit_status_->st_stat.get_total_stat();
  stat.bits += block_limit_status_->merkle_update_left_stat.estimate_proof_size() * 8;
  return stat;
}

td::uint64 Collator::VirtualShards::Worker::get_collated_data_size() const {
  if (!block_limit_status_) {
    return collated_data_stat.estimate_proof_size() + final_storage_proof_size;
  }
  return block_limit_status_->collated_data_size_estimate;
}

td::Status Collator::VirtualShards::Worker::generate_new_proof() {
  if (proof_root.not_null()) {
    vm::CellUsageTree* tree = state_usage_tree_.get();
    TRY_RESULT_ASSIGN(new_proof, vm::MerkleUpdate::generate_to_raw(proof_root, tree));
  }
  return td::Status::OK();
}

td::Status Collator::VirtualShards::Worker::generate_old_proof() {
  TRY_RESULT_ASSIGN(old_proof, vm::MerkleProof::generate_raw(prev_state_root_pure_, state_usage_tree_.get(), true));
  if (full_collated_data_ &&
      !old_account_dict->scan_diff(
          *account_dict, [](td::ConstBitPtr, int, Ref<vm::CellSlice>, Ref<vm::CellSlice>) { return true; },
          vm::DictionaryFixed::check_new_aug)) {
    return td::Status::Error("cannot prepare virtual shard account proof");
  }
  return td::Status::OK();
}

unsigned Collator::VirtualShards::owner(const StdSmcAddress& address) const {
  return partition_bits ? static_cast<unsigned>((address.cbits() + prefix_bits).get_uint(partition_bits)) : 0;
}

void Collator::VirtualShards::Worker::sync_limits(block::BlockLimitStatus& common) {
  CHECK(selected <= prepared);
  vm::NewCellStorageStat::Stat stat;
  td::uint64 collated_data_size;
  if (selected < prepared) {
    const auto& checkpoint = checkpoints[selected];
    stat = checkpoint.block_stat;
    collated_data_size = checkpoint.collated_data_size;
  } else {
    stat = get_block_stat();
    collated_data_size = get_collated_data_size();
  }
  common.additional_stat -= published_stat;
  common.additional_stat += stat;
  published_stat = stat;
  CHECK(common.collated_data_size_estimate >= published_collated_data_size);
  common.collated_data_size_estimate -= published_collated_data_size;
  common.collated_data_size_estimate += collated_data_size;
  published_collated_data_size = collated_data_size;
}

void Collator::VirtualShards::sync_limits(block::BlockLimitStatus& common) {
  for (auto& worker : workers) {
    worker->sync_limits(common);
  }
}

bool Collator::VirtualShards::begin_publish(Request* request, block::BlockLimitStatus& common) {
  CHECK(!publishing);
  if (stopped) {
    return false;
  }
  if (request) {
    auto& worker = *workers[request->owner];
    if (worker.selected == worker.prepared) {
      stopped = true;
      return false;
    }
    CHECK(worker.pending[worker.selected] == request);
    ++worker.selected;
    worker.sync_limits(common);
  }
  publishing = request;
  return true;
}

void Collator::VirtualShards::end_publish(block::BlockLimitStatus& common) {
  if (publishing) {
    workers[publishing->owner]->sync_limits(common);
    publishing = nullptr;
  }
}

td::Result<Collator::VirtualShards::Request*> Collator::VirtualShards::plan(Collator& parent, Ref<vm::Cell> message,
                                                                            const StdSmcAddress& address, bool external,
                                                                            LogicalTime after_lt) {
  auto request = std::make_unique<Request>();
  request->owner = owner(address);
  request->address = address;
  request->external = external;
  if (external) {
    after_lt = std::max(after_lt, parent.last_proc_int_msg_.first);
  }
  auto it = parent.last_dispatch_queue_emitted_lt_.find(address);
  if (it != parent.last_dispatch_queue_emitted_lt_.end()) {
    after_lt = std::max(after_lt, it->second);
  }
  request->after_lt = after_lt;
  TRY_RESULT_ASSIGN(request->message, MessageDetacher().detach(std::move(message)));
  workers[request->owner]->pending.push_back(request.get());
  requests.push_back(std::move(request));
  return requests.back().get();
}

td::actor::Task<bool> Collator::VirtualShards::run_epoch(Collator& parent, unsigned limit_class) {
  const auto& common = *parent.block_limit_status_;
  CHECK(limit_class < block::ParamLimits::limits_cnt);
  auto remaining = [limit_class](const block::ParamLimits& limits, td::uint64 used) {
    auto limit = limits.limit(limit_class);
    return used < limit ? limit - used : 0;
  };
  budget = {remaining(common.limits.bytes, common.estimate_block_size()), remaining(common.limits.gas, common.gas_used),
            remaining(common.limits.collated_data, common.collated_data_size_estimate),
            common.limits.start_lt + common.limits.lt_delta.limit(limit_class)};
  for (auto& worker : workers) {
    worker->prepared = worker->selected = 0;
    worker->timeout_ =
        limit_class == block::ParamLimits::cl_soft ? parent.external_msg_timeout_ : parent.internal_msg_timeout_;
  }
  auto result = co_await run_all(parent, Phase::RunEpoch).wrap();
  if (result.is_error()) {
    co_return parent.fatal_error(result.move_as_error());
  }
  co_return true;
}

td::actor::Task<bool> Collator::VirtualShards::finish_epoch(Collator& parent, unsigned limit_class) {
  if (stopped) {
    auto result = co_await run_all(parent, Phase::Rollback).wrap();
    if (result.is_error()) {
      co_return parent.fatal_error(result.move_as_error());
    }
  }
  sync_limits(*parent.block_limit_status_);
  parent.block_full_ |= parent.virtual_shard_budget_exhausted(limit_class);
  co_return true;
}

td::actor::Task<> Collator::VirtualShards::run(std::shared_ptr<VirtualShards> state, unsigned i, Phase phase) {
  co_await td::actor::yield_on(td::actor::Executor::on_scheduler());
  try {
    auto& worker = *state->workers[i];
    td::RealCpuTimer cpu_timer;
    SCOPE_EXIT {
      worker.cpu += cpu_timer.elapsed_cpu();
    };
    switch (phase) {
      case Phase::RunEpoch:
        worker.run_epoch(state->budget);
        co_return {};
      case Phase::Rollback:
        worker.rollback();
        co_return {};
      case Phase::Finish:
        worker.finish_epoch();
        co_return worker.combine_account_transactions();
      case Phase::NewProofs:
        co_return worker.generate_new_proof();
      case Phase::OldProofs:
        co_return worker.generate_old_proof();
    }
    UNREACHABLE();
  } catch (const vm::VmError& e) {
    co_return td::Status::Error(PSLICE() << "virtual shard VM error: " << e.get_msg());
  } catch (const std::exception& e) {
    co_return td::Status::Error(PSLICE() << "virtual shard error: " << e.what());
  } catch (...) {
    co_return td::Status::Error("virtual shard exception");
  }
}

td::actor::Task<> Collator::VirtualShards::run_all(Collator& parent, Phase phase) {
  auto state = parent.virtual_shards_;
  td::ScopedRealCpuTimer::Guard timer_total{parent.work_timer_total_, nullptr};
  td::ScopedRealCpuTimer::Guard timer{parent.work_timer_, nullptr};
  double cpu_before = 0;
  for (auto& worker : state->workers) {
    cpu_before += worker->cpu;
  }
  td::Timer wall;
  SCOPE_EXIT {
    td::RealCpuTimer::Time elapsed{wall.elapsed(), -cpu_before};
    for (auto& worker : state->workers) {
      elapsed.cpu += worker->cpu;
    }
    parent.stats_.work_time.total += elapsed;
    if (phase == Phase::Finish) {
      parent.stats_.work_time.combine_account_transactions += elapsed;
    }
  };
  std::vector<td::actor::StartedTask<>> jobs;
  for (unsigned i = 0; i < state->workers.size(); ++i) {
    if (phase == Phase::RunEpoch && state->workers[i]->pending.empty()) {
      continue;
    }
    if (phase == Phase::Rollback && state->workers[i]->selected == state->workers[i]->prepared) {
      continue;
    }
    jobs.push_back(run(state, i, phase).start());
  }
  auto results = co_await td::actor::all_wrap(std::move(jobs));
  for (auto& result : results) {
    CO_TRY(std::move(result));
  }
  if (phase == Phase::RunEpoch) {
    state->sync_limits(*parent.block_limit_status_);
  }
  co_return {};
}

void Collator::VirtualShards::split_proof_roots(Ref<vm::Cell> root, int depth, StdSmcAddress prefix) {
  if (root.is_null()) {
    return;
  }
  vm::dict::LabelParser label(root, 256 - depth, vm::dict::LabelParser::chk_size);
  depth += label.extract_label_to(prefix.bits() + depth);
  if (depth >= static_cast<int>(prefix_bits + partition_bits)) {
    auto& proof_root = workers[owner(prefix)]->proof_root;
    CHECK(proof_root.is_null());
    proof_root = std::move(root);
    return;
  }
  for (int branch = 0; branch < 2; ++branch) {
    (prefix.bits() + depth).store_uint(branch, 1);
    split_proof_roots(label.remainder->prefetch_ref(branch), depth + 1, prefix);
  }
}

void Collator::VirtualShards::clear_requests() {
  for (auto& worker : workers) {
    worker->pending.clear();
  }
  publishing = nullptr;
  requests.clear();
}

td::Status Collator::init_virtual_shards() {
  auto count = params_.collator_opts->virtual_shards;
  if (!count || is_masterchain() || before_split_ || after_split_ || after_merge_) {
    return td::Status::OK();
  }
  if (count > 64 || (count & (count - 1))) {
    return td::Status::Error("virtual_shards must be a power of two between 1 and 64");
  }
  CHECK(!virtual_shards_);
  if (!accounts.empty() || !account_storage_dicts_.empty()) {
    return td::Status::Error("virtual shards must be initialized before loading accounts");
  }
  auto state = std::make_shared<VirtualShards>();
  state->prefix_bits = ton::shard_prefix_length(shard_);
  state->partition_bits = td::count_trailing_zeroes32(count);
  std::vector<vm::CellUsageTree*> usage_trees;
  for (unsigned i = 0; i < count; ++i) {
    auto worker = std::make_unique<VirtualShards::Worker>(shard_);
    TRY_STATUS(worker->init(*this, i, state->partition_bits));
    usage_trees.push_back(worker->state_usage_tree_.get());
    state->workers.push_back(std::move(worker));
  }
  state->sync_limits(*block_limit_status_);
  block_limit_status_->st_stat.set_additional_usage_trees(std::move(usage_trees));
  virtual_shards_ = std::move(state);
  return td::Status::OK();
}

bool Collator::virtual_shard_budget_exhausted(unsigned cls) {
  if (virtual_shards_->requests.empty()) {
    virtual_shards_->sync_limits(*block_limit_status_);
  }
  if (!block_limit_status_->fits(cls)) {
    virtual_shards_->stopped = true;
    block_full_ = true;
    block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
  }
  return virtual_shards_->stopped;
}

td::Result<std::unique_ptr<block::transaction::Transaction>> Collator::create_virtual_transaction(
    Ref<vm::Cell> msg_root, const StdSmcAddress& addr, bool external, LogicalTime after_lt) {
  auto* prepared = virtual_shards_->publishing;
  CHECK(prepared && prepared->address == addr && prepared->external == external);
  CHECK(prepared->message->get_hash() == msg_root->get_hash() && prepared->after_lt >= after_lt);
  auto& request = *prepared;
  TRY_STATUS(std::move(request.status));
  auto& trans = *request.transaction;
  // The published prefix statistics already include these subtrees. Shared
  // descriptors only need their own cells and references to these roots.
  auto& stat = block_limit_status_->st_stat;
  stat.mark_subtree_accounted(trans.root->get_hash());
  stat.mark_subtree_accounted(msg_root->get_hash());
  for (const auto& message : trans.out_msgs) {
    stat.mark_subtree_accounted(message->get_hash());
  }
  // The account may already contain later, speculative transactions.
  if (!(block_limit_status_->update_gas(trans.gas_used()) && block_limit_status_->update_lt(request.end_lt) &&
        block_limit_status_->add_transaction() && block_limit_status_->add_account(trans.is_first) &&
        update_max_lt(request.end_lt))) {
    return td::Status::Error("cannot update common transaction limits");
  }
  return std::move(request.transaction);
}

td::actor::Task<bool> Collator::process_virtual_externals() {
  td::ScopedRealCpuTimer::Guard timer_total{work_timer_total_, &stats_.work_time.total};
  td::ScopedRealCpuTimer::Guard timer{work_timer_, &stats_.work_time.import_externals};
  SCOPE_EXIT {
    stats_.load_fraction_externals = block_limit_status_->load_fraction(block::ParamLimits::cl_soft);
  };
  if (skip_extmsg_ || params_.attempt_idx >= 2) {
    co_return true;
  }
  bool eof = false;
  while (!eof && !virtual_shard_budget_exhausted(block::ParamLimits::cl_soft) && !external_msg_timeout_.is_in_past()) {
    if (!check_cancelled()) {
      co_return false;
    }
    struct Item {
      Ref<ExtMessage> message;
      VirtualShards::Request* request;
    };
    std::vector<Item> batch;
    std::set<td::Bits256> batch_hashes;
    SCOPE_EXIT {
      virtual_shards_->clear_requests();
    };
    while (batch.size() < virtual_shards_->workers.size() * 32) {
      std::pair<Ref<ExtMessage>, int> item;
      if (pending_ext_msg_) {
        item = std::move(*pending_ext_msg_);
        pending_ext_msg_.reset();
      } else {
        td::ScopedRealCpuTimer::Guard timer_total{work_timer_total_, nullptr};
        td::ScopedRealCpuTimer::Guard timer{work_timer_, nullptr};
        td::Timer wait_timer;
        auto next = params_.wait_externals_until ? co_await ext_msg_queue_.try_pop().wrap()
                                                 : co_await ext_msg_queue_.pop().wrap();
        wait_externals_total_time_ += wait_timer.elapsed();
        if (next.is_error()) {
          eof = true;
          break;
        }
        item = next.move_as_ok();
      }
      auto& message = item.first;
      if (out_msg_queue_size_ > SKIP_EXTERNALS_QUEUE_SIZE && item.second < HIGH_PRIORITY_EXTERNAL) {
        ++stats_.ext_msgs_total;
        ++stats_.ext_msgs_skipped_backpressure;
        continue;
      }
      if (processed_external_messages_.find(message->hash_norm()) || batch_hashes.count(message->hash_norm())) {
        ++stats_.ext_msgs_total;
        ++stats_.ext_msgs_skipped_duplicate;
        continue;
      }
      VirtualShards::Request* request = nullptr;
      if (register_external_message(message, item.second).is_ok()) {
        auto planned = virtual_shards_->plan(*this, message->root_cell(), message->addr(), true, 0);
        if (planned.is_error()) {
          co_return fatal_error(planned.move_as_error());
        }
        request = planned.move_as_ok();
      }
      ++stats_.ext_msgs_total;
      batch_hashes.insert(message->hash_norm());
      batch.push_back({std::move(message), request});
    }
    if (batch.empty()) {
      continue;
    }
    if (!(co_await virtual_shards_->run_epoch(*this, block::ParamLimits::cl_soft))) {
      co_return false;
    }
    for (auto& item : batch) {
      if (!virtual_shards_->begin_publish(item.request, *block_limit_status_)) {
        break;
      }
      processed_external_messages_ = processed_external_messages_.insert(item.message->hash_norm(), td::Unit{});
      if (!item.request) {
        ++stats_.ext_msgs_filtered;
        bad_ext_msgs_.push_back(item.message->hash_norm());
        continue;
      }
      int result = process_external_message(item.message->root_cell());
      virtual_shards_->end_publish(*block_limit_status_);
      if (result > 0) {
        ++stats_.ext_msgs_accepted;
      } else {
        ++stats_.ext_msgs_rejected;
        bad_ext_msgs_.push_back(item.message->hash_norm());
      }
      if (result < 0) {
        co_return false;
      }
      if (virtual_shard_budget_exhausted(block::ParamLimits::cl_soft)) {
        break;
      }
    }
    if (!(co_await virtual_shards_->finish_epoch(*this, block::ParamLimits::cl_soft))) {
      co_return false;
    }
    block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
  }
  co_return true;
}

td::actor::Task<bool> Collator::process_virtual_internals() {
  SCOPE_EXIT {
    stats_.load_fraction_internals = block_limit_status_->load_fraction(block::ParamLimits::cl_normal);
  };
  bool stopped = false;
  while (!nb_out_msgs_->is_eof() && !stopped) {
    if (!check_cancelled()) {
      co_return false;
    }
    if (virtual_shard_budget_exhausted(block::ParamLimits::cl_normal) || internal_msg_timeout_.is_in_past() ||
        have_unprocessed_account_dispatch_queue_) {
      auto* entry = nb_out_msgs_->cur();
      if (!precheck_inbound_message(entry->msg, entry->lt)) {
        co_return fatal_error("invalid virtual shard inbound frontier");
      }
      block_full_ = !have_unprocessed_account_dispatch_queue_;
      stopped = true;
      break;
    }
    struct Item {
      std::unique_ptr<block::OutputQueueMerger::MsgKeyValue> entry;
      VirtualShards::Request* request;
    };
    std::vector<Item> batch;
    SCOPE_EXIT {
      virtual_shards_->clear_requests();
    };
    while (!nb_out_msgs_->is_eof() && batch.size() < virtual_shards_->workers.size() * 32) {
      auto* entry = nb_out_msgs_->cur();
      CHECK(entry);
      if (entry->limit_exceeded) {
        stats_.neighbors.at(entry->source).limit_reached = true;
        block::EnqueuedMsgDescr descr;
        descr.unpack(entry->msg.write());  // Include the unprocessed frontier in the proof.
        stopped = true;
        break;
      }
      block::tlb::MsgEnvelope::Record_std envelope;
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      // Lookahead only needs the header. Reveal the tracked envelope/message
      // when publishing it, or when proving the first unprocessed frontier.
      auto message_envelope = entry->msg->prefetch_ref();
      if (message_envelope.is_null() || !block::tlb::unpack_cell(message_envelope->untrack(), envelope) ||
          !tlb::unpack_cell_inexact(envelope.msg, info)) {
        co_return fatal_error("cannot unpack virtual shard inbound message");
      }
      auto src = block::tlb::MsgAddressInt::get_prefix(info.src);
      auto dest = block::tlb::MsgAddressInt::get_prefix(info.dest);
      auto cur = block::interpolate_addr(src, dest, envelope.cur_addr);
      auto next = block::interpolate_addr(src, dest, envelope.next_addr);
      block::EnqueuedMsgDescr descr{cur, next, envelope.emitted_lt ? envelope.emitted_lt.value() : info.created_lt,
                                    entry->msg->prefetch_ulong(64), envelope.msg->get_hash().bits()};
      VirtualShards::Request* request = nullptr;
      if (is_our_address(info.dest) && !processed_upto_->already_processed(descr)) {
        WorkchainId wc;
        StdSmcAddress address;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(info.dest, wc, address) || wc != workchain()) {
          co_return fatal_error("invalid virtual shard inbound destination");
        }
        auto planned = virtual_shards_->plan(*this, envelope.msg, address, false, 0);
        if (planned.is_error()) {
          co_return fatal_error(planned.move_as_error());
        }
        request = planned.move_as_ok();
      }
      batch.push_back({nb_out_msgs_->extract_cur(), request});
      nb_out_msgs_->next();
    }
    if (batch.empty()) {
      continue;
    }
    if (!(co_await virtual_shards_->run_epoch(*this, block::ParamLimits::cl_normal))) {
      co_return false;
    }
    for (auto& item : batch) {
      if (!precheck_inbound_message(item.entry->msg, item.entry->lt)) {
        co_return fatal_error("invalid virtual shard inbound message");
      }
      if (!virtual_shards_->begin_publish(item.request, *block_limit_status_)) {
        block_full_ = stopped = true;
        break;
      }
      ++stats_.neighbors.at(item.entry->source).processed_msgs;
      bool ok = process_inbound_message(item.entry->msg, item.entry->lt, item.entry->key.cbits(), item.entry->source);
      virtual_shards_->end_publish(*block_limit_status_);
      if (!ok) {
        co_return fatal_error("cannot publish virtual shard inbound message");
      }
      // Still precheck the first unprocessed message on the next iteration.
      block_full_ |= virtual_shard_budget_exhausted(block::ParamLimits::cl_normal);
    }
    if (!(co_await virtual_shards_->finish_epoch(*this, block::ParamLimits::cl_normal))) {
      co_return false;
    }
  }
  inbound_queues_empty_ = !stopped && nb_out_msgs_->is_eof();
  co_return true;
}

td::actor::Task<bool> Collator::process_virtual_messages(bool& enqueue_only) {
  SCOPE_EXIT {
    stats_.load_fraction_new_msgs = block_limit_status_->load_fraction(block::ParamLimits::cl_normal);
  };
  while (!new_msgs.empty()) {
    if (!check_cancelled()) {
      co_return false;
    }
    if (enqueue_only || block_full_ || have_unprocessed_account_dispatch_queue_ ||
        virtual_shard_budget_exhausted(block::ParamLimits::cl_normal) || internal_msg_timeout_.is_in_past()) {
      enqueue_only = true;
      co_return process_new_messages(enqueue_only);
    }
    struct Item {
      block::NewOutMsg message;
      VirtualShards::Request* request;
    };
    std::vector<Item> batch;
    SCOPE_EXIT {
      if (!virtual_shards_->requests.empty()) {
        virtual_shards_->clear_requests();
      }
    };
    {
      // Simulate queue counters so routing and deferring can share the batch
      // with local execution. The dictionaries change only during publication.
      std::map<StdSmcAddress, td::uint32> sender_generated_messages_count;
      std::map<StdSmcAddress, size_t> unprocessed_deferred_messages;
      size_t pending_dispatch = new_msgs_from_dispatch;
      td::uint64 out_queue_size = out_msg_queue_size_;
      while (!new_msgs.empty() && batch.size() < virtual_shards_->workers.size() * 32) {
        auto message = new_msgs.top();
        VirtualShards::Request* request = nullptr;
        block::gen::CommonMsgInfo::Record_int_msg_info info;
        if (tlb::unpack_cell_inexact(message.msg, info)) {
          StdSmcAddress src;
          WorkchainId wc;
          if (!block::tlb::t_MsgAddressInt.extract_std_address(info.src, wc, src) || wc != workchain()) {
            co_return fatal_error("invalid virtual shard output source");
          }
          auto pending =
              unprocessed_deferred_messages.try_emplace(src, td::get_or_default(unprocessed_deferred_messages_, src))
                  .first;
          bool defer = false;
          if (message.msg_env_from_dispatch_queue.not_null()) {
            CHECK(pending->second > 0 && pending_dispatch > 0);
            --pending->second;
            --pending_dispatch;
          } else {
            sender_generated_messages_count.try_emplace(src, td::get_or_default(sender_generated_messages_count_, src));
            defer = check_message_deferring(message, src, sender_generated_messages_count, pending->second != 0,
                                            out_queue_size, pending_dispatch) != nullptr;
          }
          if (defer) {
            ++pending->second;
          } else if (!is_our_address(info.dest)) {
            ++out_queue_size;
          } else {
            StdSmcAddress address;
            if (!block::tlb::t_MsgAddressInt.extract_std_address(info.dest, wc, address) || wc != workchain()) {
              co_return fatal_error("invalid virtual shard output destination");
            }
            auto planned = virtual_shards_->plan(*this, message.msg, address, false, message.lt);
            if (planned.is_error()) {
              co_return fatal_error(planned.move_as_error());
            }
            request = planned.move_as_ok();
            // Dispatch messages have no sender transaction in this block.
            request->in_msg_accounted = message.trans.not_null();
          }
        }
        batch.push_back({std::move(message), request});
        new_msgs.pop();
      }
      // Publication below updates the real counters for the accepted prefix.
      // These speculative counters are discarded, including any rejected tail.
    }
    // Every generated output must sort after the entire planned input batch.
    const auto horizon = batch.back().message.lt;
    for (auto& request : virtual_shards_->requests) {
      request->after_lt = std::max(request->after_lt, horizon);
    }
    for (auto& item : batch) {
      new_msgs.push(item.message);
    }
    if (!(co_await virtual_shards_->run_epoch(*this, block::ParamLimits::cl_normal))) {
      co_return false;
    }
    for (auto& item : batch) {
      if (!virtual_shards_->begin_publish(item.request, *block_limit_status_)) {
        break;
      }
      CHECK(new_msgs.top().msg->get_hash() == item.message.msg->get_hash());
      new_msgs.pop();
      if (item.message.msg_env_from_dispatch_queue.not_null()) {
        --new_msgs_from_dispatch;
      }
      --block_limit_status_->extra_out_msgs;
      int result = process_one_new_message(std::move(item.message), false);
      virtual_shards_->end_publish(*block_limit_status_);
      if (result < 0) {
        co_return fatal_error("cannot publish virtual shard output");
      }
      CHECK(item.request ? result == 1 || result == 3 : result == 0);
      if (result == 3 || virtual_shard_budget_exhausted(block::ParamLimits::cl_normal) ||
          internal_msg_timeout_.is_in_past()) {
        virtual_shards_->stopped = true;
        break;
      }
    }
    if (!(co_await virtual_shards_->finish_epoch(*this, block::ParamLimits::cl_normal))) {
      co_return false;
    }
  }
  co_return true;
}

td::actor::Task<> Collator::finalize_virtual_accounts() {
  co_await VirtualShards::run_all(*this, VirtualShards::Phase::Finish);
  vm::AugmentedDictionary merged_accounts(256, block::tlb::aug_ShardAccounts);
  vm::AugmentedDictionary merged_blocks(256, block::tlb::aug_ShardAccountBlocks);
  block_limit_status_->additional_stat.set_zero();
  for (auto& worker : virtual_shards_->workers) {
    worker->published_stat = worker->get_block_stat();
    block_limit_status_->additional_stat += worker->published_stat;
    vm::AugmentedDictionary blocks(vm::load_cell_slice_ref(worker->shard_account_blocks_), 256,
                                   block::tlb::aug_ShardAccountBlocks);
    if (!merged_accounts.combine_with(*worker->account_dict) || !merged_blocks.combine_with(blocks)) {
      co_return td::Status::Error("virtual shard dictionary merge conflict");
    }
    accounts.merge(worker->accounts);
    CHECK(worker->accounts.empty());
    stats_.work_time.prelim_storage_stat += worker->stats_.work_time.prelim_storage_stat;
    stats_.work_time.trx_tvm += worker->stats_.work_time.trx_tvm;
    stats_.work_time.trx_storage_stat += worker->stats_.work_time.trx_storage_stat;
    stats_.work_time.trx_other += worker->stats_.work_time.trx_other;
    stats_.work_time.final_storage_stat += worker->stats_.work_time.final_storage_stat;
    stats_.storage_stat_cache.hit_cnt += worker->stats_.storage_stat_cache.hit_cnt.load();
    stats_.storage_stat_cache.hit_cells += worker->stats_.storage_stat_cache.hit_cells.load();
    stats_.storage_stat_cache.miss_cnt += worker->stats_.storage_stat_cache.miss_cnt.load();
    stats_.storage_stat_cache.miss_cells += worker->stats_.storage_stat_cache.miss_cells.load();
    stats_.storage_stat_cache.small_cnt += worker->stats_.storage_stat_cache.small_cnt.load();
    stats_.storage_stat_cache.small_cells += worker->stats_.storage_stat_cache.small_cells.load();
    storage_stat_cache_update_.insert(storage_stat_cache_update_.end(), worker->storage_stat_cache_update_.begin(),
                                      worker->storage_stat_cache_update_.end());
  }
  account_dict = std::make_unique<vm::AugmentedDictionary>(std::move(merged_accounts));
  vm::CellBuilder cb;
  if (!cb.append_cellslice_bool(std::move(merged_blocks).extract_root()) || !cb.finalize_to(shard_account_blocks_)) {
    co_return td::Status::Error("cannot serialize virtual shard account blocks");
  }
  co_return td::Status::OK();
}

td::actor::Task<Ref<vm::Cell>> Collator::generate_virtual_update() {
  try {
    virtual_shards_->split_proof_roots(account_dict->get_root_cell(), 0, StdSmcAddress::zero());
  } catch (const vm::VmError& error) {
    co_return td::Status::Error(PSLICE() << "cannot split virtual proof roots: " << error.get_msg());
  }
  co_await VirtualShards::run_all(*this, VirtualShards::Phase::NewProofs);
  std::vector<vm::CellUsageTree*> trees{state_usage_tree_.get()};
  std::vector<Ref<vm::Cell>> subproofs;
  for (auto& worker : virtual_shards_->workers) {
    trees.push_back(worker->state_usage_tree_.get());
    if (worker->new_proof.not_null()) {
      subproofs.push_back(std::move(worker->new_proof));
    }
  }
  auto new_proof = CO_TRY(vm::MerkleUpdate::generate_to_raw(state_root, trees, subproofs));
  co_await VirtualShards::run_all(*this, VirtualShards::Phase::OldProofs);
  if (full_collated_data_ &&
      !account_dict->validate_prefix(virtual_shards_->prefix_bits + virtual_shards_->partition_bits)) {
    co_return td::Status::Error("cannot prepare virtual shard boundary proofs");
  }
  auto common = CO_TRY(vm::MerkleProof::generate_raw(prev_state_root_pure_, state_usage_tree_.get(), true));
  std::vector<Ref<vm::Cell>> old_proofs{std::move(common)};
  for (auto& worker : virtual_shards_->workers) {
    old_proofs.push_back(std::move(worker->old_proof));
  }
  // All proofs describe the same old root
  while (old_proofs.size() > 1) {
    std::vector<Ref<vm::Cell>> merged;
    for (size_t i = 0; i < old_proofs.size(); i += 2) {
      if (i + 1 == old_proofs.size()) {
        merged.push_back(std::move(old_proofs[i]));
      } else {
        auto proof = CO_TRY(vm::MerkleProof::combine_fast_raw(old_proofs[i], old_proofs[i + 1]));
        merged.push_back(std::move(proof));
      }
    }
    old_proofs = std::move(merged);
  }
  co_return vm::CellBuilder::create_merkle_update(old_proofs.front(), std::move(new_proof));
}

void Collator::collect_virtual_proofs(std::vector<Ref<vm::Cell>>& storage_roots) {
  for (auto& worker : virtual_shards_->workers) {
    collated_data_stat.add_loaded_cells(worker->collated_data_stat);
    worker->published_collated_data_size = 0;
    for (auto& [_, dict] : worker->account_storage_dicts_) {
      if (dict.add_to_collated_data) {
        storage_roots.push_back(dict.mpb.original_root());
      }
    }
  }
  td::unique(storage_roots, [](const auto& cell) { return cell->get_hash(); });
  block_limit_status_->collated_data_size_estimate = collated_data_stat.estimate_proof_size();
}

}  // namespace ton::validator
