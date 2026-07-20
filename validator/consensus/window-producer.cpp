/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/SharedFuture.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"

#include "stats.h"
#include "window-producer.h"

namespace ton::validator::consensus {

td::actor::Task<> produce_window(BusHandle bus_handle, ProduceWindowContext ctx) {
  auto& bus = *bus_handle;

  ChainStateRef state = ctx.state;
  ParentId parent = ctx.base;
  bool block_generation_active = false;
  td::actor::SharedFuture<GeneratedCandidate> block_generation;

  std::chrono::milliseconds hard_timeout = std::max(ctx.target_rate * 3, std::chrono::milliseconds(60'000));
  std::chrono::milliseconds start_collate_before =
      bus.shard.is_masterchain() ? std::chrono::milliseconds(0) : ctx.target_rate;
  td::Timestamp slot_start = ctx.start_time;

  for (td::uint32 slot = ctx.start_slot; !ctx.is_superseded() && slot < ctx.end_slot; ++slot) {
    co_await td::actor::coro_sleep(slot_start - start_collate_before);
    if (ctx.is_superseded()) {
      break;
    }
    bool is_first_block = !parent.has_value();
    if (!block_generation_active && (!ctx.should_generate_empty_block(state) || is_first_block)) {
      block_generation_active = true;
      CollateParams params{
          .shard = bus.shard,
          .min_masterchain_block_id = state->min_mc_block_id(),
          .prev = state->block_ids(),
          .creator = Ed25519_PublicKey{ctx.leader.key.ed25519_value().raw()},
          .utime = slot_start.at_unix(),
          .hard_timeout = slot_start + hard_timeout,
          .prev_block_data = state->block_data(),
          .prev_block_state_roots = state->state(),
      };
      if (bus.shard.is_masterchain()) {
        params.soft_timeout = slot_start + ctx.target_rate;
      } else {
        params.soft_timeout = slot_start;
        params.wait_externals_until = slot_start;
      }
      if (ctx.collator_node_id) {
        params.collator_node_id = *ctx.collator_node_id;
      }
      block_generation =
          td::actor::ask(bus.manager, &ManagerFacade::collate_block, std::move(params), ctx.cancellation_token);
      bus_handle.publish<TraceEvent>(stats::CollateStarted::create(slot));
    }
    co_await td::actor::coro_sleep(slot_start);

    std::optional<GeneratedCandidate> generated_candidate;
    if (block_generation_active) {
      auto r_candidate =
          co_await td::actor::await_with_timeout(block_generation.get(), slot_start + ctx.target_rate).wrap();
      // The first block in the session cannot be empty
      bool allow_empty = !is_first_block && ctx.allow_empty_on_generation_failure();
      if (r_candidate.is_error() && !allow_empty) {
        LOG(WARNING) << "Generating the first block: "
                     << (r_candidate.error().code() == td::actor::AWAIT_TIMEOUT_CODE ? "takes too long"
                                                                                     : r_candidate.error().to_string())
                     << ", don't generate empty block "
                     << (is_first_block ? "(first block)" : "(no finalized blocks for too long)");
        --slot;
        if (r_candidate.error().code() != td::actor::AWAIT_TIMEOUT_CODE) {
          block_generation_active = false;
          co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
        }
        slot_start = std::max(slot_start, td::Timestamp::now());
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
      LOG(WARNING) << "Generating an empty block for slot " << slot << ": new_seqno=" << state->next_seqno();
    }
    if (ctx.is_superseded()) {
      break;
    }

    CandidateId id;
    std::variant<BlockIdExt, BlockCandidate> block;
    if (generated_candidate.has_value()) {
      td::actor::send_closure(bus.manager, &ManagerFacade::cache_block_candidate,
                              generated_candidate->candidate.clone());
      state = state->apply(generated_candidate->candidate);
      block = std::move(generated_candidate->candidate);
      id = CandidateHashData::create_full(generated_candidate->candidate, parent).build_id_with(slot);
      bus_handle.publish<TraceEvent>(stats::CollateFinished::create(slot, id));
    } else {
      CHECK(parent.has_value());
      auto referenced_block = state->assert_normal();
      block = referenced_block;
      id = CandidateHashData::create_empty(referenced_block, *parent).build_id_with(slot);
      bus_handle.publish<TraceEvent>(stats::CollatedEmpty::create(id));
    }

    auto id_to_sign = serialize_tl_object(id.to_tl(), true);
    auto data_to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(id_to_sign));
    auto signature =
        co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, ctx.signing_key, std::move(data_to_sign));
    std::optional<Delegation> delegation;
    if (ctx.delegation.has_value()) {
      delegation = ctx.delegation->clone();
    }
    auto candidate = td::make_ref<Candidate>(id, parent, ctx.leader.idx, std::move(block), std::move(signature),
                                             std::move(delegation));
    if (ctx.is_superseded()) {
      break;
    }
    bus_handle.publish<CandidateGenerated>(candidate);
    bus_handle.publish<CandidateReceived>(candidate);
    bus_handle.publish<TraceEvent>(stats::CandidateReceived::create(candidate, true));
    parent = id;

    slot_start += ctx.target_rate;
  }

  co_return {};
}

}  // namespace ton::validator::consensus
