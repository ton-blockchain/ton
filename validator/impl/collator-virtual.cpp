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
#include <limits>
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

struct LimitUsage {
  td::uint64 bytes = 0, gas = 0, collated_data = 0;
  LogicalTime lt = 0;

  void add(const LimitUsage& other) {
    bytes += other.bytes;
    gas += other.gas;
    collated_data += other.collated_data;
    lt = std::max(lt, other.lt);
  }

  bool fits(const LimitUsage& budget) const {
    return bytes < budget.bytes && gas < budget.gas && collated_data < budget.collated_data && lt < budget.lt;
  }
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
    LogicalTime after_lt = 0;
    unsigned owner = 0;
    size_t position = 0;
    LimitUsage usage;
    block::Account* account = nullptr;
    Ref<vm::Cell> new_state;
    std::unique_ptr<block::transaction::Transaction> transaction;
    td::Status status;

    struct Checkpoint {
      std::unique_ptr<block::Account::Checkpoint> account;
      ReadJournal::Checkpoint state_reads;
      size_t cache_updates_size = 0;
      td::optional<td::Bits256> storage;
      td::optional<ReadJournal::Checkpoint> storage_reads;
    } checkpoint;
  };

  struct Worker : CollatorBase {
    StdSmcAddress prefix = StdSmcAddress::zero();
    unsigned prefix_bits = 0;
    Ref<vm::Cell> prev_state_root_pure_, prev_state_root_;
    block::BlockLimits block_limits_;
    block::WorkchainSet workchains_;
    td::CancellationToken cancellation_token_;
    td::Timestamp timeout_;

    std::vector<Request*> pending;
    size_t prepared = 0, selected = 0;
    bool recording = false;
    LimitUsage usage;
    double cpu = 0;
    std::vector<Ref<vm::Cell>> proof_roots, new_proofs;
    Ref<vm::Cell> old_proof;
    ReadJournal state_reads;
    std::map<td::Bits256, ReadJournal> storage_reads;

    explicit Worker(ShardIdFull shard);
    td::Status init(Collator& parent, unsigned index, unsigned partition_bits);
    td::Result<block::Account*> make_account(const StdSmcAddress& address);
    td::Status create_ordinary_transaction(Request& request);
    void prepare(const LimitUsage& budget);
    void begin_epoch();
    void save_checkpoint(Request& request);
    void finish_epoch();
    td::Status commit();
    td::Status generate_new_proofs();
    td::Status generate_old_proof();
  };

  std::vector<std::unique_ptr<Worker>> workers;
  unsigned prefix_bits = 0, partition_bits = 0;
  td::uint64 published_proof_estimate = 0;
  bool stopped = false;
  LimitUsage budget;
  std::vector<std::unique_ptr<Request>> requests;
  Request* publishing = nullptr;

  unsigned owner(const StdSmcAddress& address) const;
  void sync_limits(block::BlockLimitStatus& common);
  td::Result<Request*> plan(Collator& parent, Ref<vm::Cell> message, const StdSmcAddress& address, bool external,
                            LogicalTime after_lt, size_t position);
  void prepare(Collator& parent, unsigned limit_class);
  size_t select();
  enum class Phase { Prepare, Commit, Finish, NewProofs, OldProofs };
  static td::actor::Task<> run(std::shared_ptr<VirtualShards> state, unsigned i, Phase phase);
  static td::actor::Task<> run_all(Collator& parent, Phase phase);
  void split_proof_roots(Ref<vm::Cell> root, int depth, StdSmcAddress prefix);
  void finish_epoch();
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
  prev_state_root_ = vm::UsageCell::create(prev_state_root_pure_, state_usage_tree_->root_ptr());
  block::gen::ShardStateUnsplit::Record state;
  if (!tlb::unpack_cell(prev_state_root_, state)) {
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
  TRY_RESULT_ASSIGN(request.account, make_account(request.address));
  TRY_RESULT_ASSIGN(request.transaction, CollatorBase::create_ordinary_transaction(request.message, request.account,
                                                                                   request.external, request.after_lt));
  return td::Status::OK();
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
    auto& checkpoint = pending[prepared]->checkpoint;
    if (!checkpoint.storage) {
      checkpoint.storage = it->first;
      if (!created) {
        checkpoint.storage_reads = journal.checkpoint();
      }
    }
    CHECK(checkpoint.storage == it->first);
  };
}

