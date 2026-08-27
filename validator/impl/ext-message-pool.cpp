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
#include "td/utils/Timer.h"
#include "ton/ton-io.hpp"

#include "ext-message-pool.hpp"
#include "external-message.hpp"
#include "fabric.h"

namespace ton::validator {
namespace {

metrics::ExtMessageAdmissionOutcome admission_outcome(ExtMessageChecker::Failure failure) {
  switch (failure) {
    case ExtMessageChecker::Failure::invalid:
      return metrics::ExtMessageAdmissionOutcome::invalid;
    case ExtMessageChecker::Failure::state_unavailable:
      return metrics::ExtMessageAdmissionOutcome::state_unavailable;
    case ExtMessageChecker::Failure::vm_rejected:
      return metrics::ExtMessageAdmissionOutcome::vm_rejected;
    case ExtMessageChecker::Failure::none:
      return metrics::ExtMessageAdmissionOutcome::internal_error;
  }
  UNREACHABLE();
}

}  // namespace

void ExtMessagePool::init_checkers() {
  if (inited_checkers_) {
    inflight_checks_ = 0;
    for (auto &checker : checkers_) {
      td::actor::send_closure(checker.actor, &ExtMessageChecker::destroy);
      checker.actor.release();
    }
    checkers_.clear();
  }
  ++checkers_generation_;
  inited_checkers_ = true;
  size_t num_checkers = pool_opts_->num_checkers;
  checkers_.resize(num_checkers);
  for (size_t i = 0; i < num_checkers; ++i) {
    checkers_[i].actor = td::actor::create_actor<ExtMessageChecker>(PSTRING() << "extmsgcheck" << i, manager_);
  }
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_external_message(td::BufferSlice data,
                                                                                        int priority,
                                                                                        bool add_to_mempool) {
  ++admission_window_.in;
  if (last_masterchain_state_.is_null()) {
    ++admission_window_.rejected;
    record_admission(metrics::ExtMessageAdmissionOutcome::not_ready);
    co_return td::Status::Error(ErrorCode::notready, "not ready");
  }
  auto ext_msg_limits = last_masterchain_state_->get_ext_msg_limits();
  if (data.size() > ext_msg_limits.max_size) {
    ++admission_window_.rejected;
    record_admission(metrics::ExtMessageAdmissionOutcome::too_large);
    co_return td::Status::Error("external message too large, rejecting");
  }
  if (!inited_checkers_) {
    init_checkers();
  }
  // Backpressure: bound the number of concurrent checks. Without it, an over-rate burst piles
  // unbounded work onto the worker/pool mailboxes and the queueing delay alone times every
  // request out (congestion collapse) while starving the rest of the node of CPU.
  while (inflight_checks_ >= MAX_INFLIGHT_CHECKS_PER_CHECKER * checkers_.size()) {
    if (admission_waiters_.size() >= max_admission_waiters()) {
      ++admission_window_.rejected;
      record_admission(metrics::ExtMessageAdmissionOutcome::backpressure);
      co_return td::Status::Error(ErrorCode::notready, "too many pending external message checks");
    }
    if (checkers_.empty()) {
      ++admission_window_.rejected;
      record_admission(metrics::ExtMessageAdmissionOutcome::not_ready);
      co_return td::Status::Error(ErrorCode::notready, "no checkers are configured");
    }
    auto [task, promise] = td::actor::StartedTask<>::make_bridge();
    admission_waiters_.push_back(std::move(promise));
    co_await std::move(task);
    // Loop: a new arrival may have grabbed the slot between our wakeup and resumption.
  }
  size_t current_generation = checkers_generation_;
  ++inflight_checks_;
  SCOPE_EXIT {
    ++completions_in_rate_window_;
    if (current_generation == checkers_generation_) {
      release_check_slot();
    }
  };
  // The expensive stages (parse, account state fetch — cold celldb reads —, VM execution) run
  // on a worker; this pool actor stays free to dispatch/finalize other messages meanwhile.
  size_t worker = next_checker_++ % checkers_.size();
  ++checkers_[worker].inflight;
  td::Timer check_timer;
  auto r_checked = co_await td::actor::ask(checkers_[worker].actor, &ExtMessageChecker::check, std::move(data),
                                           ext_msg_limits, last_masterchain_state_)
                       .wrap();
  if (current_generation == checkers_generation_) {
    --checkers_[worker].inflight;
  }
  admission_window_.check_time += check_timer.elapsed();
  ++admission_window_.checked;
  if (r_checked.is_error()) {
    ++total_check_ext_messages_error_;
    ++admission_window_.rejected;
    record_admission(metrics::ExtMessageAdmissionOutcome::internal_error);
    co_return r_checked.move_as_error();
  }
  auto check_outcome = r_checked.move_as_ok();
  if (check_outcome.result.is_error()) {
    ++total_check_ext_messages_error_;
    ++admission_window_.rejected;
    record_admission(admission_outcome(check_outcome.failure));
    co_return check_outcome.result.move_as_error();
  }
  auto checked = check_outcome.result.move_as_ok();
  auto &t = admission_window_.timings;
  t.parse += checked.timings.parse;
  t.fetch_state += checked.timings.fetch_state;
  t.lookup += checked.timings.lookup;
  t.vm += checked.timings.vm;

  // Finalize atomically (no suspension) on the pool actor
  auto message = checked.message;
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto finalize = [&]() -> td::Result<CheckResult> {
    if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= pool_opts_->max_ext_msg_per_addr) {
      return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
    }
    td::actor::StartedTask<> wait_allow_broadcast;
    auto [task, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
    allow_broadcast_promise.set_value(td::Unit{});
    wait_allow_broadcast = std::move(task);
    if (checked_ext_msg_counter_.inc_msg_count(wc, addr) > pool_opts_->max_ext_msg_per_addr) {
      return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
    }
    return CheckResult{std::move(message), std::move(wait_allow_broadcast)};
  };
  auto result = finalize();
  ++(result.is_ok() ? total_check_ext_messages_ok_ : total_check_ext_messages_error_);
  ++(result.is_ok() ? admission_window_.admitted : admission_window_.rejected);
  if (result.is_error()) {
    record_admission(metrics::ExtMessageAdmissionOutcome::rate_limited);
    co_return result.move_as_error();
  }
  finalize_admission(checked.message, priority, add_to_mempool, alarm_timestamp());
  co_return result.move_as_ok();
}

metrics::ExtMessageAdmissionOutcome ExtMessagePool::finalize_admission(td::Ref<ExtMessage> message, int priority,
                                                                       bool add_to_mempool, td::Timestamp &alarm) {
  auto outcome = metrics::ExtMessageAdmissionOutcome::validated_only;
  if (add_to_mempool) {
    outcome = add_message_to_mempool(std::move(message), priority, alarm);
  }
  record_admission(outcome);
  return outcome;
}

void ExtMessagePool::record_admission(metrics::ExtMessageAdmissionOutcome outcome) {
  ++admission_outcomes_[static_cast<size_t>(outcome)];
}

void ExtMessagePool::record_removal(metrics::ExtMessageRemovalReason reason) {
  ++removal_reasons_[static_cast<size_t>(reason)];
}

size_t ExtMessagePool::max_admission_waiters() {
  double now = td::Time::now();
  double window = now - rate_window_start_;
  if (window >= 1.0) {
    if (window <= 10.0) {
      check_completion_rate_ = 0.5 * check_completion_rate_ + 0.5 * (double)completions_in_rate_window_ / window;
    }  // else: stale idle-period data, keep the previous estimate
    completions_in_rate_window_ = 0;
    rate_window_start_ = now;
  }
  double cap = check_completion_rate_ * MAX_ADMISSION_QUEUE_DELAY;
  double min_cap = std::min(512.0, (double)pool_opts_->max_admission_waiters);
  return (size_t)td::clamp(cap, min_cap, (double)pool_opts_->max_admission_waiters);
}

void ExtMessagePool::release_check_slot() {
  --inflight_checks_;
  // Wake one waiter per freed slot; it re-checks the limit when it resumes, so this stays
  // correct (no slot leak) even if the woken request was cancelled while waiting.
  if (!admission_waiters_.empty()) {
    auto waiter = std::move(admission_waiters_.front());
    admission_waiters_.pop_front();
    waiter.set_value(td::Unit{});
  }
}

void ExtMessagePool::log_admission_stats() {
  auto &w = admission_window_;
  double dt = td::Time::now() - w.window_start.at();
  if (w.in > 0 && dt > 0) {
    size_t busy = 0, inflight = 0;
    for (auto &checker : checkers_) {
      busy += checker.inflight > 0;
      inflight += checker.inflight;
    }
    char buf[320];
    snprintf(buf, sizeof(buf),
             "ext admission: in=%.0f/s admitted=%.0f/s rejected=%.0f/s busy_workers=%zu/%zu inflight=%zu wait_q=%zu "
             "avg_check_ms=%.2f (parse=%.2f state=%.2f lookup=%.2f vm=%.2f)",
             (double)w.in / dt, (double)w.admitted / dt, (double)w.rejected / dt, busy, checkers_.size(), inflight,
             admission_waiters_.size(), w.checked ? w.check_time / (double)w.checked * 1e3 : 0.0,
             w.checked ? w.timings.parse / (double)w.checked * 1e3 : 0.0,
             w.checked ? w.timings.fetch_state / (double)w.checked * 1e3 : 0.0,
             w.checked ? w.timings.lookup / (double)w.checked * 1e3 : 0.0,
             w.checked ? w.timings.vm / (double)w.checked * 1e3 : 0.0);
    LOG(INFO) << buf;
  }
  w = AdmissionWindowStats{};
}

void ExtMessagePool::install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) {
  // Compute shard key range [lo, hi) for splitting
  td::uint64 lo_prefix = shard.shard & (shard.shard - 1);
  td::uint64 hi_prefix_plus1 = (shard.shard | (shard.shard - 1)) + 1;  // may overflow to 0
  MessageId shard_lo{AccountIdPrefixFull{shard.workchain, lo_prefix}, Bits256::zero()};
  MessageId shard_hi{AccountIdPrefixFull{hi_prefix_plus1 == 0 ? shard.workchain + 1 : shard.workchain, hi_prefix_plus1},
                     Bits256::zero()};

  // Take O(log n) shard slices from each priority level
  using Treap = td::PersistentTreap<MessageId, std::shared_ptr<MempoolMsg>>;
  using Snapshot = std::vector<std::pair<int, Treap>>;
  Snapshot snapshot;
  for (auto it = ext_msgs_.rbegin(); it != ext_msgs_.rend(); ++it) {
    auto [_, in_shard, __] = it->second.ext_messages_.split_range(shard_lo, shard_hi);
    if (!in_shard.empty()) {
      snapshot.emplace_back(it->first, std::move(in_shard));
    }
  }

  // Spawn a coroutine that drains the shard slices randomly into the queue
  auto push_existing = [](ExtMessagePool *pool, ExtMsgQueue queue, td::CancellationToken token, ShardIdFull shard,
                          Snapshot snapshot, bool sync_only) -> td::actor::Task<> {
    SCOPE_EXIT {
      if (sync_only) {
        queue.close();
      }
    };
    td::Timer t;
    size_t pushed = 0;
    for (auto &[priority, treap] : snapshot) {
      while (!treap.empty()) {
        if (token.check().is_error()) {
          co_return {};
        }
        size_t idx = td::Random::fast_uint32() % treap.size();
        auto [key, msg] = treap.at(idx);
        treap = treap.erase_at(idx);  // local snapshot only
        if (!pool->prepare_message_for_collation(msg.get())) {
          continue;
        }
        bool ok = co_await queue.push(std::make_pair(msg->message, priority));
        if (!ok) {
          co_return {};
        }
        ++pushed;
      }
    }
    LOG(WARNING) << "install_collator_queue: pushed " << pushed << " existing messages to shard " << shard << " in "
                 << t.elapsed() << "s";
    co_return {};
  };
  push_existing(this, callback->queue, callback->cancellation_token, shard, std::move(snapshot), callback->sync_only)
      .start()
      .detach();

  if (!callback->sync_only) {
    alarm_timestamp().relax(callback->timeout);
    callbacks_.push_back(std::move(callback));
  }
}

size_t ExtMessagePool::cleanup_expired_messages(td::Timestamp now) {
  return expiry_order_.pop_expired(now, [&](MempoolMsg *message) {
    auto it = ext_messages_hashes_.find(message->message->hash());
    CHECK(it != ext_messages_hashes_.end());
    auto [priority, id] = it->second;
    bool erased = erase_message(priority, id);
    if (erased) {
      record_removal(metrics::ExtMessageRemovalReason::expired);
    }
    return erased;
  });
}

void ExtMessagePool::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                std::vector<ExtMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end() && erase_message(it->second.first, it->second.second)) {
      record_removal(metrics::ExtMessageRemovalReason::filtered);
    }
  }
  for (auto &hash : to_delay) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      auto &msgs = ext_msgs_[priority];
      auto msg = msgs.ext_messages_.find(msg_id);
      if (!msg) {
        continue;
      }
      bool can_postpone = msg.value()->can_postpone();
      if (can_postpone && msgs.ext_messages_.size() < SOFT_MEMPOOL_LIMIT) {
        if (msg.value()->postpone()) {
          ext_message_states_.transition(metrics::ExtMessageState::eligible, metrics::ExtMessageState::postponed);
        }
        continue;
      }
      if (erase_message(priority, msg_id)) {
        record_removal(can_postpone ? metrics::ExtMessageRemovalReason::pool_pressure
                                    : metrics::ExtMessageRemovalReason::rejected_final);
      }
    }
  }
}

