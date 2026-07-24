/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_task.h"
#include "td/utils/CancellationToken.h"
#include "validator/collator-scoreboard.hpp"

#include "bus.h"
#include "window-producer.h"

namespace ton::validator::consensus {

namespace {

class BlockProducerImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.is_validator();
  }

  void start_up() override {
    auto& bus = *owning_bus();
    target_rate_ = bus.config.noncritical_params.target_rate;
    no_empty_blocks_on_error_timeout_ = bus.config.noncritical_params.no_empty_blocks_on_error_timeout;

    if (!bus.shard.is_masterchain() && bus.config.enable_collators()) {
      auto list = bus.validator_opts.load()->get_collators_list();
      auto shard = list->get_shard(bus.shard);
      if (shard != nullptr) {
        for (const adnl::AdnlNodeIdShort& collator_id : shard->collators) {
          if (std::find(bus.all_overlay_nodes.begin(), bus.all_overlay_nodes.end(), collator_id) !=
              bus.all_overlay_nodes.end()) {
            collator_nodes_.push_back(collator_id);
            LOG(INFO) << "Configured collator node " << collator_id;
          } else {
            LOG(WARNING) << "Collator node " << collator_id << " is not in overlay!";
          }
        }
        allow_self_collate_ = shard->self_collate;
      }
    }
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
    current_leader_window_ = std::nullopt;
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
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowStarted> event) {
    CHECK(current_leader_window_ < event->start_slot);

    current_leader_window_ = event->start_slot;
    if (delegated_windows_.contains(event->start_slot)) {
      LOG(INFO) << "Window " << event->start_slot << " is delegated to "
                << delegated_windows_.at(event->start_slot).collator << ", not producing";
      return;
    }
    if (!allow_self_collate_) {
      LOG(WARNING) << "Window " << event->start_slot << " is not delegated to collator, self collation is not allowed";
      return;
    }
    cancellation_source_ = td::CancellationTokenSource();
    generate_candidates(event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowUpcoming> event) {
    conclude_delegations_before(event->start_slot);
    prepare_delegation(event->start_slot).start().detach();
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const CandidateReceived> event) {
    if (event->candidate->leader != bus->local_id->idx || !event->candidate->delegation.has_value()) {
      return;
    }
    td::uint32 slot = event->candidate->id.slot;
    auto it = delegated_windows_.find(slot - slot % bus->config.slots_per_leader_window);
    if (it != delegated_windows_.end()) {
      it->second.produced = true;
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    empty_block_policy_.observe_mc_finalized(event->block.seqno());
  }

 private:
  td::actor::Task<> prepare_delegation(td::uint32 start_slot) {
    auto& bus = *owning_bus();

    if (collator_nodes_.empty()) {
      co_return {};
    }
    auto maybe_collator =
        co_await td::actor::ask(bus.collator_scoreboard, &CollatorScoreboard::pick_collator, collator_nodes_).wrap();
    if (maybe_collator.is_error()) {
      LOG(INFO) << "Not delegating window " << start_slot << ": " << maybe_collator.error();
      co_return {};
    }
    auto collator = maybe_collator.move_as_ok();

    auto to_sign = create_serialize_tl_object<tl::delegationToSign>(start_slot, collator.bits256_value());
    to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(to_sign));
    auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id->short_id,
                                             std::move(to_sign));

    if (current_leader_window_.has_value() && *current_leader_window_ >= start_slot) {
      co_return {};
    }

    LOG(INFO) << "Delegating window " << start_slot << " to " << collator;
    owning_bus().publish(std::make_shared<OutgoingProtocolMessage>(
        OutgoingProtocolMessage::SendToPeer{collator},
        create_serialize_tl_object<tl::pleaseCollate>(start_slot, std::move(signature))));
    delegated_windows_[start_slot] = DelegatedWindow{collator, false};
    co_return {};
  }

  void conclude_delegations_before(td::uint32 boundary_slot) {
    auto& bus = *owning_bus();
    while (!delegated_windows_.empty() && delegated_windows_.begin()->first < boundary_slot) {
      auto& [start_slot, window] = *delegated_windows_.begin();
      td::actor::send_closure(bus.collator_scoreboard, &CollatorScoreboard::report_outcome, window.collator,
                              window.produced);
      delegated_windows_.erase(delegated_windows_.begin());
    }
  }

  td::actor::Task<> generate_candidates(std::shared_ptr<const OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return {};
    }

    ProduceWindowContext ctx{
        .base = event->base,
        .state = event->state,
        .start_slot = event->start_slot,
        .end_slot = event->end_slot,
        .start_time = event->start_time,
        .leader = *bus.local_id,
        .signing_key = bus.local_id->short_id,
        .delegation = std::nullopt,
        .collator_node_id = std::nullopt,
        .target_rate = target_rate_,
        .cancellation_token = cancellation_source_.get_cancellation_token(),
        .is_superseded = [&, window] { return current_leader_window_ != window; },
        .should_generate_empty_block =
            [&](const ChainStateRef& state) {
              return empty_block_policy_.should_generate_empty_block(owning_bus()->shard.is_masterchain(), state);
            },
        .allow_empty_on_generation_failure =
            [&] { return empty_block_policy_.allow_empty_on_generation_failure(no_empty_blocks_on_error_timeout_); },
    };
    co_await produce_window(owning_bus(), std::move(ctx));

    if (current_leader_window_ == window) {
      current_leader_window_ = std::nullopt;
    }

    co_return {};
  }

  struct DelegatedWindow {
    adnl::AdnlNodeIdShort collator;
    bool produced = false;
  };

  std::optional<td::uint32> current_leader_window_;
  td::CancellationTokenSource cancellation_source_;

  std::vector<adnl::AdnlNodeIdShort> collator_nodes_;
  bool allow_self_collate_ = true;
  std::map<td::uint32, DelegatedWindow> delegated_windows_;

  EmptyBlockPolicy empty_block_policy_;
  std::chrono::milliseconds target_rate_;
  std::chrono::milliseconds no_empty_blocks_on_error_timeout_;
};

}  // namespace

void BlockProducer::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator::consensus
