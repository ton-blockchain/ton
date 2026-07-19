/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/db/KeyValueAsync.h"
#include "td/db/RocksDb.h"
#include "td/utils/port/path.h"
#include "ton/ton-io.hpp"
#include "validator/consensus/simplex/bus.h"
#include "validator/fabric.h"
#include "validator/full-node.h"
#include "validator/validator-group.hpp"

namespace ton::validator {

namespace consensus {

namespace tl {

using dbId = ton_api::consensus_dbId;

}

namespace {

class ManagerFacadeImpl : public ManagerFacade {
 public:
  ManagerFacadeImpl(td::actor::ActorId<ValidatorManager> manager, td::Ref<block::ValidatorSet> validator_set,
                    td::Ref<ValidatorManagerOptions> opts)
      : manager_(manager), validator_set_(std::move(validator_set)), opts_(std::move(opts)) {
  }

  td::actor::Task<GeneratedCandidate> collate_block(CollateParams params,
                                                    td::CancellationToken cancellation_token) override {
    params.validator_set = validator_set_;
    params.collator_opts = opts_->get_collator_options();
    // TODO: support accelerator (use CollationManager)
    auto [task, promise] = td::actor::StartedTask<BlockCandidate>::make_bridge();
    run_collate_query(std::move(params), manager_, std::move(cancellation_token), std::move(promise));
    auto candidate = co_await std::move(task);
    co_return GeneratedCandidate{.candidate = std::move(candidate), .self_collated = true};
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate, ValidateParams params,
                                                                    td::Timestamp timeout) override {
    params.validator_set = validator_set_;
    params.parallel_validation = opts_->get_parallel_validation();
    auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
    run_validate_query(std::move(candidate), std::move(params), manager_, timeout, std::move(promise));
    co_return co_await std::move(task);
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, size_t creator_idx,
                                 td::Ref<block::BlockSignatureSet> signatures, int block_broadcast_mode,
                                 int finality_broadcast_mode, bool send_shard_block_desc, bool apply) override {
    while (true) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      run_accept_block_query(id, data, {}, validator_set_, signatures, block_broadcast_mode, finality_broadcast_mode,
                             send_shard_block_desc, apply, manager_, std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_ok() || result.error().code() == ErrorCode::cancelled) {
        break;
      }
      LOG_CHECK(result.error().code() == ErrorCode::timeout || result.error().code() == ErrorCode::notready)
          << "Failed to accept finalized block " << id << " : " << result.error();
      LOG(WARNING) << "Failed to accept finalized block " << id << ", retrying : " << result.error();
      block_broadcast_mode = 0;
      finality_broadcast_mode = 0;
      co_await td::actor::coro_sleep(td::Timestamp::in(1.0));
    }
    co_return {};
  }

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id, td::Timestamp timeout) override {
    auto state =
        co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state_short, block_id, 0, timeout, false);
    co_return state->root_cell();
  }

  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id, td::Timestamp timeout) override {
    co_return co_await td::actor::ask(manager_, &ValidatorManager::wait_block_data_short, block_id, 0, timeout);
  }

  void cache_block_candidate(BlockCandidate candidate) override {
    td::actor::send_closure(manager_, &ValidatorManager::new_block_candidate_broadcast, candidate.id,
                            validator_set_->get_catchain_seqno(), candidate.data.clone(),
                            BroadcastSource::consensus_overlay, [](td::Result<>) {});
  }

  void send_block_candidate_broadcast(BlockIdExt id, td::BufferSlice data, int mode) override {
    td::actor::send_closure(manager_, &ValidatorManager::send_block_candidate_broadcast, id,
                            validator_set_->get_catchain_seqno(), validator_set_->get_validator_set_hash(),
                            std::move(data), mode);
  }

  void update_collator_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<block::ValidatorSet> validator_set_;
  td::Ref<ValidatorManagerOptions> opts_;
};

class DbImpl : public Db {
 public:
  explicit DbImpl(std::string path) {
    td::mkpath(path).ensure();
    auto rocksdb = td::RocksDb::open(path).ensure().move_as_ok();
    reader_ = rocksdb.snapshot();
    writer_ = td::KeyValueAsync<td::BufferSlice, td::BufferSlice>(std::make_shared<td::RocksDb>(std::move(rocksdb)));
  }

