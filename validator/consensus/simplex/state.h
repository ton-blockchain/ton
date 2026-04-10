/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <map>
#include <memory>
#include <optional>

#include "td/utils/int_types.h"

namespace ton::validator::consensus::simplex {

template <typename SlotState, typename SlotParams>
  requires std::is_constructible_v<SlotState, const SlotParams&>
class ConsensusState {
 public:
  struct Slot : SlotState {
    Slot(const SlotParams& params) : SlotState(params) {
    }
  };

  ConsensusState(SlotParams slot_constructor_params) : slot_constructor_params_(std::move(slot_constructor_params)) {
  }

  struct SlotRef {
    td::uint32 i;
    std::shared_ptr<Slot> state;
  };

  std::optional<SlotRef> slot_at(td::uint32 slot) {
    if (slot < first_non_finalized_slot_) {
      return std::nullopt;
    }
    auto it = slots_.find(slot);
    if (it == slots_.end()) {
      it = slots_.emplace(slot, std::make_shared<Slot>(slot_constructor_params_)).first;
    }
    return SlotRef{
        .i = slot,
        .state = it->second,
    };
  }

  void notify_finalized(td::uint32 slot) {
    first_non_finalized_slot_ = std::max(first_non_finalized_slot_, slot + 1);
    while (!slots_.empty() && slots_.begin()->first < first_non_finalized_slot_) {
      slots_.erase(slots_.begin());
    }
  }

  struct TrackedSlotsInterval {
    td::uint32 begin;
    td::uint32 end;
  };

  TrackedSlotsInterval tracked_slots_interval() const {
    return {
        .begin = first_non_finalized_slot_,
        .end = slots_.empty() ? first_non_finalized_slot_ : slots_.rbegin()->first + 1,
    };
  }

 private:
  td::uint32 slots_per_leader_window_;
  td::uint32 max_leader_window_desync_;

  SlotParams slot_constructor_params_;

  td::uint32 first_non_finalized_slot_ = 0;

  td::uint32 offset_ = 0;
  std::map<td::uint32, std::shared_ptr<Slot>> slots_;
};

}  // namespace ton::validator::consensus::simplex
