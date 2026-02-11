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

#include "interfaces/validator-manager.h"
#include "td/actor/coro_utils.h"

#include "external-message.hpp"

namespace ton::validator {

class ExtMessagePool : public td::actor::Actor {
 public:
  ExtMessagePool(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : opts_(opts), manager_(manager) {
  }

  struct CheckResult {
    td::Ref<ExtMessage> message;
    td::actor::StartedTask<> wait_allow_broadcast;
  };
  td::actor::Task<CheckResult> check_add_external_message(td::BufferSlice data, int priority, bool add_to_mempool);
  std::vector<std::pair<td::Ref<ExtMessage>, int>> get_external_messages_for_collator(ShardIdFull shard);
  void cleanup_external_messages(ShardIdFull shard);
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay, std::vector<ExtMessage::Hash> to_delete);

  void update_last_masterchain_state(td::Ref<MasterchainState> state) {
    last_masterchain_state_ = std::move(state);
  }
  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }
  std::vector<std::pair<std::string, std::string>> prepare_stats();

  void alarm() override;

 private:
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
  };
  struct MempoolMsg {
    td::Ref<ExtMessage> message;
    td::uint32 generation = 0;
    bool active = true;
    td::Timestamp reactivate_at;
    td::Timestamp delete_at;
    td::optional<td::uint32> msg_seqno;

    auto address() const {
      return std::make_pair(message->wc(), message->addr());
    }
    bool is_active() {
      if (!active) {
        if (reactivate_at.is_in_past()) {
          active = true;
          generation++;
        }
      }
      return active;
    }
    bool can_postpone() const {
      return generation <= 2;
    }
    void postpone() {
      if (!active) {
        return;
      }
      active = false;
      reactivate_at = td::Timestamp::in(generation * 5.0);
    }
    bool expired() const {
      return delete_at.is_in_past();
    }
    explicit MempoolMsg(td::Ref<ExtMessage> msg) : message(std::move(msg)) {
      delete_at = td::Timestamp::in(600);
    }
  };

  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<MasterchainState> last_masterchain_state_;

  struct ExtMessages {
    std::map<MessageId, std::unique_ptr<MempoolMsg>> ext_messages_;
    std::map<std::pair<WorkchainId, StdSmcAddress>, std::map<ExtMessage::Hash, MessageId>> ext_addr_messages_;
    void erase(const MessageId &id) {
      auto it = ext_messages_.find(id);
      CHECK(it != ext_messages_.end());
      ext_addr_messages_[it->second->address()].erase(id.hash);
      ext_messages_.erase(it);
    }
  };
  std::map<int, ExtMessages> ext_msgs_;                                        // priority -> messages
  std::map<ExtMessage::Hash, std::pair<int, MessageId>> ext_messages_hashes_;  // hash -> priority

  struct CheckedExtMsgCounter {
    std::map<std::pair<WorkchainId, StdSmcAddress>, size_t> counter_cur_, counter_prev_;
    td::Timestamp cleanup_at_ = td::Timestamp::now();

    size_t get_msg_count(WorkchainId wc, StdSmcAddress addr);
    size_t inc_msg_count(WorkchainId wc, StdSmcAddress addr);
    void before_query();
  } checked_ext_msg_counter_;
  td::uint64 total_check_ext_messages_ok_{0}, total_check_ext_messages_error_{0};

  td::Timestamp cleanup_mempool_at_ = td::Timestamp::now();

  void add_message_to_mempool(td::Ref<ExtMessage> message, int priority, td::optional<td::uint32> msg_seqno);

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

  static constexpr double MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10.0;
  static constexpr size_t MAX_EXT_MSG_PER_ADDR = 3 * 10;
  static constexpr size_t PER_ADDRESS_LIMIT = 256;
  static constexpr size_t SOFT_MEMPOOL_LIMIT = 1024;
  static constexpr td::uint32 MAX_WALLET_SEQNO_DIFF = 16;
};

}  // namespace ton::validator
