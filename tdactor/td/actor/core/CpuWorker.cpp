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
#include <coroutine>

#include "td/actor/core/ActorExecutor.h"
#include "td/actor/core/CpuWorker.h"
#include "td/actor/core/Scheduler.h"  // FIXME: afer LocalQueue is in a separate file
#include "td/actor/core/SchedulerContext.h"

namespace td {
namespace actor {
namespace core {
void CpuWorker::run() {
  auto thread_id = get_thread_id();
  auto &dispatcher = *SchedulerContext::get();

  MpmcWaiter::Slot slot;
  waiter_.init_slot(slot, thread_id);
  auto &debug = dispatcher.get_debug();
  while (true) {
    SchedulerToken token = nullptr;
    if (try_pop(token, thread_id)) {
      waiter_.stop_wait(slot);
      if (!token) {
        return;
      }
      auto encoded = reinterpret_cast<uintptr_t>(token);
      if ((encoded & 1u) == 0u) {
        // Regular actor message
        auto raw_message = reinterpret_cast<SchedulerMessage::Raw *>(token);
        SchedulerMessage message(SchedulerMessage::acquire_t{}, raw_message);
        auto lock = debug.start(message->get_name());
        ActorExecutor executor(*message, dispatcher, ActorExecutor::Options().with_from_queue());
      } else {
        // Coroutine continuation
        auto h = std::coroutine_handle<>::from_address(reinterpret_cast<void *>(encoded & ~uintptr_t(1)));
        auto lock = debug.start("coro");
        h.resume();
      }
    } else {
      waiter_.wait(slot);
    }
  }
}

bool CpuWorker::try_pop_local(SchedulerToken &token) {
  return local_queues_[id_].try_pop(token);
}

bool CpuWorker::try_pop_global(SchedulerToken &token, size_t thread_id) {
  return queue_.try_pop(token, thread_id);
}

bool CpuWorker::try_pop(SchedulerToken &token, size_t thread_id) {
  if (++cnt_ == 51) {
    cnt_ = 0;
    if (try_pop_global(token, thread_id) || try_pop_local(token)) {
      return true;
    }
  } else {
    if (try_pop_local(token) || try_pop_global(token, thread_id)) {
      return true;
    }
  }

  for (size_t i = 1; i < local_queues_.size(); i++) {
    size_t pos = (i + id_) % local_queues_.size();
    if (local_queues_[id_].steal(token, local_queues_[pos])) {
      return true;
    }
  }

  return false;
}

}  // namespace core
}  // namespace actor
}  // namespace td
