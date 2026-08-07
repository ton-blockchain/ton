/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <functional>

#include "bus.h"

namespace ton::validator::consensus {

struct EmptyBlockPolicy {
  void observe_session_start(BlockSeqno seqno) {
    last_mc_finalized_seqno = std::max(last_mc_finalized_seqno, seqno);
    last_consensus_finalized_seqno = std::max(last_consensus_finalized_seqno, seqno);
    last_consensus_finalized_at = td::Timestamp::now();
  }

  void observe_consensus_finalized(BlockSeqno seqno) {
    last_consensus_finalized_seqno = std::max(last_consensus_finalized_seqno, seqno);
    last_consensus_finalized_at = td::Timestamp::now();
  }

  void observe_mc_finalized(BlockSeqno seqno) {
    last_mc_finalized_seqno = std::max(seqno, last_mc_finalized_seqno);
    last_consensus_finalized_seqno = std::max(last_mc_finalized_seqno, last_consensus_finalized_seqno);
  }

  bool should_generate_empty_block(bool is_masterchain, const ChainStateRef& state) const {
    if (state->is_before_split()) {
      return true;
    }
    if (is_masterchain) {
      return last_consensus_finalized_seqno + 1 < state->next_seqno();
    } else {
      return last_mc_finalized_seqno + 8 < state->next_seqno();
    }
  }

  bool allow_empty_on_generation_failure(std::chrono::milliseconds no_empty_blocks_on_error_timeout) const {
    return !(last_consensus_finalized_at + no_empty_blocks_on_error_timeout).is_in_past();
  }

  BlockSeqno last_consensus_finalized_seqno = 0;
  BlockSeqno last_mc_finalized_seqno = 0;
  td::Timestamp last_consensus_finalized_at;
};

struct ProduceWindowContext {
  ParentId base;
  ChainStateRef state;
  td::uint32 start_slot;
  td::uint32 end_slot;
  td::Timestamp start_time;

  PeerValidator leader;
  PublicKeyHash signing_key;
  std::optional<Delegation> delegation;
  std::optional<adnl::AdnlNodeIdShort> collator_node_id;

  std::chrono::milliseconds target_rate;
  td::CancellationToken cancellation_token;

  std::function<bool()> is_superseded;
  std::function<bool(const ChainStateRef&)> should_generate_empty_block;
  std::function<bool()> allow_empty_on_generation_failure;
};

td::actor::Task<> produce_window(BusHandle bus_handle, ProduceWindowContext ctx);

}  // namespace ton::validator::consensus
