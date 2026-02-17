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

#include <memory>
#include <utility>

#include "td/actor/core/ActorInfo.h"
#include "td/actor/core/SchedulerContext.h"

namespace td {
namespace actor {
namespace core {

// SchedulerMessage is a pointer-sized tagged pointer:
// Bit 0 = 0: ActorInfoPtr (raw pointer, must be 8-byte aligned)
// Bit 0 = 1, Bit 1 = 0: Timer register (TimerNode* | 1)
// Bit 0 = 1, Bit 1 = 1: Timer cancel (TimerNode* | 3)
struct SchedulerMessage {
  static constexpr uintptr_t kActorTagBit = 1u;
  static constexpr uintptr_t kTimerTagMask = 3u;
  static constexpr uintptr_t kTimerRegisterTag = 1u;
  static constexpr uintptr_t kTimerCancelTag = 3u;

  uintptr_t data_{0};

  SchedulerMessage() = default;

  // Takes ownership of ActorInfoPtr
  SchedulerMessage(ActorInfoPtr ptr) {
    data_ = reinterpret_cast<uintptr_t>(ptr.release());
    CHECK((data_ & kActorTagBit) == 0);
  }

  // Takes ownership of one timer ref.
  static SchedulerMessage timer_register(actor::Ref<actor::TimerNode> ref);
  static SchedulerMessage timer_cancel(actor::Ref<actor::TimerNode> ref);

  ~SchedulerMessage();

  SchedulerMessage(const SchedulerMessage&) = delete;
  SchedulerMessage& operator=(const SchedulerMessage&) = delete;

  SchedulerMessage(SchedulerMessage&& other) noexcept : data_(std::exchange(other.data_, 0)) {
  }
  SchedulerMessage& operator=(SchedulerMessage&& other) noexcept {
    if (this != &other) {
      reset();
      data_ = std::exchange(other.data_, 0);
    }
    return *this;
  }

  bool empty() const {
    return data_ == 0;
  }
  bool is_actor() const {
    return (data_ & kActorTagBit) == 0 && data_ != 0;
  }
  bool is_timer_register() const {
    return (data_ & kTimerTagMask) == kTimerRegisterTag;
  }
  bool is_timer_cancel() const {
    return (data_ & kTimerTagMask) == kTimerCancelTag;
  }

  // Transfers ownership back to ActorInfoPtr
  ActorInfoPtr as_actor() {
    CHECK(is_actor());
    auto* raw = reinterpret_cast<ActorInfoPtr::Raw*>(data_);
    data_ = 0;
    return ActorInfoPtr(ActorInfoPtr::acquire_t{}, raw);
  }

  // Transfers ownership back to Ref<TimerNode>
  actor::Ref<actor::TimerNode> as_timer_node();

  explicit operator bool() const {
    return data_ != 0;
  }

  void reset();

 private:
  static uintptr_t encode_timer_node(actor::TimerNode* node, uintptr_t tag) {
    auto p = reinterpret_cast<uintptr_t>(node);
    CHECK((p & kTimerTagMask) == 0);
    return p | tag;
  }
};

}  // namespace core
}  // namespace actor
}  // namespace td
