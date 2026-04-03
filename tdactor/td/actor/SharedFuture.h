/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "td/actor/coro_task.h"
#include "td/utils/CancellationToken.h"

#include "coro_utils.h"

namespace td::actor {

template <typename T>
class SharedFuture {
 public:
  SharedFuture() = default;

  SharedFuture(StartedTask<T> future) : future_(std::move(future)) {
  }

  SharedFuture(SharedFuture&&) = default;
  SharedFuture& operator=(SharedFuture&&) = default;

  Task<T> get() {
    if (is_resolved) {
      co_return value_.clone();
    }

    auto [awaiter, promise] = StartedTask<T>::make_bridge();
    promises_.push_back(std::move(promise));

    if (promises_.size() == 1) {
      CHECK(future_.valid());
      auto token = cancellation_.get_cancellation_token();
      auto result = co_await std::move(future_).wrap();
      co_await token.check();
      value_ = std::move(result);
      is_resolved = true;
      for (auto& p : promises_) {
        p.set_result(value_.clone());
      }
    }

    co_return co_await std::move(awaiter);
  }

 private:
  bool is_resolved = false;
  Result<T> value_;
  StartedTask<T> future_;
  std::vector<Promise<T>> promises_;
  CancellationTokenSource cancellation_;
};

constexpr int AWAIT_TIMEOUT_CODE = 6520;

template <typename T>
Task<T> await_with_timeout(StartedTask<T> task, Timestamp timeout) {
  auto [task_result, promise] = StartedTask<T>::make_bridge();
  auto promise_ptr = std::make_shared<Promise<T>>(std::move(promise));
  if (timeout) {
    auto worker_timeout = [](Timestamp timeout, std::shared_ptr<Promise<T>> promise_ptr) -> Task<> {
      co_await coro_sleep(timeout);
      promise_ptr->set_error(Status::Error(AWAIT_TIMEOUT_CODE, "await timeout"));
      co_return {};
    };
    worker_timeout(timeout, promise_ptr).start().detach();
  }
  auto worker_wait = [](StartedTask<T> task, std::shared_ptr<Promise<T>> promise_ptr) -> Task<> {
    promise_ptr->set_result(co_await std::move(task).wrap());
    co_return {};
  };
  worker_wait(std::move(task), std::move(promise_ptr)).start().detach();
  co_return co_await std::move(task_result);
}

template <typename T>
Task<T> await_with_timeout(Task<T> task, Timestamp timeout) {
  co_return co_await await_with_timeout(std::move(task).start(), timeout);
}

}  // namespace td::actor
