/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/window-producer.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_task.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

class CollatorProducerImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.is_collator && bus.config.enable_collators() && !bus.shard.is_masterchain();
  }

  void start_up() override {
    auto& bus = *owning_bus();
    target_rate_ = bus.config.noncritical_params.target_rate;
    no_empty_blocks_on_error_timeout_ = bus.config.noncritical_params.no_empty_blocks_on_error_timeout;
    slots_per_leader_window_ = bus.config.slots_per_leader_window;
    own_key_ = td::actor::ask(bus.keyring, &keyring::Keyring::get_public_key, bus.local_adnl_id.pubkey_hash());
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NoncriticalParamsUpdated> event) {
    target_rate_ = event->params.target_rate;
    no_empty_blocks_on_error_timeout_ = event->params.no_empty_blocks_on_error_timeout;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    empty_block_policy_.observe_session_start(event->state->next_seqno() - 1);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    producing_window_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizeBlock> event) {
    if (event->signatures->is_final()) {
      empty_block_policy_.observe_consensus_finalized(event->candidate->block_id().seqno());
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    empty_block_policy_.observe_mc_finalized(event->block.seqno());
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateGenerated> event) {
    bus.publish<StoreCandidate>(event->candidate).start().detach();
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<IncomingCollatorRequest> message) {
    auto& bus = *owning_bus();

    if (!message->source_validator.has_value()) {
      co_return td::Status::Error(ErrorCode::protoviolation, "Node is not a validator");
    }
    if (!bus.validator_opts.load()->check_collator_node_whitelist(message->source)) {
      co_return td::Status::Error("Validator is not whitelisted");
    }

    td::uint32 window_start;
    bool is_prepare;
    td::BufferSlice delegation_signature;
    if (auto R = fetch_tl_object<consensus::tl::pleaseCollatePrepare>(message->request.data, true); R.is_ok()) {
      auto request = R.move_as_ok();
      is_prepare = true;
      window_start = request->window_start_slot_;
    } else if (auto R = fetch_tl_object<consensus::tl::pleaseCollate>(message->request.data, true); R.is_ok()) {
      auto request = R.move_as_ok();
      is_prepare = false;
      window_start = request->window_start_slot_;
      delegation_signature = std::move(request->signature_);
    } else {
      co_return td::Status::Error(ErrorCode::protoviolation, "Cannot parse request");
    }

    if (window_start % slots_per_leader_window_ != 0) {
      co_return td::Status::Error(ErrorCode::protoviolation, PSTRING()
                                                                 << "Misaligned window start slot " << window_start);
    }
    if (bus.collator_schedule->expected_collator_for(window_start) != *message->source_validator) {
      co_return td::Status::Error(ErrorCode::protoviolation, PSTRING()
                                                                 << "Src is not the leader of slot " << window_start);
    }
    if (last_window_.has_value() && window_start < last_window_->start_slot) {
      co_return td::Status::Error(PSTRING() << "Too old slot" << window_start << " < " << last_window_->start_slot);
    }
    td::uint32 first_too_new_slot =
        (last_window_ ? last_window_->start_slot : 0) + MAX_FUTURE_WINDOW * slots_per_leader_window_;
    if (window_start >= first_too_new_slot) {
      co_return td::Status::Error(PSTRING() << "Too new slot" << window_start << " > " << first_too_new_slot);
    }
    if (delegation_signatures_.contains(window_start)) {
      co_return td::Status::Error(PSTRING() << "Duplicate delegation for slot " << window_start);
    }
    if (is_prepare) {
      co_return ProtocolMessage{create_tl_object<ton_api::tonNode_success>()};
    }

    const PeerValidator& leader = message->source_validator->get_using(bus);
    auto to_sign =
        create_serialize_tl_object<consensus::tl::delegationToSign>(window_start, bus.local_adnl_id.bits256_value());
    if (!leader.check_signature(bus.session_id, to_sign, delegation_signature)) {
      co_return td::Status::Error(ErrorCode::protoviolation, "Invalid delegation signature");
    }

    LOG(INFO) << "Window " << window_start << " is delegated to us by " << leader;
    delegation_signatures_[window_start] = std::move(delegation_signature);

    if (last_window_.has_value() && last_window_->start_slot == window_start) {
      start_production(window_start, last_window_->base);
    }
    co_return ProtocolMessage{create_tl_object<ton_api::tonNode_success>()};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const LeaderWindowObserved> event) {
    last_window_ = Window{event->start_slot, event->base};
    delegation_signatures_.erase(delegation_signatures_.begin(), delegation_signatures_.lower_bound(event->start_slot));

    if (event->start_slot % slots_per_leader_window_ == 0 && delegation_signatures_.contains(event->start_slot)) {
      start_production(event->start_slot, event->base);
    }
  }

 private:
  struct Window {
    td::uint32 start_slot;
    ParentId base;
  };

  void start_production(td::uint32 window_start, ParentId base) {
    if (producing_window_ == window_start) {
      return;
    }
    producing_window_ = window_start;
    cancellation_source_ = td::CancellationTokenSource();
    produce(window_start, base).start().detach();
  }

  td::actor::Task<> produce(td::uint32 window_start, ParentId base) {
    auto& bus = *owning_bus();

    auto own_key = co_await own_key_.get();
    auto it = delegation_signatures_.find(window_start);
    if (it == delegation_signatures_.end()) {
      co_return td::Status::Error(PSTRING() << "No delegation for slot " << window_start);
    }
    Delegation delegation{own_key, it->second.clone()};

    auto parent = co_await owning_bus().publish<ResolveState>(base);
    td::Timestamp start_time = td::Timestamp::now();
    if (parent.gen_utime_exact.has_value()) {
      start_time = std::max(start_time, td::Timestamp::at_unix(*parent.gen_utime_exact) + target_rate_);
      start_time = std::min(start_time, td::Timestamp::in(target_rate_));
    }

    if (producing_window_ != window_start) {
      co_return {};
    }

    ProduceWindowContext ctx{
        .base = base,
        .state = parent.state,
        .start_slot = window_start,
        .end_slot = window_start + slots_per_leader_window_,
        .start_time = start_time,
        .leader = bus.collator_schedule->expected_collator_for(window_start).get_using(bus),
        .signing_key = bus.local_adnl_id.pubkey_hash(),
        .delegation = std::move(delegation),
        .collator_node_id = bus.local_adnl_id,
        .target_rate = target_rate_,
        .cancellation_token = cancellation_source_.get_cancellation_token(),
        .is_superseded = [&, window_start] { return producing_window_ != window_start; },
        .should_generate_empty_block =
            [&](const ChainStateRef& state) {
              return empty_block_policy_.should_generate_empty_block(owning_bus()->shard.is_masterchain(), state);
            },
        .allow_empty_on_generation_failure =
            [&] { return empty_block_policy_.allow_empty_on_generation_failure(no_empty_blocks_on_error_timeout_); },
    };
    co_await produce_window(owning_bus(), std::move(ctx));

    if (producing_window_ == window_start) {
      producing_window_ = std::nullopt;
    }
    co_return {};
  }

  td::uint32 slots_per_leader_window_;
  std::chrono::milliseconds target_rate_;
  std::chrono::milliseconds no_empty_blocks_on_error_timeout_;

  td::actor::SharedFuture<PublicKey> own_key_;
  std::map<td::uint32, td::BufferSlice> delegation_signatures_;
  std::optional<Window> last_window_;
  std::optional<td::uint32> producing_window_;
  td::CancellationTokenSource cancellation_source_;

  EmptyBlockPolicy empty_block_policy_;

  static constexpr td::uint32 MAX_FUTURE_WINDOW = 20;
};

}  // namespace

void CollatorProducer::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<CollatorProducerImpl>("CollatorProducer");
}

}  // namespace ton::validator::consensus::simplex