void ExtMessagePool::erase_external_messages(BlockIdExt block_id, td::uint64 applied_count,
                                             std::vector<ExtMessage::Hash> to_delete) {
  (block_id.is_masterchain() ? applied_ext_messages_master_ : applied_ext_messages_shard_) += applied_count;
  applied_ext_msgs_delete_requests_ += to_delete.size();
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_norm_.find(hash);
    if (it != ext_messages_hashes_norm_.end()) {
      auto ids = it->second;
      for (const auto &message_id : ids) {
        double age = 0.0;
        if (erase_message(message_id.priority, message_id.id, &age)) {
          ++applied_ext_msgs_deleted_;
          record_removal(metrics::ExtMessageRemovalReason::applied);
          ext_inclusion_seconds_.observe(age);
        }
      }
    }
  }
}

double ExtMessagePool::stored_age(const MempoolMsg &message) {
  return std::max(0.0, MempoolMsg::TTL - message.delete_at.in());
}

bool ExtMessagePool::erase_message(int priority, MessageId id, double *stored_age_seconds) {
  auto it_priority = ext_msgs_.find(priority);
  if (it_priority == ext_msgs_.end()) {
    return false;
  }
  auto &msgs = it_priority->second;
  auto msg_opt = msgs.ext_messages_.find(id);
  if (!msg_opt) {
    return false;
  }

  auto message = msg_opt.value();
  if (stored_age_seconds != nullptr) {
    *stored_age_seconds = stored_age(*message);
  }
  auto address = message->address();
  auto hash_norm = message->hash_norm;
  ext_message_states_.erase(message->active ? metrics::ExtMessageState::eligible : metrics::ExtMessageState::postponed);
  unlink_message(message.get());
  msgs.ext_addr_messages_[address].erase(id.hash);
  msgs.ext_messages_ = msgs.ext_messages_.erase(id);
  ext_messages_hashes_.erase(id.hash);

  auto it_norm = ext_messages_hashes_norm_.find(hash_norm);
  if (it_norm != ext_messages_hashes_norm_.end()) {
    it_norm->second.erase(NormalizedMessageId{priority, id});
    if (it_norm->second.empty()) {
      ext_messages_hashes_norm_.erase(it_norm);
    }
  }
  return true;
}