void Collator::VirtualShards::Worker::save_checkpoint(Request& request) {
  auto& checkpoint = request.checkpoint;
  if (auto* account = lookup_account(request.address.cbits())) {
    checkpoint.account = std::make_unique<block::Account::Checkpoint>(account->checkpoint());
  }
  checkpoint.state_reads = state_reads.checkpoint();
  checkpoint.cache_updates_size = storage_stat_cache_update_.size();
}

void Collator::VirtualShards::Worker::prepare(const LimitUsage& budget) {
  CHECK(!recording);
  prepared = selected = 0;
  usage = {};
  if (pending.empty()) {
    return;
  }
  begin_epoch();
  vm::NewCellStorageStat st_stat(&block_limit_status_->st_stat);
  vm::MerkleUpdateLeftStat merkle_update_left_stat(&block_limit_status_->merkle_update_left_stat);
  for (auto* request : pending) {
    if (timeout_.is_in_past()) {
      break;
    }
    save_checkpoint(*request);
    auto before_collated_size = block_limit_status_->collated_data_size_estimate;
    request->status = create_ordinary_transaction(*request);
    if (request->status.is_ok()) {
      auto& trans = *request->transaction;
      if (request->in_msg_accounted) {
        st_stat.mark_subtree_accounted(request->message->get_hash());
      }
      auto before = st_stat.get_total_stat();
      auto before_left_size = merkle_update_left_stat.estimate_proof_size();
      request->status = trans.add_block_storage_profile(st_stat, state_usage_tree_.get(), &merkle_update_left_stat);
      if (request->status.is_ok()) {
        auto extra = st_stat.get_total_stat();
        request->usage.bytes = block_limit_status_->estimate_block_size(&extra) -
                               block_limit_status_->estimate_block_size(&before) + 200 + trans.out_msgs.size() * 300 +
                               (request->external ? 160 : 320);
        auto left_size = merkle_update_left_stat.estimate_proof_size();
        request->usage.bytes += left_size > before_left_size ? left_size - before_left_size : 0;
        request->usage.gas = trans.gas_used();
        request->usage.lt = trans.end_lt;
        request->new_state = trans.new_total_state;
        if (trans.commit(*request->account).is_null()) {
          request->status = td::Status::Error("cannot prepare virtual shard account state");
        }
      }
    }
    auto collated_size = block_limit_status_->collated_data_size_estimate;
    request->usage.collated_data = collated_size > before_collated_size ? collated_size - before_collated_size : 0;
    usage.add(request->usage);
    ++prepared;
    if (request->status.is_error() && request->status.error().code() != -701) {
      break;
    }
    if (!usage.fits(budget)) {
      break;
    }
  }
  current_tx_storage_dict_ = nullptr;
}

void Collator::VirtualShards::Worker::finish_epoch() {
  CHECK(recording && selected <= prepared);
  record_storage_dict_ = {};
  current_tx_storage_dict_ = nullptr;
  if (selected < prepared) {
    std::map<StdSmcAddress, std::unique_ptr<block::Account::Checkpoint>> account_checkpoints;
    std::map<td::Bits256, td::optional<ReadJournal::Checkpoint>> storage_checkpoints;
    for (size_t i = selected; i < prepared; ++i) {
      auto& request = *pending[i];
      auto& checkpoint = request.checkpoint;
      account_checkpoints.try_emplace(request.address, std::move(checkpoint.account));
      checkpoint.account.reset();
      if (checkpoint.storage) {
        storage_checkpoints.try_emplace(checkpoint.storage.unwrap(), std::move(checkpoint.storage_reads));
      }
      request.transaction.reset();
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
      if (checkpoint) {
        it->second.rollback(*checkpoint);
      } else {
        it->second.commit_epoch();
        storage_reads.erase(it);
        account_storage_dicts_.erase(hash);
      }
    }
    const auto& first = pending[selected]->checkpoint;
    state_reads.rollback(first.state_reads);
    storage_stat_cache_update_.resize(first.cache_updates_size);
  }
  for (auto& [hash, journal] : storage_reads) {
    journal.commit_epoch();
  }
  state_reads.commit_epoch();
  if (selected < prepared) {
    block_limit_status_->collated_data_size_estimate = collated_data_stat.estimate_proof_size();
    for (auto& [hash, dict] : account_storage_dicts_) {
      block_limit_status_->collated_data_size_estimate += dict.proof_stat.estimate_proof_size();
    }
  }
  storage_reads.clear();
  recording = false;
}

