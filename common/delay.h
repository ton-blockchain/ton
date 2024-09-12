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

#include "td/actor/actor.h"

namespace ton {

template <typename T>
class DelayedAction : public td::actor::Actor {
 public:
  DelayedAction(T promise) : promise_(std::move(promise)) {
  }
  void set_timer(td::Timestamp t) {
    alarm_timestamp() = t;
  }
  void alarm() override {
    promise_();
    stop();
  }

  static void create(T promise, td::Timestamp t) {
    auto A = td::actor::create_actor<DelayedAction>("delayed", std::move(promise));
    td::actor::send_closure(A, &DelayedAction::set_timer, t);
    A.release();
  }

 private:
  T promise_;
};

template <typename T>
void delay_action(T promise, td::Timestamp timeout) {
  DelayedAction<T>::create(std::move(promise), timeout);
}

template <typename PromiseT, typename ValueT>
class AsyncApply : public td::actor::Actor {
 public:
  AsyncApply(PromiseT promise, ValueT value) : promise_(std::move(promise)), value_(std::move(value)){
  }

  void start_up() override {
    promise_(std::move(value_));
    stop();
  }

  static void create(td::Slice name, PromiseT promise, ValueT value ) {
    td::actor::create_actor<AsyncApply>(PSLICE() << "async:" << name, std::move(promise), std::move(value)).release();
  }

 private:
  PromiseT promise_;
  ValueT value_;
};

template <class PromiseT, class ValueT>
void async_apply(td::Slice name, PromiseT &&promise, ValueT &&value) {
  AsyncApply<PromiseT, ValueT>::create(name, std::forward<PromiseT>(promise), std::forward<ValueT>(value));
}
}  // namespace ton