  std::optional<td::BufferSlice> get(td::Slice key) const override {
    std::string value;
    auto result = reader_->get(key, value).ensure().move_as_ok();
    if (result == td::KeyValueReader::GetStatus::Ok) {
      return td::BufferSlice(value);
    }
    return std::nullopt;
  }
  std::vector<std::pair<td::BufferSlice, td::BufferSlice>> get_by_prefix(td::uint32 prefix) const override {
    td::uint32 prefix2 = td::bswap32(td::bswap32(prefix) + 1);
    td::Slice begin{(const char*)&prefix, 4};
    td::Slice end{(const char*)&prefix2, 4};
    std::vector<std::pair<td::BufferSlice, td::BufferSlice>> result;
    reader_
        ->for_each_in_range(begin, end,
                            [&](td::Slice key, td::Slice value) -> td::Status {
                              result.emplace_back(key, value);
                              return td::Status::OK();
                            })
        .ensure();
    return result;
  }
  td::actor::Task<> set(td::BufferSlice key, td::BufferSlice value) override {
    auto result = co_await writer_.set(std::move(key), std::move(value)).wrap();
    if (result.is_error() && result.error().code() != ErrorCode::cancelled) {
      result.ensure();
    }
    co_return std::move(result);
  }
  td::actor::Task<> close() override {
    co_return co_await writer_.close();
  }

 private:
  td::KeyValueAsync<td::BufferSlice, td::BufferSlice> writer_;
  std::unique_ptr<td::KeyValueReader> reader_;
};

class CandidateBroadcastRelay : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateReceived> event) {
    if (!bus->config.enable_plumtree_broadcast()) {
      return;
    }
    if (event->candidate->is_empty()) {
      return;
    }

    int mode = fullnode::FullNode::broadcast_mode_custom | fullnode::FullNode::broadcast_mode_fast_sync |
               fullnode::FullNode::broadcast_mode_public;
    const auto& block = std::get<BlockCandidate>(event->candidate->block);
    td::actor::send_closure(bus->manager, &ManagerFacade::send_block_candidate_broadcast, block.id, block.data.clone(),
                            mode);
  }
};

class BridgeImpl final : public IValidatorGroup {
 public:
  BridgeImpl(std::string name, GroupParams&& params) : name_(name), params_(std::move(params)) {
  }

