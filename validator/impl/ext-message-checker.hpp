/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <functional>

#include "block/mc-config.h"
#include "interfaces/validator-manager.h"
#include "td/actor/coro_utils.h"

#include "external-message.hpp"

namespace ton::validator {

// Off-pool worker for the expensive, pool-state-independent part of external message admission:
// TLB parse + size limits, account state resolution (cold celldb reads) and the full VM execution of recv_external
// (incl. the ed25519 signature check inside the VM).
//
// Several instances run in parallel on the actor scheduler; ExtMessagePool round-robins messages
// across them and keeps all shared bookkeeping (per-address counters, mempool/treap mutation, candidate holds)
// on the pool actor itself.
class ExtMessageChecker : public td::actor::Actor {
 public:
  explicit ExtMessageChecker(td::actor::ActorId<ValidatorManager> manager) : manager_(std::move(manager)) {
  }

  // Per-stage wall-clock costs of one check, reported back with every result so the pool can
  // aggregate a per-stage cost map for its periodic admission stats.
  struct StageTimings {
    double parse{0};        // TLB validation + size limits
    double fetch_state{0};  // shard state resolution (incl. waits) + per-mc-block config extraction
    double lookup{0};       // accounts dict descent (cold celldb reads land here)
    double vm{0};           // account unpack + full VM execution (incl. ed25519 inside the VM)
  };

  struct CheckedExtMsg {
    td::Ref<ExtMessage> message;
    StageTimings timings;
  };

  // Runs every admission check that does not touch pool state. The pool finalizes the result
  // (counters, mempool insertion) atomically on its own actor.
  td::actor::Task<CheckedExtMsg> check(td::BufferSlice data, block::SizeLimitsConfig::ExtMsgLimits limits,
                                       td::Ref<MasterchainState> mc_state);

  void alarm() override;

 private:
  td::actor::ActorId<ValidatorManager> manager_;

  // Per-masterchain-block caches. These are worker-local on purpose: ConfigInfo and dictionary
  // objects must not be shared across threads; the underlying state cells are shared and that
  // is safe (same pattern as collator/liteserver sharing wait_block_state results).
  BlockIdExt config_mc_block_id_;
  std::unique_ptr<block::ConfigInfo> config_;
  // Prepared transaction phase configs, keyed by (wc, state utime); cleared whenever config_
  // refreshes. Saves re-fetching all config params for every message. The no-log config is used
  // for the first VM run; rejected messages are re-run with VM logging to produce the same
  // detailed error as before (accepted messages skip the per-instruction log entirely).
  struct ExecConfigPair {
    std::unique_ptr<ExtMessageQ::ExecutionConfig> nolog, log;
  };
  std::map<std::pair<WorkchainId, UnixTime>, ExecConfigPair> exec_configs_;

  // Runs the message without VM logging; on rejection, re-runs a freshly rebuilt account with
  // logging to reconstruct the same detailed error message as before (the VM is deterministic).
  td::Status run_message(WorkchainId wc, block::Account acc,
                         const std::function<td::Result<block::Account>()> &rebuild_account, UnixTime utime,
                         LogicalTime lt, const td::Ref<vm::Cell> &msg_root, ExecConfigPair &exec_config);

  struct CachedState {
    BlockIdExt block_id;
    td::Ref<ShardState> state;
    td::Ref<vm::CellSlice> accounts_root;
    UnixTime utime{0};
    LogicalTime lt{0};
  };
  std::map<ShardIdFull, CachedState> states_;

  // Returned by value (Refs are cheap) so callers are immune to cache mutation across co_awaits.
  struct ResolvedState {
    td::Ref<vm::CellSlice> accounts_root;
    UnixTime utime{0};
    LogicalTime lt{0};
  };
  td::actor::Task<ResolvedState> resolve_state(td::Ref<MasterchainState> mc_state, AccountIdPrefixFull prefix);
};

}  // namespace ton::validator
