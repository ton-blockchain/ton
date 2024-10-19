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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "td/actor/core/ActorState.h"
#include "td/actor/core/ActorTypeStat.h"
#include "td/actor/core/ActorMailbox.h"

#include "td/utils/Heap.h"
#include "td/utils/List.h"
#include "td/utils/Time.h"
#include "td/utils/SharedObjectPool.h"

namespace td {
namespace actor {
namespace core {
class Actor;
class ActorInfo;
using ActorInfoPtr = SharedObjectPool<ActorInfo>::Ptr;
class ActorInfo : private HeapNode, private ListNode {
 public:
  ActorInfo(std::unique_ptr<Actor> actor, ActorState::Flags state_flags, Slice name, td::uint32 actor_stat_id)
      : actor_(std::move(actor)), name_(name.begin(), name.size()), actor_stat_id_(actor_stat_id) {
    state_.set_flags_unsafe(state_flags);
    VLOG(actor) << "Create actor [" << name_ << "]";
  }
  ~ActorInfo() {
    VLOG(actor) << "Destroy actor [" << name_ << "]";
    CHECK(!actor_);
  }

  bool is_alive() const {
    return !state_.get_flags_unsafe().is_closed();
  }

  bool has_actor() const {
    return bool(actor_);
  }
  Actor &actor() {
    CHECK(has_actor());
    return *actor_;
  }
  Actor *actor_ptr() const {
    return actor_.get();
  }
  // NB: must be called only when actor is locked
  ActorTypeStatRef actor_type_stat() {
    auto res = ActorTypeStatManager::get_actor_type_stat(actor_stat_id_, actor_.get());
    if (in_queue_since_) {
      res.pop_from_queue(in_queue_since_);
      in_queue_since_ = 0;
    }
    return res;
  }
  void on_add_to_queue() {
    in_queue_since_ = td::Clocks::rdtsc();
  }
  void destroy_actor() {
    actor_.reset();
  }
  ActorState &state() {
    return state_;
  }
  ActorMailbox &mailbox() {
    return mailbox_;
  }
  CSlice get_name() const {
    return name_;
  }

  HeapNode *as_heap_node() {
    return this;
  }
  static ActorInfo *from_heap_node(HeapNode *node) {
    return static_cast<ActorInfo *>(node);
  }

  Timestamp get_alarm_timestamp() const {
    return Timestamp::at(alarm_timestamp_at_.load(std::memory_order_relaxed));
  }
  void set_alarm_timestamp(Timestamp timestamp) {
    alarm_timestamp_at_.store(timestamp.at(), std::memory_order_relaxed);
  }

  void pin(ActorInfoPtr ptr) {
    CHECK(pin_.empty());
    CHECK(&*ptr == this);
    pin_ = std::move(ptr);
  }
  ActorInfoPtr unpin() {
    CHECK(!pin_.empty());
    return std::move(pin_);
  }

 private:
  std::unique_ptr<Actor> actor_;
  ActorState state_;
  ActorMailbox mailbox_;
  std::string name_;
  std::atomic<double> alarm_timestamp_at_{0};

  ActorInfoPtr pin_;
  td::uint64 in_queue_since_{0};
  td::uint32 actor_stat_id_{0};
};

}  // namespace core
}  // namespace actor
}  // namespace td
