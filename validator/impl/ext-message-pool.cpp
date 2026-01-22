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

#include "block/block-auto.h"
#include "block/block-parse.h"

#include "ext-message-pool.hpp"
#include "external-message.hpp"
#include "fabric.h"
#include "transaction.h"

namespace ton::validator {
namespace {

td::Result<td::Bits256> compute_ext_in_msg_norm_hash(td::Ref<vm::Cell> ext_in_msg_cell) {
  block::gen::Message::Record message;
  if (!tlb::type_unpack_cell(ext_in_msg_cell, block::gen::t_Message_Any, message)) {
    return td::Status::Error("Failed to unpack Message");
  }
  auto tag = block::gen::CommonMsgInfo().get_tag(*message.info);
  if (tag != block::gen::CommonMsgInfo::ext_in_msg_info) {
    return td::Status::Error("CommonMsgInfo tag is not ext_in_msg_info");
  }
  block::gen::CommonMsgInfo::Record_ext_in_msg_info msg_info;
  if (!tlb::csr_unpack(message.info, msg_info)) {
    return td::Status::Error("Failed to unpack CommonMsgInfo::ext_in_msg_info");
  }

  td::Ref<vm::Cell> body;
  auto body_cs = message.body.write();
  if (body_cs.fetch_ulong(1) == 1) {
    body = body_cs.fetch_ref();
  } else {
    body = vm::CellBuilder().append_cellslice(body_cs).finalize();
  }

  auto cb = vm::CellBuilder();
  bool status = cb.store_long_bool(2, 2) &&  // message$_ -> info:CommonMsgInfo -> ext_in_msg_info$10
                cb.store_long_bool(0, 2) &&  // message$_ -> info:CommonMsgInfo -> src:MsgAddressExt -> addr_none$00
                cb.append_cellslice_bool(msg_info.dest) &&  // message$_ -> info:CommonMsgInfo -> dest:MsgAddressInt
                cb.store_long_bool(0, 4) &&                 // message$_ -> info:CommonMsgInfo -> import_fee:Grams -> 0
                cb.store_long_bool(0, 1) &&  // message$_ -> init:(Maybe (Either StateInit ^StateInit)) -> nothing$0
                cb.store_long_bool(1, 1) &&  // message$_ -> body:(Either X ^X) -> right$1
                cb.store_ref_bool(body);

  if (!status) {
    return td::Status::Error("Failed to build normalized message");
  }
  return cb.finalize()->get_hash().bits();
}

std::string ext_msg_norm_hash_str(td::Ref<vm::Cell> ext_in_msg_cell) {
  if (ext_in_msg_cell.is_null()) {
    return "null";
  }
  auto hash_norm_res = compute_ext_in_msg_norm_hash(std::move(ext_in_msg_cell));
  if (hash_norm_res.is_ok()) {
    return td::base64_encode(hash_norm_res.ok().as_slice());
  }
  return PSTRING() << "error:" << hash_norm_res.error().message();
}

}  // namespace
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
        auto address = it->second->address();
        auto norm_hash = ext_msg_norm_hash_str(it->second->message->root_cell());
        LOG(INFO) << "expired external message removed from mempool normalized_hash=" << norm_hash
                  << " addr=" << address.first << ":" << address.second.to_hex() << " prio=" << priority;
        msgs.ext_addr_messages_[it->second->address()].erase(it->first.hash);
        ext_messages_hashes_.erase(it->first.hash);
        it = msgs.ext_messages_.erase(it);
        ++deleted;
        continue;
      }
      if (it->second->is_active()) {
        cur_res.emplace_back(it->second->message, priority);
      }
      ++it;
    }
    td::random_shuffle(td::as_mutable_span(cur_res), rnd);
    res.insert(res.end(), cur_res.begin(), cur_res.end());
    total_msgs += msgs.ext_messages_.size();
  }

  // Sort messages to each account by msg_seqno, if present
  std::map<std::pair<WorkchainId, StdSmcAddress>, std::vector<std::pair<td::uint32, size_t>>> wallet_msg_idxs_;
  for (size_t i = 0; i < res.size(); ++i) {
    auto &[message, priority] = res[i];
    MessageId id{res[i].first->shard(), res[i].first->hash()};
    auto msg_seqno = ext_msgs_[priority].ext_messages_[id]->msg_seqno;
    if (msg_seqno) {
      wallet_msg_idxs_[{message->wc(), message->addr()}].emplace_back(msg_seqno.value(), i);
    }
  }
  for (auto &[_, idxs] : wallet_msg_idxs_) {
    auto sorted_idxs = idxs;
    std::sort(sorted_idxs.begin(), sorted_idxs.end());
    std::vector<std::pair<td::Ref<ExtMessage>, int>> new_res;
    new_res.reserve(idxs.size());
    for (auto [_, i] : sorted_idxs) {
      new_res.push_back(res[i]);
    }
    for (size_t j = 0; j < new_res.size(); ++j) {
      res[idxs[j].second] = new_res[j];
    }
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
      auto &msgs = ext_msgs_[priority];
      auto it2 = msgs.ext_messages_.find(msg_id);
      if (it2 != msgs.ext_messages_.end()) {
        auto address = it2->second->address();
        auto norm_hash = ext_msg_norm_hash_str(it2->second->message->root_cell());
        LOG(INFO) << "dropping external message normalized_hash=" << norm_hash << " addr=" << address.first << ":"
                  << address.second.to_hex() << " prio=" << priority << " reason=bad_or_filtered";
      }
      msgs.erase(msg_id);
      ext_messages_hashes_.erase(it);
    }
  }
  for (auto &hash : to_delay) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      auto &msgs = ext_msgs_[priority];
      auto it2 = msgs.ext_messages_.find(msg_id);
      if (it2 != msgs.ext_messages_.end()) {
        auto address = it2->second->address();
        auto norm_hash = ext_msg_norm_hash_str(it2->second->message->root_cell());
        if (msgs.ext_messages_.size() < SOFT_MEMPOOL_LIMIT && it2->second->can_postpone()) {
          LOG(INFO) << "delaying external message normalized_hash=" << norm_hash << " addr=" << address.first << ":"
                    << address.second.to_hex() << " prio=" << priority << " generation=" << it2->second->generation;
          it2->second->postpone();
        } else {
          LOG(INFO) << "dropping external message normalized_hash=" << norm_hash << " addr=" << address.first << ":"
                    << address.second.to_hex() << " prio=" << priority
                    << " reason=delay_limit_or_mempool_full";
          msgs.erase(msg_id);
          ext_messages_hashes_.erase(it);
        }
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

void ExtMessagePool::add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                            td::optional<td::uint32> msg_seqno) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto norm_hash = ext_msg_norm_hash_str(message->root_cell());
  auto &msgs = ext_msgs_[priority];
  if (msgs.ext_messages_.size() > opts_->max_mempool_num()) {
    LOG(INFO) << "cannot add external message normalized_hash=" << norm_hash << " addr=" << wc << ":"
              << addr.to_hex() << " prio=" << priority
              << " to mempool: mempool is full (limit=" << opts_->max_mempool_num() << ")";
    return;
  }
  auto msg = std::make_unique<MempoolMsg>(message);
  msg->msg_seqno = msg_seqno;
  MessageId id{message->shard(), message->hash()};
  auto address = msg->address();
  auto it = msgs.ext_addr_messages_.find(address);
  if (it != msgs.ext_addr_messages_.end() && it->second.size() >= PER_ADDRESS_LIMIT) {
    LOG(INFO) << "cannot add external message normalized_hash=" << norm_hash << " addr=" << wc << ":"
              << addr.to_hex() << " prio=" << priority
              << " to mempool: per address limit reached (limit=" << PER_ADDRESS_LIMIT << ")";
    return;
  }
  auto it2 = ext_messages_hashes_.find(id.hash);
  if (it2 != ext_messages_hashes_.end()) {
    int old_priority = it2->second.first;
    if (old_priority >= priority) {
      LOG(INFO) << "cannot add external message normalized_hash=" << norm_hash << " addr=" << wc << ":"
                << addr.to_hex() << " prio=" << priority
                << " to mempool: already exists";
      return;
    }
    ext_msgs_[old_priority].erase(id);
  }
  msgs.ext_messages_.emplace(id, std::move(msg));
  msgs.ext_addr_messages_[address].emplace(id.hash, id);
  ext_messages_hashes_[id.hash] = {priority, id};
  LOG(INFO) << "adding external message normalized_hash=" << norm_hash << " addr=" << wc << ":" << addr.to_hex()
            << " prio=" << priority << " to mempool";
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
