/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "validator-session/validator-session-types.h"
#include "validator/consensus/null/bus.h"
#include "validator/consensus/simplex/bus.h"
#include "validator/fabric.h"
#include "validator/full-node.h"
#include "validator/validator-group.hpp"

#include "runtime.h"

namespace ton::validator {

namespace consensus {
namespace {

class ManagerFacadeImpl : public ManagerFacade {
 public:
  ManagerFacadeImpl(td::actor::ActorId<ValidatorManager> manager,
                    td::actor::ActorId<CollationManager> collation_manager, td::Ref<block::ValidatorSet> validator_set)
      : manager_(manager), collation_manager_(collation_manager), validator_set_(std::move(validator_set)) {
  }

  td::actor::Task<GeneratedCandidate> collate_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id,
                                                    std::vector<BlockIdExt> prev, Ed25519_PublicKey creator,
                                                    BlockCandidatePriority priority, td::uint64 max_answer_size,
                                                    td::CancellationToken cancellation_token) override {
    co_return co_await td::actor::ask(collation_manager_, &CollationManager::collate_block, shard,
                                      min_masterchain_block_id, std::move(prev), creator, priority, validator_set_,
                                      max_answer_size, cancellation_token);
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate, ValidateParams params,
                                                                    td::Timestamp timeout) override {
    params.validator_set = validator_set_;
    auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
    run_validate_query(std::move(candidate), std::move(params), manager_, timeout, std::move(promise));
    co_return co_await std::move(task);
  }

  td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                 td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                 bool apply) override {
    auto [task, promise] = td::actor::StartedTask<>::make_bridge();
    run_accept_block_query(id, std::move(data), std::move(prev), validator_set_, std::move(signatures),
                           send_broadcast_mode, apply, manager_, std::move(promise));
    auto result = co_await std::move(task).wrap();
    LOG_CHECK(!result.is_error()) << "Failed to accept finalized block " << result.move_as_error();
    co_return {};
  }

  void log_validator_session_stats(validatorsession::ValidatorSessionStats stats) override {
    stats.cc_seqno = validator_set_->get_catchain_seqno();
    td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, std::move(stats));
  }

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<CollationManager> collation_manager_;
  td::Ref<block::ValidatorSet> validator_set_;
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
  BlockIdExt min_masterchain_block_id = {};

  ValidatorSessionId session_id;
  td::actor::ActorId<overlay::Overlays> overlays;

  std::vector<BlockIdExt> first_block_parents = {};
};

class BridgeImpl final : public IValidatorGroup {
 public:
  BridgeImpl(BridgeCreationParams&& params)
      : is_create_session_called_(params.is_create_session_called), params_(std::move(params)) {
  }

  virtual void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) override {
    CHECK(!is_start_called_);
    is_start_called_ = true;
    params_.min_masterchain_block_id = min_masterchain_block_id;
    params_.first_block_parents = prev;
    try_start();
  }

  virtual void create_session() override {
    CHECK(!is_create_session_called_);
    is_create_session_called_ = true;
    try_start();
  }

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) override {
    // TODO
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

  virtual void destroy() override {
    if (is_started_) {
      bus_.publish<StopRequested>();
    }
    stop();
  }

 private:
  void try_start() {
    if (!is_start_called_ || !is_create_session_called_ || is_started_) {
      return;
    }

    manager_facade_ = td::actor::create_actor<ManagerFacadeImpl>(params_.name + ".ManagerFacade", params_.manager,
                                                                 params_.collation_manager, params_.validator_set);

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
    bus->min_masterchain_block_id = params_.min_masterchain_block_id;

    bus->session_id = params_.session_id;
    bus->overlays = params_.overlays;

    bus->first_block_parents = std::move(params_.first_block_parents);

    runtime::Runtime runtime;
    BlockAccepter::register_in(runtime);
    BlockProducer::register_in(runtime);
    BlockValidator::register_in(runtime);
    PrivateOverlay::register_in(runtime);
    StatsCollector::register_in(runtime);

    if (is_simplex) {
      simplex::CandidateResolver::register_in(runtime);
      simplex::Consensus::register_in(runtime);
      simplex::Pool::register_in(runtime);

      bus_ = runtime.start(std::static_pointer_cast<simplex::Bus>(bus), params_.name);
    } else {
      null::Consensus::register_in(runtime);

      bus_ = runtime.start(std::static_pointer_cast<null::Bus>(bus), params_.name);
    }

    is_started_ = true;
  }

  bool is_start_called_ = false;
  bool is_create_session_called_ = false;
  bool is_started_ = false;

  BridgeCreationParams params_;
  td::actor::ActorOwn<ManagerFacade> manager_facade_;

  BusHandle bus_;
};

}  // namespace
}  // namespace consensus

td::actor::ActorOwn<IValidatorGroup> IValidatorGroup::create_bridge(
    td::Slice name, ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
    td::Ref<block::ValidatorSet> validator_set, BlockSeqno last_key_block_seqno, NewConsensusConfig config,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
    td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
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
  };
  return td::actor::create_actor<consensus::BridgeImpl>(name_with_seqno, std::move(params));
}

}  // namespace ton::validator