td::Status Collator::VirtualShards::Worker::commit() {
  if (!recording) {
    return td::Status::OK();
  }
  finish_epoch();
  for (size_t i = 0; i < selected; ++i) {
    auto& request = *pending[i];
    request.checkpoint.account.reset();
    if (request.status.is_ok()) {
      auto& trans = *request.transaction;
      if (request.in_msg_accounted) {
        block_limit_status_->st_stat.mark_subtree_accounted(request.message->get_hash());
      }
      if (!(block_limit_status_->update_lt(request.usage.lt) && block_limit_status_->update_gas(request.usage.gas) &&
            block_limit_status_->add_proof(request.new_state) && block_limit_status_->add_cell(trans.root) &&
            block_limit_status_->add_transaction() && block_limit_status_->add_account(trans.is_first))) {
        return td::Status::Error("cannot update virtual shard transaction limits");
      }
      if (!update_account_dict(trans)) {
        return td::Status::Error("cannot update virtual shard account dictionary");
      }
      update_account_storage_dict_info(trans);
    }
  }
  return td::Status::OK();
}

td::Status Collator::VirtualShards::Worker::generate_new_proofs() {
  vm::CellUsageTree* tree = state_usage_tree_.get();
  for (auto& root : proof_roots) {
    TRY_RESULT(proof, vm::MerkleUpdate::generate_to_raw(root, tree));
    new_proofs.push_back(std::move(proof));
  }
  return td::Status::OK();
}

