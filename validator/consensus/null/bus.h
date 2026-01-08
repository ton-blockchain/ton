/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "validator/consensus/bus.h"

namespace ton::validator::consensus::null {

namespace tl {

using signature = ton_api::consensus_null_signature;
using handshake = ton_api::consensus_null_handshake;
using Message = ton_api::consensus_null_Message;
using MessageRef = tl_object_ptr<Message>;

}  // namespace tl

class StaticCollatorSchedule : public CollatorSchedule {
 public:
  PeerValidatorId expected_collator_for(td::uint32 slot) const override {
    return PeerValidatorId{0};
  }
};

class Bus : public consensus::Bus {
 public:
  using Parent = consensus::Bus;
  using Events = td::TypeList<>;

  Bus() = default;

  void populate_collator_schedule() override {
    collator_schedule = td::make_ref<StaticCollatorSchedule>();
  }
};

using BusHandle = runtime::BusHandle<Bus>;

struct Consensus {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator::consensus::null