  virtual void start(std::vector<BlockIdExt> blocks, BlockIdExt min_mc_block_id) override {
    CHECK(!is_start_called_);
    is_start_called_ = true;
    resolve_state_and_start(blocks, min_mc_block_id).start().detach();
  }

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) override {
    if (!apply_blocks) {
      LOG(WARNING) << "Accelerator is not consistently supported with simplex consensus";
    }
    td::actor::send_closure(manager_facade_, &ManagerFacadeImpl::update_collator_options, opts);

    auto new_noncritical_params =
        opts->get_noncritical_params(bus_->shard, bus_->cc_seqno, params_.config.noncritical_params);
    if (current_noncritical_params_ != new_noncritical_params) {
      bus_.publish<NoncriticalParamsUpdated>(new_noncritical_params);
      current_noncritical_params_ = new_noncritical_params;
    }
  }

  virtual void notify_mc_finalized(BlockIdExt block) override {
    CHECK(params_.shard == block.shard_full());
    if (bus_) {
      bus_.publish<BlockFinalizedInMasterchain>(block);
    }
  }

  void destroy() override {
    destroy_inner().start().detach();
  }

  void start_up() override {
    manager_facade_ = td::actor::create_actor<ManagerFacadeImpl>(name_ + ".ManagerFacade", params_.manager,
                                                                 params_.validator_set, params_.validator_opts);

    auto bus = std::make_shared<simplex::Bus>();

    bus->shard = params_.shard;
    bus->manager = manager_facade_.get();
    bus->keyring = params_.keyring;
    bus->validator_opts = params_.validator_opts;
    bus->all_validators = params_.all_validators;

    bool found = false;
    size_t idx = 0;
    ValidatorWeight total_weight = 0;
    for (const auto& el : params_.validator_set->export_vector()) {
      PublicKey key{pubkeys::Ed25519{el.key}};
      PublicKeyHash short_id = key.compute_short_id();

      bus->validator_set.push_back(PeerValidator{
          .idx = PeerValidatorId{idx},
          .key = key,
          .short_id = short_id,
          .adnl_id = adnl::AdnlNodeIdShort{el.addr.is_zero() ? short_id.bits256_value() : el.addr},
          .weight = el.weight,
      });

      if (short_id == params_.identity.short_id) {
        CHECK(!found);
        CHECK(bus->validator_set.back().adnl_id == params_.identity.adnl_id);
        found = true;
        bus->local_id = bus->validator_set.back();
      }

      total_weight += el.weight;
      ++idx;
    }
    bus->total_weight = total_weight;
    bus->cc_seqno = params_.validator_set->get_catchain_seqno();
    bus->validator_set_hash = params_.validator_set->get_validator_set_hash();
    CHECK(!params_.identity.short_id.has_value() || found);
    bus->local_adnl_id = params_.identity.adnl_id;

    bus->config = params_.config;
    bus->config.noncritical_params =
        params_.validator_opts->get_noncritical_params(bus->shard, bus->cc_seqno, bus->config.noncritical_params);
    current_noncritical_params_ = bus->config.noncritical_params;

    bus->session_id = params_.session_id;
    bus->overlays = params_.overlays;
    bus->adnl_sender = params_.adnl_sender;

    auto [stop_waiter, stop_promise] = td::actor::StartedTask<>::make_bridge();
    stop_waiter_ = std::move(stop_waiter);
    bus->stop_promise = std::move(stop_promise);

    td::actor::Runtime runtime;

    bus->db = std::make_unique<DbImpl>(db_path());

    BlockAccepter::register_in(runtime);
    BlockProducer::register_in(runtime);
    runtime.register_actor<CandidateBroadcastRelay>("CandidateBroadcastRelay");
    BlockValidator::register_in(runtime);
    PrivateOverlay::register_in(runtime);
    TraceCollector::register_in(runtime);
    simplex::CandidateResolver::register_in(runtime);
    simplex::Consensus::register_in(runtime);
    simplex::Db::register_in(runtime);
    simplex::Pool::register_in(runtime);
    simplex::StateResolver::register_in(runtime);

    simplex::DefaultCollatorSchedule::provide_for(runtime);

    bus_ = runtime.start(bus, name_);
  }

 private:
  td::actor::Task<> destroy_inner() {
    if (bus_) {
      LOG(INFO) << "Destroying validator group";
      bus_.publish<StopRequested>();
      bool had_db = static_cast<bool>(bus_->db);
      if (had_db) {
        co_await bus_->db->close();
      }
      bus_ = {};
      co_await std::move(stop_waiter_.value());
      LOG(INFO) << "Consensus bus stopped";
      if (had_db) {
        auto path = db_path();
        auto S = td::RocksDb::destroy(path);
        td::rmrf(path).ignore();

        if (S.is_ok()) {
          LOG(INFO) << "Deleting consensus DB : done";
        } else {
          LOG(ERROR) << "Deleting consensus DB " << db_path() << " : " << S;
        }
      }
    }
    stop();
    co_return td::Unit{};
  }

  td::actor::Task<> resolve_state_and_start(std::vector<BlockIdExt> blocks, BlockIdExt min_mc_block_id) {
    auto state = co_await ChainState::from_manager(manager_facade_.get(), params_.shard, blocks, min_mc_block_id);
    start_event_ = std::make_shared<Start>(state);
    bus_.publish(start_event_);
    co_return {};
  }

  bool is_start_called_ = false;

  std::string name_;
  GroupParams params_;
  td::actor::ActorOwn<ManagerFacadeImpl> manager_facade_;

  BusHandle bus_;
  td::optional<td::actor::StartedTask<>> stop_waiter_;

  std::shared_ptr<Start> start_event_;

  NewConsensusConfig::NoncriticalParams current_noncritical_params_;

  std::string db_path() const {
    td::StringBuilder sb;
    auto hash =
        create_hash_tl_object<tl::dbId>(params_.session_id, params_.identity.is_validator(),
                                        params_.identity.short_id.value_or(PublicKeyHash::zero()).bits256_value(),
                                        params_.identity.adnl_id.bits256_value());
    sb << params_.db_root << "/consensus/" << params_.shard.workchain << "." << params_.shard.shard << "."
       << params_.validator_set->get_catchain_seqno() << "." << hash.to_hex();
    return sb.as_cslice().str();
  }
};

}  // namespace
}  // namespace consensus

td::actor::ActorOwn<IValidatorGroup> IValidatorGroup::create_bridge(td::Slice name, GroupParams params) {
  auto name_with_seqno =
      std::string(name.begin(), name.end()) + "." + std::to_string(params.validator_set->get_catchain_seqno());
  return td::actor::create_actor<consensus::BridgeImpl>(name, name_with_seqno, std::move(params));
}

}  // namespace ton::validator