void ExtMessagePool::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
  if (opts_->get_ext_message_pool_options() == pool_opts_) {
    return;
  }
  pool_opts_ = opts_->get_ext_message_pool_options();
  if (checked_ext_msg_counter_.time_window_ != pool_opts_->max_ext_msg_per_addr_time_window) {
    checked_ext_msg_counter_ = {};
    checked_ext_msg_counter_.time_window_ = pool_opts_->max_ext_msg_per_addr_time_window;
  }
  if (inited_checkers_ && checkers_.size() != pool_opts_->num_checkers) {
    init_checkers();
    // Wakeup all waiters here. Pool options don't change often, so it's OK
    auto waiters = std::move(admission_waiters_);
    admission_waiters_.clear();
    while (!waiters.empty()) {
      auto waiter = std::move(waiters.front());
      waiters.pop_front();
      waiter.set_value(td::Unit{});
    }
  }
}

std::vector<std::pair<std::string, std::string>> ExtMessagePool::prepare_stats() {
  auto stats = get_metrics_snapshot();
  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("total.ext_msg_check", PSTRING() << "ok:" << stats.check_ok << " error:" << stats.check_error);
  vec.emplace_back("total.ext_msg_applied_cleanup", PSTRING() << "requested:" << applied_ext_msgs_delete_requests_
                                                              << " deleted:" << applied_ext_msgs_deleted_);
  return vec;
}

