/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "quic/quic-sender.h"
#include "td/db/RocksDb.h"
#include "td/utils/port/path.h"
#include "validator/consensus/null/bus.h"
#include "validator/consensus/simplex/bus.h"
#include "validator/fabric.h"
#include "validator/validator-group.hpp"

#include "runtime.h"

namespace ton::validator {

namespace consensus {
namespace {

class ManagerFacadeImpl : public ManagerFacade {
 public:
  ManagerFacadeImpl(td::actor::ActorId<ValidatorManager> manager,
                    td::actor::ActorId<CollationManager> collation_manager, td::Ref<block::ValidatorSet> validator_set,
                    td::Ref<ValidatorManagerOptions> opts)
      : manager_(manager)
      , collation_manager_(collation_manager)
      , validator_set_(std::move(validator_set))
      , opts_(std::move(opts)) {
  }

  td::actor::Task<GeneratedCandidate> collate_block(CollateParams params,
                                                    td::CancellationToken cancellation_token) override {
    params.validator_set = validator_set_;
    params.collator_opts = opts_->get_collator_options();
    // TODO: support accelerator (use CollationManager)
    auto [task, promise] = td::actor::StartedTask<BlockCandidate>::make_bridge();
    run_collate_query(std::move(params), manager_, td::Timestamp::in(10.0), std::move(cancellation_token),
                      std::move(promise));
    auto candidate = co_await std::move(task);
    co_return GeneratedCandidate{.candidate = std::move(candidate), .self_collated = true};
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate, ValidateParams params,
                                                                    td::Timestamp timeout) override {
    params.validator_set = validator_set_;
    auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
    run_validate_query(std::move(candidate), std::move(params), manager_, timeout, std::move(promise));
    co_return co_await std::move(task);
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, size_t creator_idx,
                                 td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                 bool apply) override {
    while (true) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      run_accept_block_query(id, data, {}, validator_set_, signatures, send_broadcast_mode, apply, manager_,
                             std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_ok() || result.error().code() == ErrorCode::cancelled) {
        break;
      }
      LOG_CHECK(result.error().code() == ErrorCode::timeout || result.error().code() == ErrorCode::notready)
          << "Failed to accept finalized block " << id.to_str() << " : " << result.error();
      LOG(WARNING) << "Failed to accept finalized block " << id.to_str() << ", retrying : " << result.error();
      send_broadcast_mode = 0;
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

  td::actor::Task<BlockCandidate> load_block_candidate(PublicKey source, BlockIdExt block_id,
                                                       FileHash collated_data_hash) override {
    co_return co_await td::actor::ask(manager_, &ValidatorManager::get_block_candidate_from_db, source, block_id,
                                      collated_data_hash);
  }

  td::actor::Task<> store_block_candidate(BlockCandidate candidate) override {
    candidate.out_msg_queue_proof_broadcasts = {};
    BlockIdExt block_id = candidate.id;
    co_return co_await td::actor::ask(manager_, &ValidatorManager::set_block_candidate, block_id, std::move(candidate),
                                      validator_set_->get_catchain_seqno(), validator_set_->get_validator_set_hash());
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
  td::actor::ActorId<CollationManager> collation_manager_;
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
    td::uint32 prefix2 = prefix + 1;
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
    co_return co_await writer_.set(std::move(key), std::move(value));
  }

 private:
  td::KeyValueAsync<td::BufferSlice, td::BufferSlice> writer_;
  std::unique_ptr<td::KeyValueReader> reader_;
};

struct BridgeCreationParams {
  std::string name;
  bool is_create_session_called;

  ShardIdFull shard;
  td::actor::ActorId<ValidatorManager> manager;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  td::Ref<block::ValidatorSet> validator_set;
  PublicKeyHash local_id;

  td::actor::ActorId<CollationManager> collation_manager;
  NewConsensusConfig config;

  ValidatorSessionId session_id;
  td::actor::ActorId<overlay::Overlays> overlays;
  td::actor::ActorId<rldp2::Rldp> rldp2;
  td::actor::ActorId<quic::QuicSender> quic;
  std::string db_root;
};

class BridgeImpl final : public IValidatorGroup {
 public:
  BridgeImpl(BridgeCreationParams&& params)
      : is_create_session_called_(params.is_create_session_called), params_(std::move(params)) {
  }

  virtual void start(std::vector<BlockIdExt> blocks, BlockIdExt min_mc_block_id) override {
    CHECK(!is_start_called_);
    is_start_called_ = true;
    resolve_state_and_start(blocks, min_mc_block_id).start().detach();
  }

  virtual void create_session() override {
    CHECK(!is_create_session_called_);
    is_create_session_called_ = true;
    maybe_start_group();
  }

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) override {
    if (!apply_blocks) {
      LOG(WARNING) << "Accelerator is not consistently supported with simplex consensus";
    }
    td::actor::send_closure(manager_facade_, &ManagerFacadeImpl::update_collator_options, opts);
  }

  virtual void get_validator_group_info_for_litequery(
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) override {
    // TODO
    promise.set_error(td::Status::Error("Not implemented"));
  }

  virtual void notify_mc_finalized(BlockIdExt block) override {
    if (bus_) {
      bus_.publish<BlockFinalizedInMasterchain>(block);
    }
  }

  void destroy() override {
    destroy_inner().start().detach();
  }

