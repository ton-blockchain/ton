/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "interfaces/block.h"
#include "td/actor/common.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"
#include "td/utils/Status.h"
#include "ton/ton-types.h"

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate);

template <typename T>
class SharedFuture {
 public:
  SharedFuture() = default;

  SharedFuture(td::actor::StartedTask<T> future) : future_(std::move(future)) {
  }

  SharedFuture(SharedFuture&&) = default;
  SharedFuture& operator=(SharedFuture&&) = default;

  td::actor::Task<T> get() {
    if (is_resolved) {
      co_return value_.clone();
    }

    auto [awaiter, promise] = td::actor::StartedTask<T>::make_bridge();
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
  td::Result<T> value_;
  td::actor::StartedTask<T> future_;
  std::vector<td::Promise<T>> promises_;
  td::CancellationTokenSource cancellation_;
};

constexpr int AWAIT_TIMEOUT_CODE = 6520;

template <typename T>
td::actor::Task<T> await_with_timeout(td::actor::Task<T> task, td::Timestamp timeout) {
  auto [task_result, promise] = td::actor::StartedTask<T>::make_bridge();
  auto promise_ptr = std::make_shared<td::Promise<T>>(std::move(promise));
  if (timeout) {
    auto worker_timeout = [](td::Timestamp timeout, std::shared_ptr<td::Promise<T>> promise_ptr) -> td::actor::Task<> {
      co_await td::actor::coro_sleep(timeout);
      promise_ptr->set_error(td::Status::Error(AWAIT_TIMEOUT_CODE, "await timeout"));
      co_return {};
    };
    worker_timeout(timeout, promise_ptr).start().detach();
  }
  auto worker_wait = [](td::actor::Task<T> task, std::shared_ptr<td::Promise<T>> promise_ptr) -> td::actor::Task<> {
    promise_ptr->set_result(co_await std::move(task).wrap());
    co_return {};
  };
  worker_wait(std::move(task), std::move(promise_ptr)).start().detach();
  co_return co_await std::move(task_result);
}

}  // namespace ton::validator::consensus