td::Status Collator::VirtualShards::Worker::generate_old_proof() {
  TRY_RESULT_ASSIGN(old_proof, vm::MerkleProof::generate_raw(prev_state_root_pure_, state_usage_tree_.get(), true));
  // Finish the reads needed by validation after generating the Merkle update proof.
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

void Collator::VirtualShards::sync_limits(block::BlockLimitStatus& common) {
  common.additional_stat.set_zero();
  CHECK(common.collated_data_size_estimate >= published_proof_estimate);
  common.collated_data_size_estimate -= published_proof_estimate;
  published_proof_estimate = 0;
  for (auto& worker : workers) {
    CHECK(!worker->recording);
    common.additional_stat += worker->block_limit_status_->st_stat.get_total_stat();
    common.additional_stat.bits += worker->block_limit_status_->merkle_update_left_stat.estimate_proof_size() * 8;
    published_proof_estimate += worker->block_limit_status_->collated_data_size_estimate;
  }
  common.collated_data_size_estimate += published_proof_estimate;
}

td::Result<Collator::VirtualShards::Request*> Collator::VirtualShards::plan(Collator& parent, Ref<vm::Cell> message,
                                                                            const StdSmcAddress& address, bool external,
                                                                            LogicalTime after_lt, size_t position) {
  auto request = std::make_unique<Request>();
  request->owner = owner(address);
  request->position = position;
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

void Collator::VirtualShards::prepare(Collator& parent, unsigned limit_class) {
  const auto& common = *parent.block_limit_status_;
  CHECK(limit_class < block::ParamLimits::limits_cnt);
  auto remaining = [limit_class](const block::ParamLimits& limits, td::uint64 used) {
    auto limit = limits.limit(limit_class);
    return used < limit ? (limit - used) * 7 / 8 : 0;
  };
  // Each worker gets the same 7/8 of the remaining physical-shard budget
  budget = {remaining(common.limits.bytes, common.estimate_block_size()), remaining(common.limits.gas, common.gas_used),
            remaining(common.limits.collated_data, common.collated_data_size_estimate),
            common.limits.start_lt + common.limits.lt_delta.limit(limit_class)};
  for (auto& worker : workers) {
    worker->prepared = worker->selected = 0;
    worker->usage = {};
    worker->timeout_ =
        limit_class == block::ParamLimits::cl_soft ? parent.external_msg_timeout_ : parent.internal_msg_timeout_;
  }
}

size_t Collator::VirtualShards::select() {
  LimitUsage total;
  bool complete = true;
  for (auto& worker : workers) {
    total.add(worker->usage);
    complete &= worker->prepared == worker->pending.size();
  }
  if (complete && total.fits(budget)) {
    for (auto& worker : workers) {
      worker->selected = worker->prepared;
    }
    return std::numeric_limits<size_t>::max();
  }
  total = {};
  for (auto& request : requests) {
    auto& worker = *workers[request->owner];
    if (worker.selected == worker.prepared) {
      stopped = true;
      return request->position;
    }
    ++worker.selected;
    total.add(request->usage);
    if (!total.fits(budget)) {
      stopped = true;
      return request->position + 1;  // Include the first crossing, discard only its suffix
    }
  }
  return std::numeric_limits<size_t>::max();
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
      case Phase::Prepare:
        worker.prepare(state->budget);
        co_return {};
      case Phase::Commit:
        co_return worker.commit();
      case Phase::Finish:
        co_return worker.combine_account_transactions();
      case Phase::NewProofs:
        co_return worker.generate_new_proofs();
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
    if ((phase != Phase::Prepare && phase != Phase::Commit) || !state->workers[i]->pending.empty()) {
      jobs.push_back(run(state, i, phase).start());
    }
  }
  auto results = co_await td::actor::all_wrap(std::move(jobs));
  for (auto& result : results) {
    CO_TRY(std::move(result));
  }
  if (phase == Phase::Commit) {
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
    workers[owner(prefix)]->proof_roots.push_back(std::move(root));
    return;
  }
  for (int branch = 0; branch < 2; ++branch) {
    (prefix.bits() + depth).store_uint(branch, 1);
    split_proof_roots(label.remainder->prefetch_ref(branch), depth + 1, prefix);
  }
}

void Collator::VirtualShards::finish_epoch() {
  for (auto& worker : workers) {
    if (worker->recording) {
      worker->selected = 0;
      worker->finish_epoch();
    }
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
  virtual_shards_->sync_limits(*block_limit_status_);
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
  // The committed worker statistics already include these subtrees. Shared
  // descriptors only need their own cells and references to these roots.
  auto& stat = block_limit_status_->st_stat;
  stat.mark_subtree_accounted(trans.root->get_hash());
  stat.mark_subtree_accounted(msg_root->get_hash());
  for (const auto& message : trans.out_msgs) {
    stat.mark_subtree_accounted(message->get_hash());
  }
  if (!(block_limit_status_->update_gas(request.usage.gas) && block_limit_status_->update_lt(request.usage.lt) &&
        block_limit_status_->add_transaction() && block_limit_status_->add_account(trans.is_first))) {
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
      virtual_shards_->finish_epoch();
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
        auto planned = virtual_shards_->plan(*this, message->root_cell(), message->addr(), true, 0, batch.size());
        if (planned.is_error()) {
          co_return fatal_error(planned.move_as_error());
        }
        request = planned.move_as_ok();
        if (!request) {
          pending_ext_msg_ = std::move(item);
          break;
        }
      }
      ++stats_.ext_msgs_total;
      batch_hashes.insert(message->hash_norm());
      batch.push_back({std::move(message), request});
    }
    if (batch.empty()) {
      continue;
    }
    virtual_shards_->prepare(*this, block::ParamLimits::cl_soft);
    auto prepared = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Prepare).wrap();
    if (prepared.is_error()) {
      co_return fatal_error(prepared.move_as_error());
    }
    auto accepted = std::min(batch.size(), virtual_shards_->select());
    auto committed = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Commit).wrap();
    if (committed.is_error()) {
      co_return fatal_error(committed.move_as_error());
    }
    for (size_t i = 0; i < accepted; ++i) {
      auto& item = batch[i];
      processed_external_messages_ = processed_external_messages_.insert(item.message->hash_norm(), td::Unit{});
      if (!item.request) {
        ++stats_.ext_msgs_filtered;
        bad_ext_msgs_.push_back(item.message->hash_norm());
        continue;
      }
      virtual_shards_->publishing = item.request;
      int result = process_external_message(item.message->root_cell());
      virtual_shards_->publishing = nullptr;
      if (result > 0) {
        ++stats_.ext_msgs_accepted;
      } else {
        ++stats_.ext_msgs_rejected;
        bad_ext_msgs_.push_back(item.message->hash_norm());
      }
      if (result < 0) {
        co_return false;
      }
    }
    block_full_ |= virtual_shard_budget_exhausted(block::ParamLimits::cl_soft);
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
      virtual_shards_->finish_epoch();
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
        auto planned = virtual_shards_->plan(*this, envelope.msg, address, false, 0, batch.size());
        if (planned.is_error()) {
          co_return fatal_error(planned.move_as_error());
        }
        request = planned.move_as_ok();
        if (!request) {
          break;
        }
      }
      // Transit/cleanup changes common queues. Publish those as their own
      // epoch, so the next execution batch starts with an updated estimate.
      if (!request && !batch.empty()) {
        break;
      }
      batch.push_back({nb_out_msgs_->extract_cur(), request});
      nb_out_msgs_->next();
      if (!request) {
        break;
      }
    }
    if (batch.empty()) {
      continue;
    }
    virtual_shards_->prepare(*this, block::ParamLimits::cl_normal);
    auto prepared = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Prepare).wrap();
    if (prepared.is_error()) {
      co_return fatal_error(prepared.move_as_error());
    }
    auto accepted = std::min(batch.size(), virtual_shards_->select());
    auto committed = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Commit).wrap();
    if (committed.is_error()) {
      co_return fatal_error(committed.move_as_error());
    }
    for (size_t i = 0; i < batch.size(); ++i) {
      auto& item = batch[i];
      if (!precheck_inbound_message(item.entry->msg, item.entry->lt)) {
        co_return fatal_error("invalid virtual shard inbound message");
      }
      if (i == accepted) {
        block_full_ = stopped = true;
        break;
      }
      virtual_shards_->publishing = item.request;
      ++stats_.neighbors.at(item.entry->source).processed_msgs;
      bool ok = process_inbound_message(item.entry->msg, item.entry->lt, item.entry->key.cbits(), item.entry->source);
      virtual_shards_->publishing = nullptr;
      if (!ok) {
        co_return fatal_error("cannot publish virtual shard inbound message");
      }
    }
    block_full_ |= virtual_shard_budget_exhausted(block::ParamLimits::cl_normal);
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
    std::vector<block::NewOutMsg> messages;
    SCOPE_EXIT {
      if (!virtual_shards_->requests.empty()) {
        virtual_shards_->finish_epoch();
      }
    };
    {
      // Simulate only counters of touched senders. Local execution does not
      // change the queue dictionaries; stop the epoch before any routing does.
      std::map<StdSmcAddress, td::uint32> sender_generated_messages_count;
      std::map<StdSmcAddress, size_t> unprocessed_deferred_messages;
      size_t pending_dispatch = new_msgs_from_dispatch;
      while (!new_msgs.empty() && messages.size() < virtual_shards_->workers.size() * 32) {
        auto message = new_msgs.top();
        block::gen::CommonMsgInfo::Record_int_msg_info info;
        StdSmcAddress address, src;
        WorkchainId wc;
        if (!tlb::unpack_cell_inexact(message.msg, info) || !is_our_address(info.dest) ||
            !block::tlb::t_MsgAddressInt.extract_std_address(info.dest, wc, address) ||
            !block::tlb::t_MsgAddressInt.extract_std_address(info.src, wc, src) || wc != workchain()) {
          break;
        }
        auto pending =
            unprocessed_deferred_messages.try_emplace(src, td::get_or_default(unprocessed_deferred_messages_, src))
                .first;
        bool from_dispatch_queue = message.msg_env_from_dispatch_queue.not_null();
        if (!from_dispatch_queue) {
          sender_generated_messages_count.try_emplace(src, td::get_or_default(sender_generated_messages_count_, src));
          if (check_message_deferring(message, src, sender_generated_messages_count, pending->second != 0,
                                      pending_dispatch)) {
            break;
          }
        }
        auto planned = virtual_shards_->plan(*this, message.msg, address, false, message.lt, messages.size());
        if (planned.is_error()) {
          co_return fatal_error(planned.move_as_error());
        }
        if (!planned.ok()) {
          break;
        }
        // Only outputs of accepted transactions have trans; dispatch messages
        // have no sender transaction in this block and must still be counted.
        planned.ok()->in_msg_accounted = message.trans.not_null();
        if (from_dispatch_queue) {
          CHECK(pending->second > 0 && pending_dispatch > 0);
          --pending->second;
          --pending_dispatch;
        }
        messages.push_back(std::move(message));
        new_msgs.pop();
      }
      // Publication below updates the real counters for the accepted prefix.
      // These speculative counters are discarded, including any rejected tail.
    }
    if (messages.empty()) {
      auto message = new_msgs.top();
      new_msgs.pop();
      if (message.msg_env_from_dispatch_queue.not_null()) {
        --new_msgs_from_dispatch;
      }
      --block_limit_status_->extra_out_msgs;
      if (process_one_new_message(std::move(message), false) < 0) {
        co_return fatal_error("cannot route virtual shard output");
      }
      continue;
    }
    // Every generated output must sort after the entire planned input batch.
    const auto horizon = messages.back().lt;
    for (auto& request : virtual_shards_->requests) {
      request->after_lt = std::max(request->after_lt, horizon);
    }
    for (auto& message : messages) {
      new_msgs.push(message);
    }
    virtual_shards_->prepare(*this, block::ParamLimits::cl_normal);
    auto prepared = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Prepare).wrap();
    if (prepared.is_error()) {
      co_return fatal_error(prepared.move_as_error());
    }
    auto accepted = std::min(messages.size(), virtual_shards_->select());
    auto committed = co_await VirtualShards::run_all(*this, VirtualShards::Phase::Commit).wrap();
    if (committed.is_error()) {
      co_return fatal_error(committed.move_as_error());
    }
    for (size_t i = 0; i < accepted; ++i) {
      CHECK(new_msgs.top().msg->get_hash() == messages[i].msg->get_hash());
      new_msgs.pop();
      if (messages[i].msg_env_from_dispatch_queue.not_null()) {
        --new_msgs_from_dispatch;
      }
      --block_limit_status_->extra_out_msgs;
      virtual_shards_->publishing = virtual_shards_->requests[i].get();
      virtual_batch_publishing_ = true;
      int result = process_one_new_message(std::move(messages[i]), false);
      virtual_batch_publishing_ = false;
      virtual_shards_->publishing = nullptr;
      if (result < 0) {
        co_return fatal_error("cannot publish virtual shard output");
      }
      CHECK(result == 1);
    }
    block_full_ |= virtual_shard_budget_exhausted(block::ParamLimits::cl_normal);
  }
  co_return true;
}

