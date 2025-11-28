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

class Bus : public consensus::Bus {
 public:
  using Parent = consensus::Bus;
  using Events = td::TypeList<>;

  Bus() = default;
};

using BusHandle = runtime::BusHandle<Bus>;

struct Consensus {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator::consensus::null
