/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <memory>
#include <vector>

#include "common/refint.h"
#include "td/actor/coro_task.h"
#include "ton/ton-types.h"
#include "validator/interfaces/shard.h"
#include "vm/dict.h"

namespace block {
struct EnqueuedMsgDescr;
}

namespace ton::validator::detail {

struct ParsedShardState {
  BlockIdExt block_id;
  std::unique_ptr<vm::AugmentedDictionary> msg_queue;
  std::unique_ptr<vm::AugmentedDictionary> dispatch_queue;  // optional - only if queue balance is not stored in state
  td::Ref<vm::CellSlice> proc_info;
  td::RefInt256 accounts_balance = td::zero_refint();
  td::RefInt256 dispatch_queue_balance = td::zero_refint();
  td::RefInt256 mc_total_validator_fees = td::zero_refint();
  bool dispatch_queue_balance_stored = false;

  static td::actor::Task<std::shared_ptr<ParsedShardState>> fetch(
      td::Ref<ShardState> state, td::Ref<vm::Cell> block_root, int global_version,
      std::vector<std::shared_ptr<ParsedShardState>> prev = {});
};

td::Result<td::RefInt256> calculate_dispatch_queue_balance(vm::AugmentedDictionary& dispatch_queue, int global_version);
td::Result<td::RefInt256> calculate_dispatch_queue_balance_diff(vm::AugmentedDictionary& old_queue,
                                                                vm::AugmentedDictionary& new_queue, int global_version);
td::Result<td::RefInt256> get_out_queue_message_balance(const block::EnqueuedMsgDescr& msg, int global_version);
td::Result<std::unique_ptr<vm::AugmentedDictionary>> prune_message_queue(vm::AugmentedDictionary& msg_queue);
td::Result<std::unique_ptr<vm::AugmentedDictionary>> update_message_queue(
    const std::vector<std::shared_ptr<ParsedShardState>>& prev, td::Ref<vm::Cell> block_root, BlockIdExt block_id);
td::Result<td::RefInt256> get_dispatch_queue_balance(vm::AugmentedDictionary& dispatch_queue,
                                                     const std::vector<std::shared_ptr<ParsedShardState>>& prev,
                                                     BlockIdExt block_id, int global_version);

}  // namespace ton::validator::detail