td::actor::Task<> Collator::finalize_virtual_accounts() {
  co_await VirtualShards::run_all(*this, VirtualShards::Phase::Finish);
  vm::AugmentedDictionary merged_accounts(256, block::tlb::aug_ShardAccounts);
  vm::AugmentedDictionary merged_blocks(256, block::tlb::aug_ShardAccountBlocks);
  // Keep the workers' proof estimates instead of rescanning the merged state in create_shard_state().
  block_limit_status_->additional_stat.set_zero();
  for (auto& worker : virtual_shards_->workers) {
    block_limit_status_->additional_stat += worker->block_limit_status_->st_stat.get_total_stat();
    block_limit_status_->additional_stat.bits +=
        worker->block_limit_status_->merkle_update_left_stat.estimate_proof_size() * 8;
    vm::AugmentedDictionary blocks(vm::load_cell_slice_ref(worker->shard_account_blocks_), 256,
                                   block::tlb::aug_ShardAccountBlocks);
    if (!merged_accounts.combine_with(*worker->account_dict) || !merged_blocks.combine_with(blocks)) {
      co_return td::Status::Error("virtual shard dictionary merge conflict");
    }
    accounts.merge(worker->accounts);
    CHECK(worker->accounts.empty());
    for (auto& [hash, source] : worker->account_storage_dicts_) {
      auto [it, inserted] = account_storage_dicts_.try_emplace(hash);
      auto& dest = it->second;
      if (inserted) {
        dest.mpb = source.mpb;
        dest.inited = source.inited;
      }
      dest.proof_stat.add_loaded_cells(source.proof_stat);
      dest.add_to_collated_data |= source.add_to_collated_data;
    }
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
    subproofs.insert(subproofs.end(), worker->new_proofs.begin(), worker->new_proofs.end());
  }
  auto new_proof = CO_TRY(vm::MerkleUpdate::generate_to_raw(state_root, trees, subproofs));
  co_await VirtualShards::run_all(*this, VirtualShards::Phase::OldProofs);
  // Local diffs miss relabelled boundary roots when a neighboring range becomes empty.
  // Workers have read the old prefix paths during init; check the new top while they are idle.
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

void Collator::collect_virtual_proofs() {
  for (auto& worker : virtual_shards_->workers) {
    collated_data_stat.add_loaded_cells(worker->collated_data_stat);
  }
  block_limit_status_->collated_data_size_estimate = collated_data_stat.estimate_proof_size();
  virtual_shards_->published_proof_estimate = 0;
}

}  // namespace ton::validator
