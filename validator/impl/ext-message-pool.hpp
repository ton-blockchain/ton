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

namespace ton::validator {

class ExtMessagePool : public td::actor::Actor {
 public:
  ExtMessagePool(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : opts_(opts), manager_(manager) {
  }

  td::actor::Task<td::Ref<ExtMessage>> check_add_external_message(td::BufferSlice data, int priority,
                                                                  bool add_to_mempool);
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
  class MessageExt {
   public:
    auto shard() const {
      return message_->shard();
    }
    auto ext_id() const {
      auto shard = message_->shard();
      return MessageId{shard, message_->hash()};
    }
    auto message() const {
      return message_;
    }
    auto hash() const {
      return message_->hash();
    }
    auto address() const {
      return std::make_pair(message_->wc(), message_->addr());
    }
    bool is_active() {
      if (!active_) {
        if (reactivate_at_.is_in_past()) {
          active_ = true;
          generation_++;
        }
      }
      return active_;
    }
    bool can_postpone() const {
      return generation_ <= 2;
    }
    void postpone() {
      if (!active_) {
        return;
      }
      active_ = false;
      reactivate_at_ = td::Timestamp::in(generation_ * 5.0);
    }
    bool expired() const {
      return delete_at_.is_in_past();
    }
    explicit MessageExt(td::Ref<ExtMessage> msg) : message_(std::move(msg)) {
      delete_at_ = td::Timestamp::in(600);
    }

   private:
    td::Ref<ExtMessage> message_;
    td::uint32 generation_ = 0;
    bool active_ = true;
    td::Timestamp reactivate_at_;
    td::Timestamp delete_at_;
  };

  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<MasterchainState> last_masterchain_state_;

  struct ExtMessages {
    std::map<MessageId, std::unique_ptr<MessageExt>> ext_messages_;
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

  static constexpr double MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10.0;
  static constexpr size_t MAX_EXT_MSG_PER_ADDR = 3 * 10;
  static constexpr size_t PER_ADDRESS_LIMIT = 256;
};

}  // namespace ton::validator
