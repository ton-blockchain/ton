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

#include <deque>
#include <set>

#include "interfaces/validator-manager.h"
#include "metrics/ext-message-pool-metrics.h"
#include "td/actor/coro_utils.h"
#include "td/utils/PersistentTreap.h"

#include "expiry-ordered-list.hpp"
#include "ext-message-checker.hpp"
#include "external-message.hpp"

namespace ton::validator {

struct ExtMessagePoolTestPeer;

class ExtMessagePool : public td::actor::Actor {
 public:
  ExtMessagePool(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : opts_(opts), manager_(manager) {
    pool_opts_ = opts_->get_ext_message_pool_options();
    checked_ext_msg_counter_.time_window_ = pool_opts_->max_ext_msg_per_addr_time_window;
  }

  struct CheckResult {
    td::Ref<ExtMessage> message;
    td::actor::StartedTask<> wait_allow_broadcast;
  };
  td::actor::Task<CheckResult> check_add_external_message(td::BufferSlice data, int priority, bool add_to_mempool);
  void install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback);
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay, std::vector<ExtMessage::Hash> to_delete);
  void erase_external_messages(BlockIdExt block_id, td::uint64 applied_count, std::vector<ExtMessage::Hash> to_delete);

  void update_last_masterchain_state(td::Ref<MasterchainState> state) {
    last_masterchain_state_ = std::move(state);
  }
  void update_options(td::Ref<ValidatorManagerOptions> opts);
  std::vector<std::pair<std::string, std::string>> prepare_stats();

  // Cross the actor boundary with values, not a scrape-local metrics::Context.
  using MetricsSnapshot = metrics::ExtMessagePoolSnapshot;
  MetricsSnapshot get_metrics_snapshot();

  void alarm() override;
  void start_up() override {
    alarm_timestamp().relax(admission_stats_at_);
  }

 private:
  friend struct ExtMessagePoolTestPeer;

  struct MessageId {
    AccountIdPrefixFull dst;
    ExtMessage::Hash hash;

    bool operator<(const MessageId &msg) const {
      if (dst < msg.dst) {
        return true;
      }
      if (msg.dst < dst) {
        return false;
      }
      return hash < msg.hash;
    }
    bool operator==(const MessageId &msg) const {
      return !(*this < msg) && !(msg < *this);
    }
  };
  struct MempoolMsg {
    static constexpr double TTL = 600.0;

    td::Ref<ExtMessage> message;
    ExtMessage::Hash hash_norm;
    MempoolMsg *older = nullptr;
    MempoolMsg *newer = nullptr;
    td::uint32 generation = 0;
    bool active = true;
    td::Timestamp reactivate_at;
    const td::Timestamp delete_at;

    auto address() const {
      return std::make_pair(message->wc(), message->addr());
    }
    bool is_active(bool *reactivated = nullptr) {
      if (reactivated != nullptr) {
        *reactivated = false;
      }
      if (!active) {
        if (reactivate_at.is_in_past()) {
          active = true;
          generation++;
          if (reactivated != nullptr) {
            *reactivated = true;
          }
        }
      }
      return active;
    }
    bool can_postpone() const {
      return generation <= 2;
    }
    bool postpone() {
      if (!active) {
        return false;
      }
      active = false;
      reactivate_at = td::Timestamp::in(generation * 5.0);
      return true;
    }
    bool expired() const {
      return delete_at.is_in_past();
    }
    explicit MempoolMsg(td::Ref<ExtMessage> msg)
        : message(std::move(msg)), hash_norm(message->hash_norm()), delete_at(td::Timestamp::in(TTL)) {
    }
  };

  td::Ref<ValidatorManagerOptions> opts_;
  td::Ref<ExtMessagePoolOptions> pool_opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<MasterchainState> last_masterchain_state_;

  struct ExtMessages {
    td::PersistentTreap<MessageId, std::shared_ptr<MempoolMsg>> ext_messages_;
    std::map<std::pair<WorkchainId, StdSmcAddress>, std::map<ExtMessage::Hash, MessageId>> ext_addr_messages_;
  };
  struct NormalizedMessageId {
    int priority;
    MessageId id;

    bool operator<(const NormalizedMessageId &msg) const {
      if (priority != msg.priority) {
        return priority < msg.priority;
      }
      return id < msg.id;
    }
  };
  std::map<int, ExtMessages> ext_msgs_;                                        // priority -> messages
  std::map<ExtMessage::Hash, std::pair<int, MessageId>> ext_messages_hashes_;  // raw hash -> priority
  std::map<ExtMessage::Hash, std::set<NormalizedMessageId>> ext_messages_hashes_norm_;

  struct CheckedExtMsgCounter {
    std::map<std::pair<WorkchainId, StdSmcAddress>, size_t> counter_cur_, counter_prev_;
    td::Timestamp cleanup_at_ = td::Timestamp::now();
    double time_window_ = 10.0;

