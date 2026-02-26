/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/SharedFuture.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "stats.h"
#include "utils.h"

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
  void handle(BusHandle, std::shared_ptr<const FinalizeBlock> event) {
    if (event->signatures->is_final()) {
      last_consensus_finalized_seqno_ = std::max(last_consensus_finalized_seqno_, event->candidate->block_id().seqno());
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

    ChainStateRef state = event->state;
    ParentId parent = event->base;
    bool block_generation_active = false;
    td::actor::SharedFuture<GeneratedCandidate> block_generation;

    td::Timestamp start_time = event->start_time;
    double target_rate = bus.config.target_rate_ms / 1000.0;
    double hard_timeout = std::max(target_rate * 3.0, 5.0);
    double start_collate_before = bus.shard.is_masterchain() ? 0.0 : target_rate;

    for (td::uint32 slot = event->start_slot; current_leader_window_ == window && slot < event->end_slot; ++slot) {
      td::Timestamp slot_start = start_time + target_rate * (slot - event->start_slot);
      co_await td::actor::coro_sleep(slot_start - start_collate_before);
      if (current_leader_window_ != window) {
        break;
      }
      bool is_first_block = !parent.has_value();
      if (!block_generation_active && (!should_generate_empty_block(state) || is_first_block)) {
        block_generation_active = true;
        CollateParams params{
            .shard = bus.shard,
            .min_masterchain_block_id = state->min_mc_block_id(),
            .prev = state->block_ids(),
            .creator = Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()},
            .skip_store_candidate = true,
            .utime = slot_start.at_unix(),
            .hard_timeout = slot_start + hard_timeout,
            .prev_block_data = state->block_data(),
            .prev_block_state_roots = state->state(),
            .is_new_consensus = true,
        };
        if (bus.shard.is_masterchain()) {
          params.soft_timeout = slot_start + target_rate;
        } else {
          params.soft_timeout = slot_start;
          params.wait_externals_until = slot_start;
        }
        block_generation = td::actor::ask(bus.manager, &ManagerFacade::collate_block, std::move(params),
                                          cancellation_source_.get_cancellation_token());
        owning_bus().publish<TraceEvent>(stats::CollateStarted::create(slot));
      }
      co_await td::actor::coro_sleep(slot_start);

      std::optional<GeneratedCandidate> generated_candidate;
      if (block_generation_active) {
        auto r_candidate =
            co_await td::actor::await_with_timeout(block_generation.get(), slot_start + target_rate).wrap();
        if (r_candidate.is_error() && is_first_block) {
          // The first block in the session cannot be empty
          LOG(WARNING) << "Generating the first block: "
                       << (r_candidate.error().code() == td::actor::AWAIT_TIMEOUT_CODE
                               ? "takes too long"
                               : r_candidate.error().to_string())
                       << ", don't generate empty block";
          --slot;
          start_time = td::Timestamp::now();
          continue;
        }
        if (r_candidate.is_ok()) {
          generated_candidate = r_candidate.move_as_ok();
          block_generation_active = false;
        } else if (r_candidate.error().code() == td::actor::AWAIT_TIMEOUT_CODE) {
          generated_candidate = std::nullopt;
          LOG(WARNING) << "Generating an empty block for slot " << slot << ": block collation takes too long";
        } else {
          generated_candidate = std::nullopt;
          LOG(WARNING) << "Generating an empty block for slot " << slot << ": collation error: " << r_candidate.error();
          block_generation_active = false;
        }
      } else {
        generated_candidate = std::nullopt;
        LOG(WARNING) << "Generating an empty block for slot " << slot << ": new_seqno=" << state->next_seqno()
                     << ", last_consensus_finalized_seqno_=" << last_consensus_finalized_seqno_
                     << ", last_mc_finalized_seqno_=" << last_mc_finalized_seqno_
                     << ", before_split=" << state->is_before_split();
      }
      if (current_leader_window_ != window) {
        break;
      }

      CandidateId id;
      std::variant<BlockIdExt, BlockCandidate> block;
      std::optional<adnl::AdnlNodeIdShort> collator;
      if (generated_candidate.has_value()) {
        state = state->apply(generated_candidate->candidate);
        block = std::move(generated_candidate->candidate);
        if (!generated_candidate->collator_node_id.is_zero()) {
          collator = adnl::AdnlNodeIdShort{generated_candidate->collator_node_id};
        }
        id = CandidateHashData::create_full(generated_candidate->candidate, parent).build_id_with(slot);
        owning_bus().publish<TraceEvent>(stats::CollateFinished::create(slot, id));
      } else {
        CHECK(parent.has_value());
        auto referenced_block = state->assert_normal();
        block = referenced_block;
        id = CandidateHashData::create_empty(referenced_block, *parent).build_id_with(slot);
        owning_bus().publish<TraceEvent>(stats::CollatedEmpty::create(id));
      }

      auto id_to_sign = serialize_tl_object(id.to_tl(), true);
      auto data_to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));
      auto candidate = td::make_ref<Candidate>(id, parent, bus.local_id.idx, std::move(block), std::move(signature));
      if (current_leader_window_ != window) {
        break;
      }
      owning_bus().publish<CandidateGenerated>(candidate, collator);
      owning_bus().publish<CandidateReceived>(candidate);
      owning_bus().publish<TraceEvent>(stats::CandidateReceived::create(candidate, true));
      parent = id;
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