  void start_up() override {
    manager_facade_ = td::actor::create_actor<ManagerFacadeImpl>(params_.name + ".ManagerFacade", params_.manager,
                                                                 params_.collation_manager, params_.validator_set,
                                                                 params_.validator_opts);

    bool is_simplex = params_.config.consensus.has<NewConsensusConfig::Simplex>();
    std::shared_ptr<Bus> bus;

    if (is_simplex) {
      auto simplex_bus = std::make_shared<simplex::Bus>();

      simplex_bus->simplex_config = params_.config.consensus.get<NewConsensusConfig::Simplex>();

      bus = simplex_bus;
    } else {
      bus = std::make_shared<null::Bus>();
    }

    bus->shard = params_.shard;
    bus->manager = manager_facade_.get();
    bus->keyring = params_.keyring;
    bus->validator_opts = params_.validator_opts;

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

      if (short_id == params_.local_id) {
        found = true;
        bus->local_id = bus->validator_set.back();
      }

      total_weight += el.weight;
      ++idx;
    }
    bus->total_weight = total_weight;
    bus->cc_seqno = params_.validator_set->get_catchain_seqno();
    bus->validator_set_hash = params_.validator_set->get_validator_set_hash();
    CHECK(found);

    bus->config = std::move(params_.config);

    bus->session_id = params_.session_id;
    bus->overlays = params_.overlays;
    bus->rldp2 = params_.rldp2;
    bus->quic = params_.quic;

    bus->populate_collator_schedule();

    bus->db = std::make_unique<DbImpl>(db_path() + "/db/");

    auto [stop_waiter, stop_promise] = td::actor::StartedTask<>::make_bridge();
    stop_waiter_ = std::move(stop_waiter);
    bus->stop_promise = std::move(stop_promise);

    runtime::Runtime runtime;
    BlockAccepter::register_in(runtime);
    BlockProducer::register_in(runtime);
    BlockValidator::register_in(runtime);
    PrivateOverlay::register_in(runtime);
    TraceCollector::register_in(runtime);

    if (is_simplex) {
      auto simplex_bus = std::static_pointer_cast<simplex::Bus>(bus);
      simplex_bus->load_bootstrap_state();

      simplex::CandidateResolver::register_in(runtime);
      simplex::Consensus::register_in(runtime);
      simplex::Pool::register_in(runtime);
      simplex::MetricCollector::register_in(runtime);

      bus_ = runtime.start(simplex_bus, params_.name);
    } else {
      auto null_bus = std::static_pointer_cast<null::Bus>(bus);

      null::Consensus::register_in(runtime);

      bus_ = runtime.start(null_bus, params_.name);
    }
  }

 private:
  td::actor::Task<> destroy_inner() {
    if (bus_) {
      LOG(INFO) << "Destroying validator group";
      bus_.publish<StopRequested>();
      bus_ = {};
      co_await std::move(stop_waiter_.value());
      LOG(INFO) << "Consensus bus stopped";
      td::rmrf(db_path()).ignore();
    }
    stop();
    co_return td::Unit{};
  }

  td::actor::Task<> resolve_state_and_start(std::vector<BlockIdExt> blocks, BlockIdExt min_mc_block_id) {
    auto state = co_await ChainState::from_manager(manager_facade_.get(), params_.shard, blocks, min_mc_block_id);
    start_event_ = std::make_shared<Start>(state);
    maybe_start_group();
    co_return {};
  }

  void maybe_start_group() {
    if (!is_create_session_called_ || !is_start_called_ || !start_event_ || is_started_) {
      return;
    }
    is_started_ = true;
    bus_.publish(start_event_);
  }

  bool is_start_called_ = false;
  bool is_create_session_called_ = false;
  bool is_started_ = false;

  BridgeCreationParams params_;
  td::actor::ActorOwn<ManagerFacadeImpl> manager_facade_;

  BusHandle bus_;
  td::optional<td::actor::StartedTask<>> stop_waiter_;

  std::shared_ptr<Start> start_event_;

  std::string db_path() const {
    return PSTRING() << params_.db_root << "/consensus/consensus." << params_.shard.workchain << "."
                     << params_.shard.shard << "." << params_.validator_set->get_catchain_seqno() << "."
                     << params_.session_id.to_hex() << "/";
  }
};

}  // namespace
}  // namespace consensus

td::actor::ActorOwn<IValidatorGroup> IValidatorGroup::create_bridge(
    td::Slice name, ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
    td::Ref<block::ValidatorSet> validator_set, BlockSeqno last_key_block_seqno, NewConsensusConfig config,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
    td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
    td::actor::ActorId<ValidatorManager> validator_manager, td::actor::ActorId<CollationManager> collation_manager,
    bool create_session, bool allow_unsafe_self_blocks_resync, td::Ref<ValidatorManagerOptions> opts,
    bool monitoring_shard) {
  auto name_with_seqno =
      std::string(name.begin(), name.end()) + "." + std::to_string(validator_set->get_catchain_seqno());
  consensus::BridgeCreationParams params{
      .name = name_with_seqno,
      .is_create_session_called = create_session,
      .shard = shard,
      .manager = validator_manager,
      .keyring = keyring,
      .validator_opts = opts,
      .validator_set = std::move(validator_set),
      .local_id = std::move(local_id),
      .collation_manager = collation_manager,
      .config = std::move(config),
      .session_id = std::move(session_id),
      .overlays = overlays,
      .rldp2 = rldp2,
      .quic = quic,
      .db_root = db_root,
  };
  return td::actor::create_actor<consensus::BridgeImpl>(name_with_seqno, std::move(params));
}

}  // namespace ton::validator
