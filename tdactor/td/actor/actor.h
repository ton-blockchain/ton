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
#include "td/actor/common.h"
#include "td/actor/ActorId.h"
#include "td/actor/ActorOwn.h"
#include "td/actor/ActorShared.h"

namespace td {
namespace actor {

template <class T, class... ArgsT>
TD_WARN_UNUSED_RESULT ActorOwn<T> create_actor(ActorOptions options, ArgsT &&...args) {
  return ActorOwn<T>(ActorId<T>::create(options, std::forward<ArgsT>(args)...));
}

template <class T, class... ArgsT>
TD_WARN_UNUSED_RESULT ActorOwn<T> create_actor(Slice name, ArgsT &&...args) {
  return ActorOwn<T>(ActorId<T>::create(ActorOptions().with_name(name), std::forward<ArgsT>(args)...));
}

namespace internal {

template <bool Later, class ActorIdT, class FunctionT, class... ArgsT,
          class FunctionClassT = member_function_class_t<FunctionT>>
void send_closure_dispatch(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  using ActorT = typename std::decay_t<ActorIdT>::ActorT;
  static_assert(std::is_base_of<FunctionClassT, ActorT>::value, "unsafe send_closure");

  constexpr size_t argument_count = member_function_argument_count<FunctionT>();

  ActorIdT id = std::forward<ActorIdT>(actor_id);
  if constexpr (argument_count == sizeof...(ArgsT)) {
    if constexpr (Later) {
      detail::send_closure_later(id.as_actor_ref(), function, std::forward<ArgsT>(args)...);
    } else {
      detail::send_closure(id.as_actor_ref(), function, std::forward<ArgsT>(args)...);
    }
  } else {
    auto closure = call_n_arguments<argument_count>(
        [&function](auto &&...nargs) {
          if constexpr (Later) {
            return create_delayed_closure(function, std::forward<decltype(nargs)>(nargs)...);
          } else {
            return create_immediate_closure(function, std::forward<decltype(nargs)>(nargs)...);
          }
        },
        std::forward<ArgsT>(args)...);
    auto promise = get_last_argument(std::forward<ArgsT>(args)...);
    if constexpr (Later) {
      detail::send_closure_with_promise_later(id.as_actor_ref(), std::move(closure), std::move(promise));
    } else {
      detail::send_closure_with_promise(id.as_actor_ref(), std::move(closure), std::move(promise));
    }
  }
}

}  // namespace internal

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count == sizeof...(ArgsT), bool> with_promise = false>
void send_closure_immediate(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<false>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count != sizeof...(ArgsT), bool> with_promise = true>
void send_closure_immediate(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<false>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count == sizeof...(ArgsT), bool> with_promise = false>
void send_closure(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<true>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count != sizeof...(ArgsT), bool> with_promise = true>
void send_closure(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<true>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count == sizeof...(ArgsT), bool> with_promise = false>
auto future_send_closure(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  using R = ::td::detail::get_ret_t<std::decay_t<FunctionT>>;
  auto pf = make_promise_future<R>();
  send_closure(std::forward<ActorIdT>(actor_id), std::move(function), std::forward<ArgsT>(args)...,
               std::move(pf.first));
  return std::move(pf.second);
}

template <class R, class ActorIdT, class FunctionT, class... ArgsT,
          class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count != sizeof...(ArgsT), bool> with_promise = true>
Future<R> future_send_closure(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  auto pf = make_promise_future<R>();
  send_closure(std::forward<ActorIdT>(actor_id), std::move(function), std::forward<ArgsT>(args)...,
               std::move(pf.first));
  return std::move(pf.second);
}

template <typename ActorIdT, typename FunctionT, typename... ArgsT>
bool send_closure_bool(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  send_closure(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
  return true;
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count == sizeof...(ArgsT), bool> with_promise = false>
void send_closure_later(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<true>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <class ActorIdT, class FunctionT, class... ArgsT, class FunctionClassT = member_function_class_t<FunctionT>,
          size_t argument_count = member_function_argument_count<FunctionT>(),
          std::enable_if_t<argument_count != sizeof...(ArgsT), bool> with_promise = true>
void send_closure_later(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  internal::send_closure_dispatch<true>(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
}

template <typename ActorIdT, typename FunctionT, typename... ArgsT>
bool send_closure_later_bool(ActorIdT &&actor_id, FunctionT function, ArgsT &&...args) {
  send_closure_later(std::forward<ActorIdT>(actor_id), function, std::forward<ArgsT>(args)...);
  return true;
}

template <class ActorIdT, class... ArgsT>
void send_lambda(ActorIdT &&actor_id, ArgsT &&...args) {
  ActorIdT id = std::forward<ActorIdT>(actor_id);
  detail::send_lambda(id.as_actor_ref(), std::forward<ArgsT>(args)...);
}
template <class ActorIdT, class... ArgsT>
void send_lambda_later(ActorIdT &&actor_id, ArgsT &&...args) {
  ActorIdT id = std::forward<ActorIdT>(actor_id);
  detail::send_lambda_later(id.as_actor_ref(), std::forward<ArgsT>(args)...);
}
template <class ActorIdT>
void send_signals(ActorIdT &&actor_id, ActorSignals signals) {
  ActorIdT id = std::forward<ActorIdT>(actor_id);
  detail::send_signals(id.as_actor_ref(), signals);
}
template <class ActorIdT>
void send_signals_later(ActorIdT &&actor_id, ActorSignals signals) {
  ActorIdT id = std::forward<ActorIdT>(actor_id);
  detail::send_signals_later(id.as_actor_ref(), signals);
}
}  // namespace actor

class SendClosure {
 public:
  template <class... ArgsT>
  void operator()(ArgsT &&...args) const {
    td::actor::send_closure(std::forward<ArgsT>(args)...);
  }
};

template <class T>
template <class... ArgsT>
auto Promise<T>::send_closure(ArgsT &&...args) {
  return [promise = std::move(*this), t = std::make_tuple(std::forward<ArgsT>(args)...)](auto &&r_res) mutable {
    TRY_RESULT_PROMISE(promise, res, std::move(r_res));
    td::call_tuple(SendClosure(), std::tuple_cat(std::move(t), std::make_tuple(std::move(res), std::move(promise))));
  };
}

template <class... ArgsT>
auto promise_send_closure(ArgsT &&...args) {
  return [t = std::make_tuple(std::forward<ArgsT>(args)...)](auto &&res) mutable {
    td::call_tuple(SendClosure(), std::tuple_cat(std::move(t), std::make_tuple(std::move(res))));
  };
}

}  // namespace td
