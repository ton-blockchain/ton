/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <memory>

#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"

namespace td::actor {

template <typename T>
class SharedFuture {
 public:
  SharedFuture() = delete;

  explicit SharedFuture(Task<T> future) {
    init(std::move(future));
  }
  explicit SharedFuture(StartedTask<T> future) {
    init(started_to_task(std::move(future)));
  }
  ~SharedFuture() {
    if (state_) {
      state_->cancel_source.cancel();
    }
  }

  SharedFuture(const SharedFuture&) = delete;
  SharedFuture& operator=(const SharedFuture&) = delete;
  SharedFuture(SharedFuture&&) = delete;
  SharedFuture& operator=(SharedFuture&&) = delete;

  td::actor::Task<T> get(bool propagate_cancel = false) const {
    CHECK(state_);
    return get_impl(state_, propagate_cancel);
  }

 private:
  struct State {
    CoroMutex mutex;
    std::optional<td::Result<T>> value;
    TaskCancellationSource cancel_source = TaskCancellationSource::create_detached();
  };

  static Task<T> started_to_task(StartedTask<T> started) {
    co_return co_await std::move(started).child();
  }

  void init(Task<T> future) {
    CHECK(!state_);
    state_ = std::make_shared<State>();
    auto state = state_;
    auto lock = state->mutex.lock_unsafe();
    auto task = wait_and_save(std::move(future), std::move(lock), state);
    std::move(task).start_in_parent_scope(state->cancel_source.get_scope_lease()).detach();
  }

  static Task<td::Unit> wait_and_save(Task<T> future, CoroMutex::Lock lock, std::shared_ptr<State> state) {
    SCOPE_EXIT {
      // wait_and_save may be finished by cancellation before assigning value.
      // Publish a terminal cancelled result before unlocking waiters.
      if (!state->value) {
        state->value = cancelled_status();
      }
      lock.reset();
    };
    state->value = co_await std::move(future).child().wrap();
    co_return td::Unit{};
  }

  static Task<T> get_impl(std::shared_ptr<State> state, bool propagate_cancel) {
    if (propagate_cancel) {
      current_scope_lease().publish_cancel_promise([state](td::Result<td::Unit> r_cancel) {
        if (r_cancel.is_ok()) {
          state->cancel_source.cancel();
        }
      });
    }
    auto lock = co_await state->mutex.lock();
    CHECK(state->value.has_value());
    co_return state->value->clone();
  }

  // SharedFuture is actor-confined: safe within a single actor, but not thread-safe.
  // Keep it as a stable actor member and initialize exactly once.
  std::shared_ptr<State> state_;
};

}  // namespace td::actor
