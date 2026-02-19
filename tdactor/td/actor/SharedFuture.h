/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_task.h"

namespace td::actor {

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
      if (value_.has_value()) {
        co_return *value_;
      } else {
        co_return get_error();
      }
    }

    auto [awaiter, promise] = td::actor::StartedTask<T>::make_bridge();
    promises_.push_back(std::move(promise));

    if (promises_.size() == 1) {
      CHECK(future_.valid());
      td::Result<T> result = co_await std::move(future_).wrap();

      is_resolved = true;

      if (result.is_error()) {
        promises_[0].set_error(result.move_as_error());
        for (size_t i = 1; i < promises_.size(); i++) {
          promises_[i].set_error(get_error());
        }
      } else {
        value_.emplace(result.move_as_ok());
        for (auto& p : promises_) {
          auto value_copy = *value_;
          p.set_result(std::move(value_copy));
        }
      }
    }

    co_return co_await std::move(awaiter);
  }

 private:
  auto get_error() {
    return td::Status::Error("SharedFuture<T> already resolved with error");
  }

  bool is_resolved = false;
  std::optional<T> value_;
  td::actor::StartedTask<T> future_;
  std::vector<td::Promise<T>> promises_;
};

}  // namespace td::actor
