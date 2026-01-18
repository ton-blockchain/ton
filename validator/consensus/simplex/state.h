/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <deque>
#include <memory>
#include <optional>

#include "td/utils/int_types.h"

namespace ton::validator::consensus::simplex {

template <typename WindowState, typename SlotState, typename WindowParams, typename SlotParams>
  requires std::is_constructible_v<WindowState, const WindowParams&> &&
           std::is_constructible_v<SlotState, const SlotParams&>
class ConsensusState {
 public:
  struct Slot : SlotState {
    Slot(const SlotParams& params) : SlotState(params) {
    }
  };

  struct Window : WindowState {
    Window(const WindowParams& params) : WindowState(params) {
    }

    std::unique_ptr<std::shared_ptr<Slot>[]> slots;
  };

  ConsensusState(td::uint32 slots_per_leader_window, WindowParams window_constructor_params,
                 SlotParams slot_constructor_params)
      : slots_per_leader_window_(slots_per_leader_window)
      , window_constructor_params_(std::move(window_constructor_params))
      , slot_constructor_params_(std::move(slot_constructor_params)) {
  }

  std::optional<std::shared_ptr<Window>> window_at(td::uint32 slot) {
    if (first_non_finalized_slot_ > slot) {
      return std::nullopt;
    }

    td::uint32 window = slot / slots_per_leader_window_;

    while (offset_ + windows_.size() <= window) {
      auto new_window = std::make_shared<Window>(window_constructor_params_);
      new_window->slots = std::make_unique<std::shared_ptr<Slot>[]>(slots_per_leader_window_);
      for (td::uint32 i = 0; i < slots_per_leader_window_; ++i) {
        new_window->slots[i] = std::make_shared<Slot>(slot_constructor_params_);
      }
      windows_.push_back(std::move(new_window));
    }

    return windows_[window - offset_];
  }

  struct SlotRef {
    td::uint32 i;
    bool is_first_in_window = false;
    bool is_last_in_window = false;
    std::shared_ptr<Window> window;
    std::shared_ptr<Slot> state;
  };

  std::optional<SlotRef> slot_at(td::uint32 slot) {
    auto window = window_at(slot);
    if (!window) {
      return std::nullopt;
    }
    auto slot_in_window = slot % slots_per_leader_window_;
    return SlotRef{
        .i = slot,
        .is_first_in_window = slot_in_window == 0,
        .is_last_in_window = slot_in_window + 1 == slots_per_leader_window_,
        .window = *window,
        .state = (*window)->slots[slot_in_window],
    };
  }

  void notify_finalized(td::uint32 slot) {
    first_non_finalized_slot_ = std::max(first_non_finalized_slot_, slot + 1);
    td::uint32 needed_window = first_non_finalized_slot_ / slots_per_leader_window_;

    while (!windows_.empty() && offset_ < needed_window) {
      windows_.pop_front();
      ++offset_;
    }
    if (offset_ < needed_window) {
      offset_ = needed_window;
    }
  }

  struct TrackedSlotsInterval {
    td::uint32 begin;
    td::uint32 end;
  };

  TrackedSlotsInterval tracked_slots_interval() const {
    return {
        .begin = first_non_finalized_slot_,
        .end = (offset_ + static_cast<td::uint32>(windows_.size())) * slots_per_leader_window_,
    };
  }

 private:
  td::uint32 slots_per_leader_window_;
  td::uint32 max_leader_window_desync_;

  WindowParams window_constructor_params_;
  SlotParams slot_constructor_params_;

  td::uint32 first_non_finalized_slot_ = 0;

  td::uint32 offset_ = 0;
  std::deque<std::shared_ptr<Window>> windows_;
};

}  // namespace ton::validator::consensus::simplex
