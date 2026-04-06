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
#include "td/utils/PersistentTreap.h"
#include "td/utils/Random.h"

#include "ext-message-pool.hpp"
#include "external-message.hpp"
#include "fabric.h"
#include "transaction.h"

namespace ton::validator {
class ExtMsgSnapshotImpl final : public ExtMsgSnapshot {
 public:
  struct MessageId {
    AccountIdPrefixFull dst;
    ExtMessage::Hash hash;
    bool operator<(const MessageId &o) const {
      return dst < o.dst ? true : o.dst < dst ? false : hash < o.hash;
    }
  };
  using Treap = td::PersistentTreap<MessageId, td::Ref<ExtMessage>>;

  bool empty() const override {
    return buckets_.empty();
  }

  void push(td::Ref<ExtMessage> message, int priority) override {
    auto &treap = buckets_[priority];
    treap = treap.insert({message->shard(), message->hash()}, std::move(message));
  }

  std::optional<std::pair<td::Ref<ExtMessage>, int>> try_pop() override {
    if (empty()) {
      return std::nullopt;
    }
    auto it = buckets_.begin();
    auto &[priority, treap] = *it;
    auto [key, message] = treap.at(td::Random::fast_uint32() % treap.size());
    treap = treap.erase(key);
    if (treap.empty()) {
      buckets_.erase(it);
    }
    return std::make_pair(std::move(message), priority);
  }

  void erase(td::Ref<ExtMessage> message, int priority) override {
    MessageId id{message->shard(), message->hash()};
    auto it = buckets_.find(priority);
    if (it == buckets_.end() || !it->second.find(id)) {
      return;
    }
    it->second = it->second.erase(id);
    if (it->second.empty()) {
      buckets_.erase(it);
    }
  }

  std::unique_ptr<ExtMsgSnapshot> slice(ShardIdFull shard) const override {
    td::uint64 lo = shard.shard & (shard.shard - 1);
    td::uint64 hi = (shard.shard | (shard.shard - 1)) + 1;
    MessageId shard_lo{AccountIdPrefixFull{shard.workchain, lo}, Bits256::zero()};
    MessageId shard_hi{AccountIdPrefixFull{hi == 0 ? shard.workchain + 1 : shard.workchain, hi}, Bits256::zero()};

    auto snapshot = std::make_unique<ExtMsgSnapshotImpl>();
    for (const auto &[priority, messages] : buckets_) {
      auto [_, in_shard, __] = messages.split_range(shard_lo, shard_hi);
      if (!in_shard.empty()) {
        snapshot->buckets_.emplace(priority, std::move(in_shard));
      }
    }
    return snapshot;
  }

 private:
  std::map<int, Treap, std::greater<int>> buckets_;
};

std::unique_ptr<ExtMsgSnapshot> create_ext_msg_snapshot() {
  return std::make_unique<ExtMsgSnapshotImpl>();
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_external_message(td::BufferSlice data,
                                                                                        int priority,
                                                                                        bool add_to_mempool) {
  if (last_masterchain_state_.is_null()) {
    co_return td::Status::Error(ErrorCode::notready, "not ready");
  }
  auto message = co_await create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  td::optional<td::uint32> msg_seqno;
  auto result = co_await check_message(message, msg_seqno).wrap();
  ++(result.is_ok() ? total_check_ext_messages_ok_ : total_check_ext_messages_error_);
  if (result.is_error()) {
    co_return result.move_as_error();
  }
  if (checked_ext_msg_counter_.inc_msg_count(wc, addr) > MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  if (add_to_mempool) {
    add_message_to_mempool(message, priority, msg_seqno);
  }
  co_return result.move_as_ok();
}

void ExtMessagePool::install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) {
  reactivate_due();
  if (callback->on_snapshot) {
    callback->on_snapshot(ext_msgs_->slice(shard));
  }

  alarm_timestamp().relax(callback->timeout);
  callbacks_.push_back(std::move(callback));
}

void ExtMessagePool::cleanup_external_messages(ShardIdFull shard) {
  std::vector<ExtMessage::Hash> to_erase;
  for (const auto &[hash, msg] : ext_msgs_info_) {
    if (shard_contains(shard, msg.message->shard()) && msg.expired()) {
      to_erase.push_back(hash);
    }
  }
  for (auto &hash : to_erase) {
    erase_message(hash);
  }
}

void ExtMessagePool::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                std::vector<ExtMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    erase_message(hash);
  }
  for (auto &hash : to_delay) {
    auto it = ext_msgs_info_.find(hash);
    if (it == ext_msgs_info_.end()) {
      continue;
    }
    auto &msg = it->second;
    if (ext_msgs_total_[msg.priority] < SOFT_MEMPOOL_LIMIT && msg.can_postpone()) {
      msg.postpone();
      ext_msgs_->erase(msg.message, msg.priority);
      reactivate_queue_.emplace(msg.reactivate_at, hash);
      alarm_timestamp().relax(msg.reactivate_at);
    } else {
      erase_message(hash);
    }
  }
  reactivate_due();
}