ExtMessagePool::MetricsSnapshot ExtMessagePool::get_metrics_snapshot() {
  double oldest_age = expiry_order_.oldest() != nullptr ? stored_age(*expiry_order_.oldest()) : 0.0;
  DCHECK(ext_message_states_.total() == ext_messages_hashes_.size());
  return {
      .ext_messages = ext_message_states_,
      .oldest_ext_message_age_seconds = oldest_age,
      .check_ok = total_check_ext_messages_ok_,
      .check_error = total_check_ext_messages_error_,
      .admission = admission_outcomes_,
      .removed = removal_reasons_,
      .ext_inclusion_seconds = ext_inclusion_seconds_,
      .applied_master = applied_ext_messages_master_,
      .applied_shard = applied_ext_messages_shard_,
  };
}

void ExtMessagePool::alarm() {
  if (admission_stats_at_.is_in_past()) {
    log_admission_stats();
    admission_stats_at_ = td::Timestamp::in(ADMISSION_STATS_PERIOD);
  }
  alarm_timestamp().relax(admission_stats_at_);
  cleanup_expired_messages();
  expiry_order_.relax_alarm(alarm_timestamp());
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->timeout && callback->timeout.is_in_past()) {
      return true;
    }
    alarm_timestamp().relax(callback->timeout);
    return false;
  });
}

