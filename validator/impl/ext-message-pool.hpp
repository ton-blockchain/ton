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

#include <map>
#include <set>

#include "interfaces/validator-manager.h"
#include "td/actor/coro_utils.h"

#include "external-message.hpp"

namespace ton::validator {

class ExtMessagePool : public td::actor::Actor {
 public:
  ExtMessagePool(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : opts_(opts), manager_(manager), ext_msgs_(create_ext_msg_snapshot()) {
  }

  struct CheckResult {
    td::Ref<ExtMessage> message;
    td::actor::StartedTask<> wait_allow_broadcast;
  };
  td::actor::Task<CheckResult> check_add_external_message(td::BufferSlice data, int priority, bool add_to_mempool);
  void install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback);
  void cleanup_external_messages(ShardIdFull shard);
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay, std::vector<ExtMessage::Hash> to_delete);
  void erase_external_messages(std::vector<ExtMessage::Hash> to_delete);

  void update_last_masterchain_state(td::Ref<MasterchainState> state) {
    last_masterchain_state_ = std::move(state);
  }
  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }
  std::vector<std::pair<std::string, std::string>> prepare_stats();

  void alarm() override;

 private:
  struct MempoolMsg {
    td::Ref<ExtMessage> message;
    ExtMessage::Hash hash_norm;
    int priority;
    td::uint32 generation = 0;
    bool active = true;
    td::Timestamp reactivate_at;
    td::Timestamp delete_at;
    td::optional<td::uint32> msg_seqno;

    auto address() const {
      return std::make_pair(message->wc(), message->addr());
    }
    bool can_postpone() const {
      return generation <= 2;
    }
    bool try_reactivate() {
      if (active || !reactivate_at.is_in_past()) {
        return false;
      }
      active = true;
      ++generation;
      return true;
    }
    void postpone() {
      if (!active) {
        return;
      }
      reactivate_at = td::Timestamp::in(generation * 5.0);
      active = false;
    }
    bool expired() const {
      return delete_at.is_in_past();
    }
    MempoolMsg(td::Ref<ExtMessage> msg, int priority)
        : message(std::move(msg)), hash_norm(message->hash_norm()), priority(priority) {
      delete_at = td::Timestamp::in(600);
    }
  };

  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<MasterchainState> last_masterchain_state_;

  std::unique_ptr<ExtMsgSnapshot> ext_msgs_;
  std::map<ExtMessage::Hash, MempoolMsg> ext_msgs_info_;
  std::map<ExtMessage::Hash, std::set<ExtMessage::Hash>> ext_messages_hashes_norm_;  // normalized hash -> raw hashes
  std::map<int, size_t> ext_msgs_total_;
  std::map<int, std::map<std::pair<WorkchainId, StdSmcAddress>, std::set<ExtMessage::Hash>>> ext_addr_messages_;
  std::multimap<td::Timestamp, ExtMessage::Hash> reactivate_queue_;

  struct CheckedExtMsgCounter {
    std::map<std::pair<WorkchainId, StdSmcAddress>, size_t> counter_cur_, counter_prev_;
    td::Timestamp cleanup_at_ = td::Timestamp::now();

    size_t get_msg_count(WorkchainId wc, StdSmcAddress addr);
    size_t inc_msg_count(WorkchainId wc, StdSmcAddress addr);
    void before_query();
  } checked_ext_msg_counter_;
  td::uint64 total_check_ext_messages_ok_{0}, total_check_ext_messages_error_{0};
  td::uint64 applied_ext_msgs_delete_requests_{0}, applied_ext_msgs_deleted_{0};

  td::Timestamp cleanup_mempool_at_ = td::Timestamp::now();

  void add_message_to_mempool(td::Ref<ExtMessage> message, int priority, td::optional<td::uint32> msg_seqno);
  bool erase_message(ExtMessage::Hash hash);
  void notify_callbacks_add(td::Ref<ExtMessage> message, int priority);
  void reactivate_due();

  struct WalletMessageInfo {
    td::uint32 valid_until;
    td::Promise<td::Unit> allow_broadcast_promise;
  };
  struct WalletInfo {
    std::map<td::uint32, WalletMessageInfo> messages;
    ~WalletInfo() {
      for (auto &[_, message] : messages) {
        if (message.allow_broadcast_promise) {
          message.allow_broadcast_promise.set_error(td::Status::Error("wallet is no longer valid"));
        }
      }
    }
    void process_messages(td::uint32 wallet_seqno, UnixTime utime);
  };
  std::map<std::pair<WorkchainId, StdSmcAddress>, WalletInfo> wallets_;

  td::actor::Task<CheckResult> check_message(td::Ref<ExtMessage> message, td::optional<td::uint32> &msg_seqno);
  td::Result<td::uint32> check_message_to_wallet(td::Ref<ExtMessage> message, const WalletMessageProcessor *wallet,
                                                 block::Account acc, UnixTime utime, LogicalTime lt,
                                                 std::unique_ptr<block::ConfigInfo> config,
                                                 td::Promise<td::Unit> allow_broadcast_promise);

  std::vector<std::unique_ptr<ExtMsgCallback>> callbacks_;

  static constexpr double MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10.0;
  static constexpr size_t MAX_EXT_MSG_PER_ADDR = 3 * 10;
  static constexpr size_t PER_ADDRESS_LIMIT = 256;
  static constexpr size_t SOFT_MEMPOOL_LIMIT = 1024;
  static constexpr td::uint32 MAX_WALLET_SEQNO_DIFF = 16;
};

}  // namespace ton::validator
