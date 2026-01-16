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
#include "td/actor/ActorId.h"

namespace td {
namespace actor {

// ActorRef: strong reference that keeps actor alive
// - Prevents actor destruction while coroutines hold refs
// - Can be created from ActorId (may fail if actor closing)
// - Copyable (each copy increments refcount)
// - Stores ActorInfoPtr to keep ActorInfo memory alive via pool refcount
template <class ActorType = core::Actor>
class ActorRef {
 public:
  ActorRef() = default;

  ActorRef(const ActorRef &other) : ptr_(other.ptr_) {
    acquire_ref_if_needed();
  }

  ActorRef &operator=(const ActorRef &other) {
    if (this != &other) {
      dec_ref_if_needed();
      ptr_ = other.ptr_;
      acquire_ref_if_needed();
    }
    return *this;
  }

  ActorRef(ActorRef &&other) noexcept : ptr_(std::move(other.ptr_)) {
  }

  ActorRef &operator=(ActorRef &&other) noexcept {
    if (this != &other) {
      dec_ref_if_needed();
      ptr_ = std::move(other.ptr_);
    }
    return *this;
  }

  ~ActorRef() {
    dec_ref_if_needed();
  }

  // Try to create ActorRef from ActorId. Returns empty if actor is closing.
  static ActorRef try_from(const ActorId<ActorType> &id) {
    if (id.empty()) {
      return {};
    }
    auto ptr = id.actor_info_ptr();
    if (ptr->try_acquire_ref()) {
      return ActorRef(std::move(ptr));
    }
    return {};
  }

  bool empty() const {
    return !ptr_;
  }

  explicit operator bool() const {
    return !empty();
  }

  ActorType &get_actor_unsafe() const {
    CHECK(!empty());
    return static_cast<ActorType &>(ptr_->actor());
  }

  core::ActorInfo &actor_info() const {
    CHECK(!empty());
    return *ptr_;
  }

  detail::ActorRef as_actor_ref() const {
    CHECK(!empty());
    return detail::ActorRef(*ptr_);
  }

 private:
  core::ActorInfoPtr ptr_;

  explicit ActorRef(core::ActorInfoPtr ptr) : ptr_(std::move(ptr)) {
  }

  void acquire_ref_if_needed() {
    if (ptr_) {
      ptr_->acquire_ref();
    }
  }

  void dec_ref_if_needed() {
    if (ptr_) {
      ptr_->dec_ref();
    }
  }
};

}  // namespace actor
}  // namespace td