void ExtMessagePool::erase_external_messages(std::vector<ExtMessage::Hash> to_delete) {
  applied_ext_msgs_delete_requests_ += to_delete.size();
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_norm_.find(hash);
    if (it != ext_messages_hashes_norm_.end()) {
      auto raw_hashes = it->second;
      for (const auto &raw_hash : raw_hashes) {
        if (erase_message(raw_hash)) {
          ++applied_ext_msgs_deleted_;
        }
      }
    }
  }
}

bool ExtMessagePool::erase_message(ExtMessage::Hash hash) {
  auto it = ext_msgs_info_.find(hash);
  if (it == ext_msgs_info_.end()) {
    return false;
  }
  auto message = it->second.message;
  auto priority = it->second.priority;
  auto address = it->second.address();
  auto hash_norm = it->second.hash_norm;
  ext_msgs_info_.erase(it);

  ext_msgs_->erase(message, priority);
  auto it_priority = ext_addr_messages_.find(priority);
  CHECK(it_priority != ext_addr_messages_.end());
  auto it_addr = it_priority->second.find(address);
  CHECK(it_addr != it_priority->second.end());
  CHECK(it_addr->second.erase(hash));
  if (it_addr->second.empty()) {
    it_priority->second.erase(it_addr);
    if (it_priority->second.empty()) {
      ext_addr_messages_.erase(it_priority);
    }
  }
  if (auto it_norm = ext_messages_hashes_norm_.find(hash_norm); it_norm != ext_messages_hashes_norm_.end()) {
    it_norm->second.erase(hash);
    if (it_norm->second.empty()) {
      ext_messages_hashes_norm_.erase(it_norm);
    }
  }
  CHECK(ext_msgs_total_[priority] > 0);
  if (--ext_msgs_total_[priority] == 0) {
    ext_msgs_total_.erase(priority);
  }
  return true;
}

void ExtMessagePool::notify_callbacks_add(td::Ref<ExtMessage> message, int priority) {
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->cancellation_token.check().is_error()) {
      return true;
    }
    if (shard_contains(callback->shard, message->shard()) && callback->on_message) {
      callback->on_message(message, priority);
    }
    return false;
  });
}

void ExtMessagePool::reactivate_due() {
  for (auto it = reactivate_queue_.begin(); it != reactivate_queue_.end();) {
    if (!it->first.is_in_past()) {
      alarm_timestamp().relax(it->first);
      return;
    }
    auto hash = it->second;
    it = reactivate_queue_.erase(it);
    auto it_info = ext_msgs_info_.find(hash);
    if (it_info == ext_msgs_info_.end()) {
      continue;
    }
    auto &msg = it_info->second;
    if (msg.expired()) {
      erase_message(hash);
      continue;
    }
    if (!msg.try_reactivate()) {
      continue;
    }
    ext_msgs_->push(msg.message, msg.priority);
    notify_callbacks_add(msg.message, msg.priority);
  }
}

std::vector<std::pair<std::string, std::string>> ExtMessagePool::prepare_stats() {
  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("total.ext_msg_check",
                   PSTRING() << "ok:" << total_check_ext_messages_ok_ << " error:" << total_check_ext_messages_error_);
  vec.emplace_back("total.ext_msg_applied_cleanup", PSTRING() << "requested:" << applied_ext_msgs_delete_requests_
                                                              << " deleted:" << applied_ext_msgs_deleted_);
  return vec;
}

