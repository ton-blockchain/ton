/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"

namespace ton::validator::consensus::simplex {

std::string BroadcastVote::contents_to_string() const {
  return PSTRING() << "{vote=" << vote << "}";
}

std::string NotarizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string FinalizationObserved::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

std::string LeaderWindowObserved::contents_to_string() const {
  return PSTRING() << "{start_slot=" << start_slot << ", base=" << base << "}";
}

std::string WaitForParent::contents_to_string() const {
  return PSTRING() << "{id=" << candidate->id << ", parent=" << candidate->parent_id << "}";
}

std::string ResolveCandidate::contents_to_string() const {
  return PSTRING() << "{id=" << id << "}";
}

namespace {

class SimplexCollatorSchedule : public CollatorSchedule {
 public:
  SimplexCollatorSchedule(td::uint32 slots_per_leader_window, td::uint32 leaders_count)
      : slots_per_leader_window_(slots_per_leader_window), leaders_count_(leaders_count) {
  }

  PeerValidatorId expected_collator_for(td::uint32 slot) const override {
    return PeerValidatorId{slot / slots_per_leader_window_ % leaders_count_};
  }

 private:
  td::uint32 slots_per_leader_window_;
  td::uint32 leaders_count_;
};

}  // namespace

void Bus::populate_collator_schedule() {
  auto validators = static_cast<td::uint32>(validator_set.size());
  collator_schedule = td::make_ref<SimplexCollatorSchedule>(simplex_config.slots_per_leader_window, validators);
}

void Bus::load_bootstrap_state() {
  auto pool_state_str = db->get(create_serialize_tl_object<ton_api::consensus_simplex_db_key_poolState>());
  if (pool_state_str.has_value()) {
    auto pool_state =
        fetch_tl_object<ton_api::consensus_simplex_db_poolState>(*pool_state_str, true).ensure().move_as_ok();
    first_nonannounced_window = pool_state->first_nonannounced_window_;
  }

  auto votes = db->get_by_prefix(ton_api::consensus_simplex_db_key_vote::ID);
  for (auto &[_, data] : votes) {
    auto f = fetch_tl_object<ton_api::consensus_simplex_db_vote>(data, true).ensure().move_as_ok();
    PeerValidatorId validator{static_cast<size_t>(f->node_idx_)};
    bootstrap_votes.push_back(Signed<Vote>::deserialize(f->data_, validator, *this).move_as_ok());
  }
}

}  // namespace ton::validator::consensus::simplex
