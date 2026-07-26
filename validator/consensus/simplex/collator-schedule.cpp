/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"

namespace ton::validator::consensus::simplex {

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

void provide_schedule(Bus& bus) {
  auto validators = static_cast<td::uint32>(bus.validator_set.size());
  bus.collator_schedule = td::make_ref<SimplexCollatorSchedule>(bus.config.slots_per_leader_window, validators);
}

}  // namespace

void DefaultCollatorSchedule::provide_for(td::actor::Runtime& runtime) {
  runtime.register_provider(provide_schedule);
}

}  // namespace ton::validator::consensus::simplex
