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
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/transaction.h"
#include "td/utils/Timer.h"
#include "vm/dict.h"

#include "ext-message-checker.hpp"
#include "fabric.h"

namespace ton::validator {

td::actor::Task<ExtMessageChecker::CheckedExtMsg> ExtMessageChecker::check(td::BufferSlice data,
                                                                           block::SizeLimitsConfig::ExtMsgLimits limits,
                                                                           td::Ref<MasterchainState> mc_state) {
  CheckedExtMsg result;
  td::Timer timer;
  auto message = co_await create_ext_message(std::move(data), limits);
  result.message = message;
  result.timings.parse = timer.elapsed();

  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  // Note: the per-address rate limit is enforced by the pool at finalization (a mid-check
  // round-trip to the pool costs more than it saves: under load it adds milliseconds of
  // pool-mailbox latency per message and idles the workers).

  timer = td::Timer();
  auto state = co_await resolve_state(mc_state, message->shard());
  result.timings.fetch_state = timer.elapsed();

  timer = td::Timer();
  vm::AugmentedDictionary accounts_dict{state.accounts_root, 256, block::tlb::aug_ShardAccounts};
  auto shard_acc = accounts_dict.lookup(addr);
  result.timings.lookup = timer.elapsed();

  timer = td::Timer();
  bool special = wc == masterchainId && config_->is_special_smartcontract(addr);
  auto unpack_account = [shard_acc, special, utime = state.utime, lt = state.lt]() -> td::Result<block::Account> {
    block::Account a;
    if (!a.unpack(shard_acc, utime, special)) {
      return td::Status::Error("Failed to unpack account state");
    }
    a.block_lt = lt;
    return std::move(a);
  };
  auto acc = co_await unpack_account();

  // NOTE: exec_config stays valid below because nothing in between actually suspends (the
  // co_awaits unwrap ready td::Result values); only this worker's tasks mutate exec_configs_.
  auto &exec_config = exec_configs_[{wc, state.utime}];
  if (exec_config.nolog == nullptr) {
    exec_config.nolog = co_await ExtMessageQ::ExecutionConfig::create(*config_, wc, state.utime, false);
    exec_config.log = co_await ExtMessageQ::ExecutionConfig::create(*config_, wc, state.utime, true);
    if (exec_configs_.size() > 16) {
      std::erase_if(exec_configs_,
                    [&](const auto &kv) { return kv.second.nolog == nullptr || kv.first.second + 60 < state.utime; });
    }
  }

  const WalletMessageProcessor *wallet =
      acc.code.not_null() ? WalletMessageProcessor::get(acc.code->get_hash().bits()) : nullptr;
  if (wallet == nullptr) {
    co_await run_message(wc, std::move(acc), unpack_account, state.utime, state.lt + 1, message->root_cell(),
                         exec_config);
    result.timings.vm = timer.elapsed();
    co_return result;
  }

  LOG(DEBUG) << "Checking external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  auto wallet_seqno = co_await wallet->get_wallet_seqno(acc.data);
  auto [msg_seqno, msg_valid_until] = co_await wallet->parse_message(message->root_cell());
  LOG(DEBUG) << "External message to " << wallet->name() << ": msg_seqno=" << msg_seqno
             << ", msg_ttl=" << msg_valid_until << ", wallet_seqno=" << wallet_seqno;
  if (msg_valid_until <= (UnixTime)td::Clocks::system()) {
    co_return td::Status::Error("valid_until is in the past");
  }
  if (msg_seqno < wallet_seqno) {
    co_return td::Status::Error(PSTRING()
                                << "Too old seqno: msg_seqno=" << msg_seqno << ", wallet_seqno=" << wallet_seqno);
  }
  if (msg_seqno - wallet_seqno > MAX_WALLET_SEQNO_DIFF) {
    co_return td::Status::Error(PSTRING()
                                << "Too new seqno: msg_seqno=" << msg_seqno << ", wallet_seqno=" << wallet_seqno);
  }
  // Note: the duplicate-seqno check against other in-flight messages is pool state; the pool
  // performs it at finalization (after this VM run instead of before it — same admission verdict).
  acc.data = co_await wallet->set_wallet_seqno(acc.data, msg_seqno);
  acc.storage_dict_hash = acc.orig_storage_dict_hash = {};
  auto unpack_wallet_account = [&unpack_account, wallet, msg_seqno]() -> td::Result<block::Account> {
    TRY_RESULT(a, unpack_account());
    TRY_RESULT_ASSIGN(a.data, wallet->set_wallet_seqno(a.data, msg_seqno));
    a.storage_dict_hash = a.orig_storage_dict_hash = {};
    return std::move(a);
  };
  co_await run_message(wc, std::move(acc), unpack_wallet_account, state.utime, state.lt + 1, message->root_cell(),
                       exec_config);
  result.timings.vm = timer.elapsed();
  result.is_wallet = true;
  result.msg_seqno = msg_seqno;
  result.msg_valid_until = msg_valid_until;
  result.wallet_seqno = wallet_seqno;
  result.state_utime = state.utime;
  LOG(DEBUG) << "Checked external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  co_return result;
}

td::Status ExtMessageChecker::run_message(WorkchainId wc, block::Account acc,
                                          const std::function<td::Result<block::Account>()> &rebuild_account,
                                          UnixTime utime, LogicalTime lt, const td::Ref<vm::Cell> &msg_root,
                                          ExecConfigPair &exec_config) {
  auto status = ExtMessageQ::run_message_on_account(wc, &acc, utime, lt, msg_root, *exec_config.nolog);
  if (status.is_ok()) {
    return status;
  }
  // Rejected: re-run a pristine account with VM logging so the error carries the same detail as
  // before (the VM is deterministic; only the log differs). Costs a second run on the rejection
  // path only, which is bounded by the admission in-flight cap.
  auto r_acc = rebuild_account();
  if (r_acc.is_error()) {
    return status;
  }
  auto acc_retry = r_acc.move_as_ok();
  auto status_with_log = ExtMessageQ::run_message_on_account(wc, &acc_retry, utime, lt, msg_root, *exec_config.log);
  if (status_with_log.is_error()) {
    return status_with_log;
  }
  return status;
}

td::actor::Task<ExtMessageChecker::ResolvedState> ExtMessageChecker::resolve_state(td::Ref<MasterchainState> mc_state,
                                                                                   AccountIdPrefixFull prefix) {
  // Refresh the per-mc-block config. Extracted once per masterchain block per worker instead of
  // once per message (the old LiteQuery-based fetch re-extracted the full config every message).
  if (config_ == nullptr || config_mc_block_id_ != mc_state->get_block_id()) {
    config_ = co_await block::ConfigInfo::extract_config(mc_state->root_cell(), mc_state->get_block_id(), 0xFFFF);
    config_mc_block_id_ = mc_state->get_block_id();
    exec_configs_.clear();
  }

  BlockIdExt block_id;
  if (prefix.workchain == masterchainId) {
    block_id = mc_state->get_block_id();
  } else {
    auto shard_hash = mc_state->get_shard_from_config(shard_prefix(prefix, 60), false);
    if (shard_hash.is_null()) {
      co_return td::Status::Error(ErrorCode::notready, PSTRING() << "no shard in masterchain state for account "
                                                                 << prefix.workchain << ":"
                                                                 << td::format::as_hex(prefix.account_id_prefix));
    }
    block_id = shard_hash->top_block_id();
  }

  auto make_resolved = [](const CachedState &entry) {
    return ResolvedState{entry.accounts_root, entry.utime, entry.lt};
  };
  {
    auto it = states_.find(block_id.shard_full());
    if (it != states_.end() && it->second.block_id == block_id) {
      co_return make_resolved(it->second);
    }
  }

  td::Ref<ShardState> state;
  if (prefix.workchain == masterchainId) {
    state = mc_state;
  } else {
    state = co_await td::actor::ask(manager_, &ValidatorManager::wait_block_state_short, block_id, (td::uint32)0,
                                    td::Timestamp::in(10.0), false);
  }
  // We may have suspended above: another in-flight check on this worker may have already
  // populated the cache entry in the meantime.
  auto &entry = states_[block_id.shard_full()];
  if (entry.block_id != block_id || entry.state.is_null()) {
    block::gen::ShardStateUnsplit::Record sstate;
    if (!tlb::unpack_cell(state->root_cell(), sstate)) {
      co_return td::Status::Error("cannot unpack shard state header");
    }
    entry = CachedState{block_id, std::move(state), vm::load_cell_slice_ref(sstate.accounts), sstate.gen_utime,
                        sstate.gen_lt};
    if (states_.size() > 64) {
      // Drop stale shards (after splits/merges); the hot entries repopulate on the next message.
      std::erase_if(states_, [&](const auto &kv) { return kv.second.block_id != block_id; });
    }
  }
  co_return make_resolved(entry);
}

}  // namespace ton::validator
