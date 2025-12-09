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
#include "td/utils/Random.h"

#include "ext-message-pool.hpp"
#include "fabric.h"

namespace ton::validator {

td::actor::Task<td::Ref<ExtMessage>> ExtMessagePool::check_add_external_message(td::BufferSlice data, int priority,
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
  auto result = co_await run_check_external_message(std::move(message), manager_).wrap();
  ++(result.is_ok() ? total_check_ext_messages_ok_ : total_check_ext_messages_error_);
  message = co_await std::move(result);
  if (checked_ext_msg_counter_.inc_msg_count(wc, addr) > MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }

  if (add_to_mempool) {
    auto &msgs = ext_msgs_[priority];
    if (msgs.ext_messages_.size() > opts_->max_mempool_num()) {
      VLOG(VALIDATOR_DEBUG) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                            << " to mempool: mempool is full (limit=" << opts_->max_mempool_num() << ")";
      co_return message;
    }
    auto msg = std::make_unique<MessageExt>(message);
    auto id = msg->ext_id();
    auto address = msg->address();
    auto it = msgs.ext_addr_messages_.find(address);
    if (it != msgs.ext_addr_messages_.end() && it->second.size() >= PER_ADDRESS_LIMIT) {
      VLOG(VALIDATOR_DEBUG) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                            << " to mempool: per address limit reached (limit=" << PER_ADDRESS_LIMIT << ")";
      co_return message;
    }
    auto it2 = ext_messages_hashes_.find(id.hash);
    if (it2 != ext_messages_hashes_.end()) {
      int old_priority = it2->second.first;
      if (old_priority >= priority) {
        VLOG(VALIDATOR_DEBUG) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                              << " to mempool: already exists";
        co_return message;
      }
      ext_msgs_[old_priority].erase(id);
    }
    msgs.ext_messages_.emplace(id, std::move(msg));
    msgs.ext_addr_messages_[address].emplace(id.hash, id);
    ext_messages_hashes_[id.hash] = {priority, id};
    VLOG(VALIDATOR_DEBUG) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                          << " to mempool";
  }

  co_return message;
}

std::vector<std::pair<td::Ref<ExtMessage>, int>> ExtMessagePool::get_external_messages_for_collator(ShardIdFull shard) {
  td::Timer t;
  size_t processed = 0, deleted = 0;
  std::vector<std::pair<td::Ref<ExtMessage>, int>> res;
  MessageId left{AccountIdPrefixFull{shard.workchain, shard.shard & (shard.shard - 1)}, Bits256::zero()};
  size_t total_msgs = 0;
  td::Random::Fast rnd;
  for (auto iter = ext_msgs_.rbegin(); iter != ext_msgs_.rend(); ++iter) {
    std::vector<std::pair<td::Ref<ExtMessage>, int>> cur_res;
    int priority = iter->first;
    auto &msgs = iter->second;
    auto it = msgs.ext_messages_.lower_bound(left);
    while (it != msgs.ext_messages_.end()) {
      auto s = it->first;
      if (!shard_contains(shard, s.dst)) {
        break;
      }
      ++processed;
      if (it->second->expired()) {
        msgs.ext_addr_messages_[it->second->address()].erase(it->first.hash);
        ext_messages_hashes_.erase(it->first.hash);
        it = msgs.ext_messages_.erase(it);
        ++deleted;
        continue;
      }
      if (it->second->is_active()) {
        cur_res.emplace_back(it->second->message(), priority);
      }
      ++it;
    }
    td::random_shuffle(td::as_mutable_span(cur_res), rnd);
    res.insert(res.end(), cur_res.begin(), cur_res.end());
    total_msgs += msgs.ext_messages_.size();
  }
  if (!res.empty() || deleted > 0) {
    LOG(WARNING) << "get_external_messages to shard " << shard.to_str() << " : time=" << t.elapsed()
                 << " result_size=" << res.size() << " processed=" << processed << " expired=" << deleted
                 << " total_size=" << total_msgs;
  }
  return res;
}

void ExtMessagePool::cleanup_external_messages(ShardIdFull shard) {
  get_external_messages_for_collator(shard);
}

void ExtMessagePool::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                std::vector<ExtMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      ext_msgs_[priority].erase(msg_id);
      ext_messages_hashes_.erase(it);
    }
  }
  unsigned long soft_mempool_limit = 1024;
  for (auto &hash : to_delay) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      auto &msgs = ext_msgs_[priority];
      auto it2 = msgs.ext_messages_.find(msg_id);
      if ((msgs.ext_messages_.size() < soft_mempool_limit) && it2->second->can_postpone()) {
        it2->second->postpone();
      } else {
        msgs.erase(msg_id);
        ext_messages_hashes_.erase(it);
      }
    }
  }
}

std::vector<std::pair<std::string, std::string>> ExtMessagePool::prepare_stats() {
  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("total.ext_msg_check",
                   PSTRING() << "ok:" << total_check_ext_messages_ok_ << " error:" << total_check_ext_messages_error_);
  return vec;
}

void ExtMessagePool::alarm() {
  if (cleanup_mempool_at_.is_in_past()) {
    cleanup_external_messages(ShardIdFull{masterchainId, shardIdAll});
    cleanup_external_messages(ShardIdFull{basechainId, shardIdAll});
    cleanup_mempool_at_ = td::Timestamp::in(250.0);
  }
  alarm_timestamp().relax(cleanup_mempool_at_);
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
