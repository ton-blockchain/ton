/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "utils.h"

namespace ton::validator::consensus {

namespace {

class CandidateParent {
 public:
  CandidateParent(const Bus& bus, const ParentId& parent) {
    parent_blocks_ = bus.convert_id_to_blocks(parent);
    seqno_ = parent_blocks_.size() == 1 ? parent_blocks_[0].seqno()
                                        : std::max(parent_blocks_[0].seqno(), parent_blocks_[1].seqno());
    parent_id_ = parent;
  }

  CandidateParent(const CandidateId& id) {
    parent_blocks_ = {id.block};
    seqno_ = id.block.seqno();
    parent_id_ = id;
  }

  const std::vector<BlockIdExt>& parent_blocks() const {
    return parent_blocks_;
  }

  int seqno() const {
    return seqno_;
  }

  int next_seqno() const {
    return seqno_ + 1;
  }

  ParentId id() const {
    return parent_id_;
  }

 private:
  std::vector<BlockIdExt> parent_blocks_;
  int seqno_;
  ParentId parent_id_;
};

class BlockProducerImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();

    last_mc_finalized_seqno_ = last_consensus_finalized_seqno_ = CandidateParent{bus, std::nullopt}.seqno();
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
  bool is_before_split(const std::vector<td::Ref<BlockData>>& prev_block_data) {
    if (prev_block_data.size() != 1 || prev_block_data[0]->block_id().shard_full() != owning_bus()->shard) {
      return false;
    }
    auto result = get_before_split(prev_block_data[0]);
    if (result.is_error()) {
      LOG(INFO) << "Failed to get before_split of the previous block: " << result.move_as_error();
      return false;
    }
    return result.move_as_ok();
  }

  bool should_generate_empty_block(BlockSeqno new_seqno, const std::vector<td::Ref<BlockData>>& prev_block_data) {
    if (is_before_split(prev_block_data)) {
      return true;
    }
    if (owning_bus()->shard.is_masterchain()) {
      return last_consensus_finalized_seqno_ + 1 < new_seqno;
    } else {
      return last_mc_finalized_seqno_ + 8 < new_seqno;
    }
  }

  td::actor::Task<> generate_candidates(std::shared_ptr<const OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return {};
    }

    td::Timestamp target_time = event->start_time;

    CandidateParent parent{bus, event->base};

    td::uint32 slot = event->start_slot;

    std::vector<td::Ref<vm::Cell>> prev_block_state_roots = event->prev_block_state_roots;
    std::vector<td::Ref<BlockData>> prev_block_data = event->prev_block_data;

    while (current_leader_window_ == window && slot < event->end_slot) {
      co_await td::actor::coro_sleep(target_time);

      BlockSeqno new_seqno = parent.next_seqno();

      CandidateHashData hash_builder;
      std::variant<BlockIdExt, BlockCandidate> block;
      std::optional<adnl::AdnlNodeIdShort> collator;

      owning_bus().publish<StatsTargetReached>(StatsTargetReached::CollateStarted, slot);

      if (should_generate_empty_block(new_seqno, prev_block_data)) {
        LOG(WARNING) << "Generating an empty block for slot " << slot << "! new_seqno=" << new_seqno
                     << ", last_consensus_finalized_seqno_=" << last_consensus_finalized_seqno_
                     << ", last_mc_finalized_seqno_=" << last_mc_finalized_seqno_;
        CHECK(parent.id().has_value());  // first generated block in an epoch cannot be empty

        hash_builder = CandidateHashData::create_empty(parent.id()->block, *parent.id());
        block = parent.id()->block;
      } else {
        // Before doing anything substantial, check the leader window.
        if (current_leader_window_ != window) {
          break;
        }

        // FIXME: What to do if collate_block suddenly fails?
        CollateParams params{
            .shard = bus.shard,
            .min_masterchain_block_id = bus.min_masterchain_block_id,
            .prev = parent.parent_blocks(),
            .creator = Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()},
            .prev_block_data = prev_block_data,
            .prev_block_state_roots = prev_block_state_roots,
            .is_new_consensus = true,
        };
        auto block_candidate = co_await td::actor::ask(bus.manager, &ManagerFacade::collate_block, std::move(params),
                                                       cancellation_source_.get_cancellation_token());

        if (!prev_block_state_roots.empty()) {
          auto [new_state_root, new_block_data] =
              co_await apply_block_to_state(prev_block_state_roots, block_candidate.candidate);
          prev_block_state_roots = {new_state_root};
          prev_block_data = {new_block_data};
        }
        hash_builder = CandidateHashData::create_full(block_candidate.candidate, parent.id());
        block = std::move(block_candidate.candidate);
        if (!block_candidate.collator_node_id.is_zero()) {
          collator = adnl::AdnlNodeIdShort{block_candidate.collator_node_id};
        }
      }

      auto id = CandidateId::create(slot, hash_builder);
      auto id_to_sign = serialize_tl_object(id.as_raw().to_tl(), true);
      auto data_to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));

      auto candidate =
          td::make_ref<RawCandidate>(id, parent.id(), bus.local_id.idx, std::move(block), std::move(signature));

      owning_bus().publish<StatsTargetReached>(StatsTargetReached::CollateFinished, slot);

      if (current_leader_window_ != window) {
        break;
      }
      owning_bus().publish<CandidateGenerated>(candidate, collator);
      owning_bus().publish<CandidateReceived>(candidate);

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