void ExtMessagePool::alarm() {
  reactivate_due();
  if (cleanup_mempool_at_.is_in_past()) {
    cleanup_external_messages(ShardIdFull{masterchainId, shardIdAll});
    cleanup_external_messages(ShardIdFull{basechainId, shardIdAll});
    cleanup_mempool_at_ = td::Timestamp::in(250.0);
  }
  alarm_timestamp().relax(cleanup_mempool_at_);
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->timeout && callback->timeout.is_in_past()) {
      return true;
    }
    alarm_timestamp().relax(callback->timeout);
    return false;
  });
}

void ExtMessagePool::add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                            td::optional<td::uint32> msg_seqno) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  if (auto it_total = ext_msgs_total_.find(priority);
      it_total != ext_msgs_total_.end() && it_total->second > opts_->max_mempool_num()) {
    LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
              << " to mempool: mempool is full (limit=" << opts_->max_mempool_num() << ")";
    return;
  }
  auto address = std::make_pair(wc, addr);
  auto it_priority = ext_addr_messages_.find(priority);
  if (it_priority != ext_addr_messages_.end()) {
    if (auto it_addr = it_priority->second.find(address);
        it_addr != it_priority->second.end() && it_addr->second.size() >= PER_ADDRESS_LIMIT) {
      LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                << " to mempool: per address limit reached (limit=" << PER_ADDRESS_LIMIT << ")";
      return;
    }
  }
  auto hash = message->hash();
  auto it2 = ext_msgs_info_.find(hash);
  if (it2 != ext_msgs_info_.end()) {
    if (it2->second.priority >= priority) {
      LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                << " to mempool: already exists";
      return;
    }
    erase_message(hash);
  }
  auto msg = MempoolMsg{message, priority};
  msg.msg_seqno = msg_seqno;
  auto hash_norm = msg.hash_norm;
  CHECK(ext_msgs_info_.emplace(hash, std::move(msg)).second);
  ext_msgs_->push(message, priority);
  auto &addr_messages = ext_addr_messages_[priority][address];
  addr_messages.insert(hash);
  ++ext_msgs_total_[priority];
  ext_messages_hashes_norm_[hash_norm].insert(hash);
  LOG(INFO) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " to mempool";
  notify_callbacks_add(message, priority);
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_message(td::Ref<ExtMessage> message,
                                                                           td::optional<td::uint32> &msg_seqno) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto [shard_acc, utime, lt, config] = co_await run_fetch_account_state(wc, addr, manager_);
  bool special = wc == masterchainId && config->is_special_smartcontract(addr);
  block::Account acc;
  if (!acc.unpack(shard_acc, utime, special)) {
    co_return td::Status::Error(PSLICE() << "Failed to unpack account state");
  }
  acc.block_lt = lt;

  auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
  CheckResult check_result{.message = message, .wait_allow_broadcast = std::move(wait_allow_broadcast)};

  const WalletMessageProcessor *wallet =
      acc.code.not_null() ? WalletMessageProcessor::get(acc.code->get_hash().bits()) : nullptr;
  if (wallet != nullptr) {
    msg_seqno = co_await check_message_to_wallet(message, wallet, std::move(acc), utime, lt, std::move(config),
                                                 std::move(allow_broadcast_promise));
    co_return check_result;
  }
  wallets_.erase({wc, addr});
  co_await ExtMessageQ::run_message_on_account(wc, &acc, utime, lt + 1, message->root_cell(), std::move(config));
  allow_broadcast_promise.set_value(td::Unit{});
  co_return check_result;
}

