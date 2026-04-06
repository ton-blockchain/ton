/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <optional>
#include <queue>

#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"

namespace td::actor {

template <typename T>
class BackpressureQueueActor : public Actor {
 public:
  explicit BackpressureQueueActor(size_t capacity) : capacity_(capacity) {
    CHECK(capacity > 0);
  }

  // Push an item. If block=true and queue is full, suspends until space is available.
  // If block=false and queue is full, returns false immediately.
  // Returns false if closed.
  Task<bool> push(T item, bool block = true) {
    while (true) {
      if (closed_) {
        co_return false;
      }
      if (items_.size() < capacity_) {
        items_.push(std::move(item));
        wake_pop_waiter();
        co_return true;
      }
      if (!block) {
        co_return false;
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      push_waiters_.push_back(std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_error()) {
        co_return false;
      }
    }
  }

  // Pop an item. If block=true and queue is empty, suspends until an item arrives.
  // If block=false and queue is empty, returns error immediately.
  // Returns error if closed and empty.
  Task<T> pop(bool block = true) {
    while (true) {
      if (!items_.empty()) {
        auto item = std::move(items_.front());
        items_.pop();
        wake_push_waiter();
        co_return std::move(item);
      }
      if (closed_ || !block) {
        co_return td::Status::Error("BackpressureQueue is empty");
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      pop_waiters_.push_back(std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_error()) {
        if (!items_.empty()) {
          auto item = std::move(items_.front());
          items_.pop();
          wake_push_waiter();
          co_return std::move(item);
        }
        co_return td::Status::Error("BackpressureQueue is closed");
      }
    }
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    for (auto& w : push_waiters_) {
      w.set_error(td::Status::Error("BackpressureQueue is closed"));
    }
    push_waiters_.clear();
    for (auto& w : pop_waiters_) {
      w.set_error(td::Status::Error("BackpressureQueue is closed"));
    }
    pop_waiters_.clear();
  }

 private:
  std::queue<T> items_;
  size_t capacity_;
  bool closed_ = false;

  using Waiter = typename StartedTask<td::Unit>::ExternalPromise;
  std::vector<Waiter> pop_waiters_;
  std::vector<Waiter> push_waiters_;

  void wake_pop_waiter() {
    if (!pop_waiters_.empty()) {
      auto w = std::move(pop_waiters_.front());
      pop_waiters_.erase(pop_waiters_.begin());
      w.set_value(td::Unit());
    }
  }

  void wake_push_waiter() {
    if (!push_waiters_.empty()) {
      auto w = std::move(push_waiters_.front());
      push_waiters_.erase(push_waiters_.begin());
      w.set_value(td::Unit());
    }
  }
};

// Copyable handle to a BackpressureQueue actor. Safe to pass into lambdas.
template <typename T>
class BackpressureQueue {
  using A = BackpressureQueueActor<T>;

 public:
  BackpressureQueue() = default;

  explicit BackpressureQueue(td::Slice name, size_t capacity) {
    actor_ = std::make_shared<ActorOwn<A>>(create_actor<A>(name, capacity));
  }

  StartedTask<bool> push(T item) {
    return ask(*actor_, &A::push, std::move(item), true);
  }

  StartedTask<bool> try_push(T item) {
    return ask(*actor_, &A::push, std::move(item), false);
  }

  StartedTask<T> pop() {
    return ask(*actor_, &A::pop, true);
  }

  StartedTask<T> try_pop() {
    return ask(*actor_, &A::pop, false);
  }

  void close() {
    send_closure(*actor_, &A::close);
  }

 private:
  std::shared_ptr<ActorOwn<A>> actor_;
};

}  // namespace td::actor