metrics::ExtMessageAdmissionOutcome ExtMessagePool::add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                                                           td::Timestamp &alarm) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto &msgs = ext_msgs_[priority];
  if (msgs.ext_messages_.size() > pool_opts_->max_mempool_messages) {
    LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
              << " to mempool: mempool is full (limit=" << pool_opts_->max_mempool_messages << ")";
    return metrics::ExtMessageAdmissionOutcome::pool_full;
  }
  auto msg = std::make_shared<MempoolMsg>(message);
  MessageId id{message->shard(), message->hash()};
  auto address = msg->address();
  auto it = msgs.ext_addr_messages_.find(address);
  if (it != msgs.ext_addr_messages_.end() && it->second.size() >= PER_ADDRESS_LIMIT) {
    LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
              << " to mempool: per address limit reached (limit=" << PER_ADDRESS_LIMIT << ")";
    return metrics::ExtMessageAdmissionOutcome::address_full;
  }
  bool reprioritized = false;
  auto existing = ext_messages_hashes_.find(id.hash);
  if (existing != ext_messages_hashes_.end()) {
    int old_priority = existing->second.first;
    if (old_priority >= priority) {
      LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                << " to mempool: already exists";
      return metrics::ExtMessageAdmissionOutcome::duplicate;
    }
    erase_message(old_priority, id);
    reprioritized = true;
  }
  auto hash_norm = msg->hash_norm;
  auto *msg_ptr = msg.get();
  msgs.ext_messages_ = msgs.ext_messages_.insert(id, std::move(msg));
  link_message(msg_ptr);
  msgs.ext_addr_messages_[address].emplace(id.hash, id);
  ext_messages_hashes_[id.hash] = {priority, id};
  ext_messages_hashes_norm_[hash_norm].insert(NormalizedMessageId{priority, id});
  ext_message_states_.insert();
  expiry_order_.relax_alarm(alarm);
  LOG(INFO) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " to mempool";
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->cancellation_token.check().is_error()) {
      return true;
    }
    if (shard_contains(callback->shard, message->shard())) {
      callback->queue.try_push(std::make_pair(message, priority)).detach();
    }
    return false;
  });
  return reprioritized ? metrics::ExtMessageAdmissionOutcome::reprioritized
                       : metrics::ExtMessageAdmissionOutcome::accepted;
}

void ExtMessagePool::link_message(MempoolMsg *message) {
  expiry_order_.append(message);
}

void ExtMessagePool::unlink_message(MempoolMsg *message) {
  expiry_order_.unlink(message);
}

bool ExtMessagePool::prepare_message_for_collation(MempoolMsg *message) {
  bool reactivated = false;
  if (message->expired() || !message->is_active(&reactivated)) {
    return false;
  }
  if (reactivated && expiry_order_.contains(message)) {
    ext_message_states_.transition(metrics::ExtMessageState::postponed, metrics::ExtMessageState::eligible);
  }
  return true;
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
      cleanup_at_ = td::Timestamp::in(time_window_ / 2.0);
      break;
    }
    cleanup_at_ += time_window_ / 2.0;
  }
}

}  // namespace ton::validator
