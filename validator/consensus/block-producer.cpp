/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus {

namespace {

class BlockProducerImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    td::uint32 seqno = event->state->next_seqno() - 1;
    last_mc_finalized_seqno_ = std::max(last_mc_finalized_seqno_, seqno);
    last_consensus_finalized_seqno_ = std::max(last_consensus_finalized_seqno_, seqno);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    current_leader_window_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalized> event) {
    if (event->final_signatures) {
      last_consensus_finalized_seqno_ = std::max(last_consensus_finalized_seqno_, event->candidate.block.seqno());
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowStarted> event) {
    current_leader_window_ = event->start_slot;
    cancellation_source_ = td::CancellationTokenSource();
    generate_candidates(event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowAborted> event) {
    // Sanity check: consensus and us should agree on the start slot.
    CHECK(current_leader_window_ == event->start_slot);
    current_leader_window_ = std::nullopt;
    cancellation_source_ = td::CancellationTokenSource();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = std::max(event->block.seqno(), last_mc_finalized_seqno_);
    last_consensus_finalized_seqno_ = std::max(last_mc_finalized_seqno_, last_consensus_finalized_seqno_);
  }

 private:
  bool should_generate_empty_block(const ChainStateRef& state) {
    if (state->is_before_split()) {
      return true;
    }
    if (owning_bus()->shard.is_masterchain()) {
      return last_consensus_finalized_seqno_ + 1 < state->next_seqno();
    } else {
      return last_mc_finalized_seqno_ + 8 < state->next_seqno();
    }
  }

  td::actor::Task<> generate_candidates(std::shared_ptr<const OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return {};
    }

    td::Timestamp target_time = event->start_time;

    ChainStateRef state = event->state;
    RawParentId parent = event->base;

    td::uint32 slot = event->start_slot;

    while (current_leader_window_ == window && slot < event->end_slot) {
      co_await td::actor::coro_sleep(target_time);

      CandidateId id;
      std::variant<BlockIdExt, BlockCandidate> block;
      std::optional<adnl::AdnlNodeIdShort> collator;

      if (should_generate_empty_block(state)) {
        LOG(WARNING) << "Generating an empty block for slot " << slot << "! new_seqno=" << state->next_seqno()
                     << ", last_consensus_finalized_seqno_=" << last_consensus_finalized_seqno_
                     << ", last_mc_finalized_seqno_=" << last_mc_finalized_seqno_;
        CHECK(parent.has_value());  // first generated block in an epoch cannot be empty

        auto referenced_block = state->assert_normal();
        block = referenced_block;
        id = CandidateId::create(slot, CandidateHashData::create_empty(referenced_block, *parent));

        owning_bus().publish<TraceEvent>(stats::CollatedEmpty::create(id));
      } else {
        // Before doing anything substantial, check the leader window.
        if (current_leader_window_ != window) {
          break;
        }

        owning_bus().publish<TraceEvent>(stats::CollateStarted::create(slot));

        // FIXME: What to do if collate_block suddenly fails?
        CollateParams params{
            .shard = bus.shard,
            .min_masterchain_block_id = state->min_mc_block_id(),
            .prev = state->block_ids(),
            .creator = Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()},
            .prev_block_data = state->block_data(),
            .prev_block_state_roots = state->state(),
            .is_new_consensus = true,
        };
        auto block_candidate = co_await td::actor::ask(bus.manager, &ManagerFacade::collate_block, std::move(params),
                                                       cancellation_source_.get_cancellation_token());

        state = state->apply(block_candidate.candidate);

        block = std::move(block_candidate.candidate);
        if (!block_candidate.collator_node_id.is_zero()) {
          collator = adnl::AdnlNodeIdShort{block_candidate.collator_node_id};
        }
        id = CandidateId::create(slot, CandidateHashData::create_full(block_candidate.candidate, parent));

        owning_bus().publish<TraceEvent>(stats::CollateFinished::create(slot, id));
      }

      auto id_to_sign = serialize_tl_object(id.as_raw().to_tl(), true);
      auto data_to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));

      auto candidate = td::make_ref<RawCandidate>(id, parent, bus.local_id.idx, std::move(block), std::move(signature));

      if (current_leader_window_ != window) {
        break;
      }
      owning_bus().publish<CandidateGenerated>(candidate, collator);
      owning_bus().publish<CandidateReceived>(candidate);
      owning_bus().publish<TraceEvent>(stats::CandidateReceived::create(candidate, true));

      ++slot;
      parent = id;
      target_time = td::Timestamp::in(bus.config.target_rate_ms / 1000., target_time);
    }

    co_return {};
  }

  std::optional<td::uint32> current_leader_window_;
  td::CancellationTokenSource cancellation_source_;

  BlockSeqno last_consensus_finalized_seqno_ = 0;
  BlockSeqno last_mc_finalized_seqno_ = 0;
};

}  // namespace

void BlockProducer::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator::consensus