    size_t get_msg_count(WorkchainId wc, StdSmcAddress addr);
    size_t inc_msg_count(WorkchainId wc, StdSmcAddress addr);
    void before_query();
  } checked_ext_msg_counter_;
  td::uint64 total_check_ext_messages_ok_{0}, total_check_ext_messages_error_{0};
  td::uint64 applied_ext_msgs_delete_requests_{0}, applied_ext_msgs_deleted_{0};
  std::array<td::uint64, static_cast<size_t>(metrics::ExtMessageAdmissionOutcome::count)> admission_outcomes_{};
  std::array<td::uint64, static_cast<size_t>(metrics::ExtMessageRemovalReason::count)> removal_reasons_{};
  td::uint64 applied_ext_messages_master_{0}, applied_ext_messages_shard_{0};
  metrics::ExtMessageStateCounts ext_message_states_;
  metrics::Histogram<metrics::kExtInclusionBuckets> ext_inclusion_seconds_;
  detail::ExpiryOrderedList<MempoolMsg> expiry_order_;

  metrics::ExtMessageAdmissionOutcome add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                                             td::Timestamp &alarm);
  metrics::ExtMessageAdmissionOutcome finalize_admission(td::Ref<ExtMessage> message, int priority, bool add_to_mempool,
                                                         td::Timestamp &alarm);
  // `stored_age_seconds`, when given, receives the erased entry's age before it is destroyed.
  bool erase_message(int priority, MessageId id, double *stored_age_seconds = nullptr);
  static double stored_age(const MempoolMsg &message);
  size_t cleanup_expired_messages(td::Timestamp now = td::Timestamp::now());
  bool prepare_message_for_collation(MempoolMsg *message);
  void link_message(MempoolMsg *message);
  void unlink_message(MempoolMsg *message);
  void record_admission(metrics::ExtMessageAdmissionOutcome outcome);
  void record_removal(metrics::ExtMessageRemovalReason reason);

  // ===== Parallel admission =====
  // The expensive per-message stages (parse, account state fetch, VM check) run on these worker
  // actors; the pool only dispatches and finalizes. Created lazily on the first check.
  bool inited_checkers_ = false;
  size_t checkers_generation_ = 0;
  struct Checker {
    td::actor::ActorOwn<ExtMessageChecker> actor;
    size_t inflight = 0;
    bool priority = false;
  };
  // First num_regular_checkers_ are regular, last num_priority_checkers_ are priority
  std::vector<Checker> checkers_;
  size_t num_regular_checkers_ = 0, num_priority_checkers_ = 0;
  size_t next_regular_checker_ = 0, next_priority_checker_ = 0;
  void init_checkers();
  // Admission backpressure: only MAX_INFLIGHT_CHECKS checks run concurrently; the rest wait in
  // FIFO order (bounded — beyond that requests fail fast instead of queueing into a congestion
  // collapse that would starve the whole node).
  size_t inflight_total_checks_{0};
  size_t inflight_regular_checks_{0};
  size_t inflight_priority_checks_{0};
  std::map<int, std::deque<td::actor::StartedTask<>::ExternalPromise>> admission_waiters_;  // priority -> queue
  size_t total_admission_waiters_ = 0;
  bool have_free_slots(int priority);
  size_t select_worker(int priority);
  void release_check_slot(size_t worker);
  // Adaptive wait-queue cap: bound the ESTIMATED queueing delay, not just the count, so that
  // under degraded capacity (CPU contention, cold caches) requests fail fast instead of being
  // answered after the client has already timed out.
  double check_completion_rate_{2000.0};  // EWMA, completions/s; optimistic start for cold boot
  td::uint64 completions_in_rate_window_{0};
  double rate_window_start_{td::Time::now()};
  size_t max_admission_waiters();

  // Rolling window for the periodic "ext admission" INFO stat.
  struct AdmissionWindowStats {
    td::uint64 in{0}, admitted{0}, rejected{0}, checked{0};
    double check_time{0};
    ExtMessageChecker::StageTimings timings;
    td::Timestamp window_start = td::Timestamp::now();
  };
  AdmissionWindowStats admission_window_;
  td::Timestamp admission_stats_at_ = td::Timestamp::in(ADMISSION_STATS_PERIOD);
  void log_admission_stats();

  std::vector<std::unique_ptr<ExtMsgCallback>> callbacks_;

  static constexpr size_t PER_ADDRESS_LIMIT = 256;
  static constexpr size_t SOFT_MEMPOOL_LIMIT = 1024;
  static constexpr size_t MAX_INFLIGHT_CHECKS_PER_CHECKER = 8;
  // Keep the estimated queueing delay well under client/liteserver timeouts (~10s): beyond
  // that the requests would be answered after the caller gave up anyway, so fail them fast.
  static constexpr double MAX_ADMISSION_QUEUE_DELAY = 5.0;
  static constexpr double ADMISSION_STATS_PERIOD = 5.0;
};

}  // namespace ton::validator