td::Result<td::uint32> ExtMessagePool::check_message_to_wallet(td::Ref<ExtMessage> message,
                                                               const WalletMessageProcessor *wallet, block::Account acc,
                                                               UnixTime utime, LogicalTime lt,
                                                               std::unique_ptr<block::ConfigInfo> config,
                                                               td::Promise<td::Unit> allow_broadcast_promise) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  LOG(DEBUG) << "Checking external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  TRY_RESULT(wallet_seqno, wallet->get_wallet_seqno(acc.data));
  auto &wallet_info = wallets_[{wc, addr}];
  SCOPE_EXIT {
    if (wallet_info.messages.empty()) {
      wallets_.erase({wc, addr});
    }
  };
  wallet_info.process_messages(wallet_seqno, utime);
  TRY_RESULT(parsed_message, wallet->parse_message(message->root_cell()));
  auto [msg_seqno, msg_valid_until] = parsed_message;
  LOG(DEBUG) << "External message to " << wallet->name() << ": msg_seqno=" << msg_seqno
             << ", msg_ttl=" << msg_valid_until << ", wallet_seqno=" << wallet_seqno;
  if (msg_valid_until <= (UnixTime)td::Clocks::system()) {
    return td::Status::Error("valid_until is in the past");
  }
  if (msg_seqno < wallet_seqno) {
    return td::Status::Error(PSTRING() << "Too old seqno: msg_seqno=" << msg_seqno
                                       << ", wallet_seqno=" << wallet_seqno);
  }
  if (msg_seqno - wallet_seqno > MAX_WALLET_SEQNO_DIFF) {
    return td::Status::Error(PSTRING() << "Too new seqno: msg_seqno=" << msg_seqno
                                       << ", wallet_seqno=" << wallet_seqno);
  }
  if (wallet_info.messages.contains(msg_seqno)) {
    return td::Status::Error(PSTRING() << "Duplicate msg_seqno " << msg_seqno);
  }
  TRY_RESULT_ASSIGN(acc.data, wallet->set_wallet_seqno(acc.data, msg_seqno));
  acc.storage_dict_hash = acc.orig_storage_dict_hash = {};
  TRY_STATUS(ExtMessageQ::run_message_on_account(wc, &acc, utime, lt + 1, message->root_cell(), std::move(config)));
  wallet_info.messages[msg_seqno] =
      WalletMessageInfo{.valid_until = msg_valid_until, .allow_broadcast_promise = std::move(allow_broadcast_promise)};
  wallet_info.process_messages(wallet_seqno, utime);
  LOG(DEBUG) << "Checked external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  return msg_seqno;
}

void ExtMessagePool::WalletInfo::process_messages(td::uint32 wallet_seqno, UnixTime utime) {
  for (auto it = messages.begin(); it != messages.end();) {
    auto &[seqno, message] = *it;
    if (seqno < wallet_seqno) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error(PSTRING() << "Too old seqno: msg_seqno=" << seqno << ", wallet_seqno=" << wallet_seqno));
      }
      it = messages.erase(it);
      continue;
    }
    if (message.valid_until <= utime) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(td::Status::Error("valid_until is in the past"));
      }
      it = messages.erase(it);
      continue;
    }
    ++it;
  }
  for (td::uint32 seqno = wallet_seqno;; ++seqno) {
    auto it = messages.find(seqno);
    if (it == messages.end()) {
      break;
    }
    if (it->second.allow_broadcast_promise) {
      it->second.allow_broadcast_promise.set_value(td::Unit{});
    }
  }
}

size_t ExtMessagePool::CheckedExtMsgCounter::get_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it1 = counter_cur_.find({wc, addr});
  auto it2 = counter_prev_.find({wc, addr});
  return (it1 == counter_cur_.end() ? 0 : it1->second) + (it2 == counter_prev_.end() ? 0 : it2->second);
}

size_t ExtMessagePool::CheckedExtMsgCounter::inc_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it2 = counter_prev_.find({wc, addr});
  return (it2 == counter_prev_.end() ? 0 : it2->second) + ++counter_cur_[{wc, addr}];
}

void ExtMessagePool::CheckedExtMsgCounter::before_query() {
  while (cleanup_at_.is_in_past()) {
    counter_prev_ = std::move(counter_cur_);
    counter_cur_.clear();
    if (counter_prev_.empty()) {
      cleanup_at_ = td::Timestamp::in(MAX_EXT_MSG_PER_ADDR_TIME_WINDOW / 2.0);
      break;
    }
    cleanup_at_ += MAX_EXT_MSG_PER_ADDR_TIME_WINDOW / 2.0;
  }
}

}  // namespace ton::validator
