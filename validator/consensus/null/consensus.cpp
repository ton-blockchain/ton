/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"

namespace ton::validator::consensus::null {

namespace {

struct SlotState {
  SlotState(size_t validator_set_size) : signed_by(validator_set_size) {
  }

  void add_signature(const PeerValidator& validator, td::BufferSlice signature) {
    if (finalized || signed_by[validator.idx.value()]) {
      return;
    }

    signed_by[validator.idx.value()] = true;
    signatures.emplace_back(validator.short_id.bits256_value(), std::move(signature));
    total_signed_weight += validator.weight;
  }

  std::optional<RawCandidateRef> raw_candidate;
  std::optional<CandidateRef> candidate;

  std::vector<BlockSignature> signatures;
  ValidatorWeight total_signed_weight = 0;
  std::vector<bool> signed_by;

  bool validated = false;
  bool finalized = false;
};

class ConsensusImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();

    validator_count_ = bus.validator_set.size();
    ValidatorWeight total_weight = 0;
    for (const auto& validator : bus.validator_set) {
      total_weight += validator.weight;
    }
    weight_threshold_ = (total_weight * 2) / 3 + 1;

    leader_ = bus.validator_set[0].idx;
    is_leader_ = bus.local_id.idx == leader_;

    if (bus.validator_set.size() == 1) {
      try_start_generation();
    } else if (is_leader_) {
      send_message({}, create_tl_object<tl::handshake>());
    } else {
      send_message(leader_, create_tl_object<tl::handshake>());
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    on_new_candidate(event->candidate).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const IncomingProtocolMessage> event) {
    auto message = fetch_tl_object<tl::Message>(event->message.data, true).move_as_ok();

    ton_api::downcast_call(*message, [&](auto& message) { handle_message(event->source, message); });
  }

 private:
  void send_message(std::optional<PeerValidatorId> recipient, ProtocolMessage message) {
    owning_bus().publish<OutgoingProtocolMessage>(recipient, std::move(message));
  }

  SlotState& get_or_create_slot_state(td::uint32 slot) {
    CHECK(next_slot_to_finalize_ <= slot);

    auto it = block_states_.find(slot);
    if (it == block_states_.end()) {
      return block_states_.emplace(slot, SlotState(validator_count_)).first->second;
    } else {
      return it->second;
    }
  }

  void handle_message(PeerValidatorId source, tl::handshake& handshake) {
    if (is_leader_) {
      if (seen_handshakes_.insert(source).second) {
        try_start_generation();
      }
    } else {
      send_message(leader_, create_tl_object<tl::handshake>());
    }
  }

  void handle_message(PeerValidatorId source, tl::signature& signature) {
    auto slot = static_cast<td::uint32>(signature.slot_);

    if (next_slot_to_finalize_ > slot) {
      return;
    }

    SlotState& state = get_or_create_slot_state(slot);
    state.add_signature(source.get_using(*owning_bus()), std::move(signature.signature_));
    try_finalize_blocks();
  }

  void try_start_generation() {
    if (seen_handshakes_.size() == validator_count_ - 1) {
      owning_bus().publish<OurLeaderWindowStarted>(std::nullopt, 0, std::numeric_limits<td::uint32>::max());
    }
  }

  td::actor::Task<> on_new_candidate(RawCandidateRef candidate) {
    SlotState& state = get_or_create_slot_state(candidate->id.slot);
    CHECK(!state.raw_candidate.has_value());
    state.raw_candidate = candidate;

    co_await try_validate_blocks();
    try_finalize_blocks();

    co_return {};
  }

  td::actor::Task<> try_validate_blocks() {
    if (try_validate_blocks_running_) {
      co_return {};
    }
    try_validate_blocks_running_ = true;

    auto& bus = *owning_bus();

    while (true) {
      auto it = block_states_.find(next_slot_to_validate_);
      if (it == block_states_.end()) {
        break;
      }
      SlotState& state = it->second;
      CHECK(!state.validated);
      if (!state.raw_candidate.has_value()) {
        break;
      }
      const auto& raw_candidate = *state.raw_candidate;

      CHECK(raw_candidate->parent_id == parent_for_validation_);

      CandidateRef candidate = td::make_ref<Candidate>(parent_for_validation_, raw_candidate);
      state.candidate = candidate;

      auto validation_result = co_await owning_bus().publish<ValidationRequest>(candidate).wrap();
      validation_result.ensure();

      auto signature_data = create_serialize_tl_object<ton_api::ton_blockId>(candidate->id.block.root_hash,
                                                                             candidate->id.block.file_hash);
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(signature_data));

      state.add_signature(bus.local_id, signature.clone());
      state.validated = true;

      send_message({}, create_tl_object<tl::signature>(candidate->id.slot, std::move(signature)));

      ++next_slot_to_validate_;
      parent_for_validation_ = raw_candidate->id;
      if (state.finalized) {
        block_states_.erase(it);
      }
    }

    try_validate_blocks_running_ = false;
    co_return {};
  }

  void try_finalize_blocks() {
    while (true) {
      auto it = block_states_.find(next_slot_to_finalize_);
      if (it == block_states_.end()) {
        break;
      }
      SlotState& state = it->second;
      CHECK(!state.finalized);
      if (state.total_signed_weight < weight_threshold_ || !state.candidate.has_value()) {
        break;
      }
      auto& bus = owning_bus();
      bus.publish<BlockFinalized>(*state.candidate,
                                  block::BlockSignatureSet::create_ordinary(std::move(state.signatures), bus->cc_seqno,
                                                                            bus->validator_set_hash))
          .detach();
      ++next_slot_to_finalize_;
      state.finalized = true;
      if (state.validated) {
        block_states_.erase(it);
      }
    }
  }

  std::set<PeerValidatorId> seen_handshakes_;

  size_t validator_count_ = 0;
  ValidatorWeight weight_threshold_ = 0;
  PeerValidatorId leader_;
  bool is_leader_ = false;

  std::map<td::uint32, SlotState> block_states_;

  bool try_validate_blocks_running_ = false;
  ParentId parent_for_validation_;
  td::uint32 next_slot_to_validate_ = 0;
  td::uint32 next_slot_to_finalize_ = 0;
};

}  // namespace

void Consensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<ConsensusImpl>("NullConsensus");
}

}  // namespace ton::validator::consensus::null
