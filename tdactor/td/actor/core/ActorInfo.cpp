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
#include "td/actor/core/Actor.h"
#include "td/actor/core/ActorInfo.h"
#include "td/actor/coro_cancellation_runtime.h"

namespace td {
namespace actor {
namespace core {

bool ActorInfo::publish_coro_cancel_node(::td::actor::CancelNode &node) {
  return coro_cancel_topology_.publish_and_maybe_cancel(
      node, [&] { return coro_cancelled_.load(std::memory_order_seq_cst); });
}

bool ActorInfo::unpublish_coro_cancel_node(::td::actor::CancelNode &node) {
  return coro_cancel_topology_.unpublish_and_cleanup(node);
}

void ActorInfo::cancel_coro_cancel_nodes() {
  if (coro_cancelled_.exchange(true, std::memory_order_seq_cst)) {
    return;
  }
  coro_cancel_topology_.cancel_snapshot();
}

}  // namespace core
}  // namespace actor
}  // namespace td
