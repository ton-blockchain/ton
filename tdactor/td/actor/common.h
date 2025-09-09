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
#include "td/actor/core/Actor.h"
#include "td/actor/core/ActorSignals.h"
#include "td/actor/core/ActorTypeStat.h"
#include "td/actor/core/SchedulerId.h"
#include "td/actor/core/SchedulerContext.h"
#include "td/actor/core/Scheduler.h"

#include "td/actor/PromiseFuture.h"

#include "td/utils/Timer.h"

namespace td {
namespace actor {
using core::ActorOptions;

// Replacement for core::ActorSignals. Easier to use and do not allow internal signals
class ActorSignals {
 public:
  static ActorSignals pause() {
    return ActorSignals(core::ActorSignals::one(core::ActorSignals::Pause));
  }
  static ActorSignals kill() {
    return ActorSignals(core::ActorSignals::one(core::ActorSignals::Kill));
  }
  static ActorSignals wakeup() {
    return ActorSignals(core::ActorSignals::one(core::ActorSignals::Wakeup));
  }
  friend ActorSignals operator|(ActorSignals a, ActorSignals b) {
    a.raw_.add_signals(b.raw_);
    return a;
  }
  ActorSignals() = default;

  core::ActorSignals raw() const {
    return raw_;
  }

 private:
  ActorSignals(core::ActorSignals raw) : raw_(raw) {
  }
  core::ActorSignals raw_;
};

// TODO: proper interface
using core::Actor;
using core::SchedulerContext;
using core::SchedulerId;
using core::set_debug;

struct Debug {
 public:
  Debug() = default;
  Debug(core::SchedulerGroupInfo *group_info) : group_info_(group_info) {
  }
  template <class F>
  void for_each(F &&f) {
    for (auto &scheduler : group_info_->schedulers) {
      f(scheduler.io_worker->debug);
      for (auto &cpu : scheduler.cpu_workers) {
        f(cpu->debug);
      }
    }
  }

  void dump(td::StringBuilder &sb) {
    sb << "list of active actors with names:\n";
    for_each([&](core::Debug &debug) {
      core::DebugInfo info;
      debug.read(info);
      if (info.is_active) {
        sb << "\t\"" << info.name << "\" is active for " << Time::now() - info.start_at << "s\n";
      }
    });
    sb << "\nsizes of cpu local queues:\n";
    for (auto &scheduler : group_info_->schedulers) {
      for (size_t i = 0; i < scheduler.cpu_threads_count; i++) {
        auto size = scheduler.cpu_local_queue[i].size();
        if (size != 0) {
          sb << "\tcpu#" << i << " queue.size() = " << size << "\n";
        }
      }
    }
    sb << "\n";
  }

 private:
  core::SchedulerGroupInfo *group_info_;
};

class Scheduler {
 public:
  struct NodeInfo {
    NodeInfo(size_t cpu_threads) : cpu_threads_(cpu_threads) {
    }
    NodeInfo(size_t cpu_threads, size_t io_threads) : cpu_threads_(cpu_threads), io_threads_(io_threads) {
    }
    size_t cpu_threads_;
    size_t io_threads_{1};
  };

  enum Mode { Running, Paused };
  Scheduler(std::vector<NodeInfo> infos, bool skip_timeouts = false, Mode mode = Paused)
      : infos_(std::move(infos)), skip_timeouts_(skip_timeouts) {
    init();
    if (mode == Running) {
      start();
    }
  }

  ~Scheduler() {
    stop();
  }
  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  void start() {
    if (is_started_) {
      return;
    }
    is_started_ = true;
    for (size_t it = 0; it < schedulers_.size(); it++) {
      auto &scheduler = schedulers_[it];
      scheduler->start();
      if (it != 0) {
        auto thread = td::thread([&] {
          while (scheduler->run(10)) {
          }
        });
        thread.set_name(PSLICE() << "#" << it << ":io");
        thread.detach();
      }
    }
  }

  Debug get_debug() {
    return Debug{group_info_.get()};
  }

  bool run() {
    start();
    while (schedulers_[0]->run(10)) {
    }
    return false;
  }

  bool run(double timeout) {
    start();
    return schedulers_[0]->run(timeout);
  }

  template <class F>
  void run_in_context(F &&f) {
    schedulers_[0]->run_in_context(std::forward<F>(f));
  }

  template <class F>
  void run_in_context_external(F &&f) {
    schedulers_[0]->run_in_context_external(std::forward<F>(f));
  }

  void stop() {
    if (!group_info_) {
      return;
    }
    if (!is_started_) {
      start();
    }
    schedulers_[0]->stop();
    run();
    core::Scheduler::close_scheduler_group(*group_info_);
    group_info_.reset();
  }

 private:
  std::vector<NodeInfo> infos_{td::thread::hardware_concurrency()};
  std::shared_ptr<core::SchedulerGroupInfo> group_info_;
  std::vector<td::unique_ptr<core::Scheduler>> schedulers_;
  bool is_started_{false};
  bool skip_timeouts_{false};

  void init() {
    CHECK(infos_.size() < 256);
    CHECK(!group_info_);
    group_info_ = std::make_shared<core::SchedulerGroupInfo>(infos_.size());
    td::uint8 id = 0;
    for (const auto &info : infos_) {
      schedulers_.emplace_back(
          td::make_unique<core::Scheduler>(group_info_, core::SchedulerId{id}, info.cpu_threads_, skip_timeouts_));
      id++;
    }
  }
};

using core::ActorTypeStat;
using core::ActorTypeStatManager;
using core::ActorTypeStats;

// Some helper functions. Not part of public interface and not part
// of namespace core
namespace detail {
template <class LambdaT>
class ActorMessageLambda : public core::ActorMessageImpl {
 public:
  template <class FromLambdaT>
  explicit ActorMessageLambda(FromLambdaT &&lambda) : lambda_(std::forward<FromLambdaT>(lambda)) {
  }
  void run() override {
    //delivery_warning_.reset();
    lambda_();
  }

 private:
  LambdaT lambda_;
  //PerfWarningTimer delivery_warning_{"Long message delivery", 2};
};

class ActorMessageCreator {
 public:
  template <class F>
  static auto lambda(F &&f) {
    return core::ActorMessage(std::make_unique<ActorMessageLambda<std::decay_t<F>>>(std::forward<F>(f)));
  }

  static auto hangup() {
    return core::ActorMessage(std::make_unique<core::ActorMessageHangup>());
  }

  static auto hangup_shared() {
    return core::ActorMessage(std::make_unique<core::ActorMessageHangupShared>());
  }

  // Use faster allocation?
};
struct ActorRef {
  ActorRef(core::ActorInfo &actor_info, uint64 link_token = core::EmptyLinkToken)
      : actor_info(actor_info), link_token(link_token) {
  }

  core::ActorInfo &actor_info;
  uint64 link_token;
};

template <class T>
T &current_actor() {
  return static_cast<T &>(core::ActorExecuteContext::get()->actor());
}

inline void send_message(core::ActorInfo &actor_info, core::ActorMessage message) {
  auto scheduler_context_ptr = core::SchedulerContext::get();
  if (scheduler_context_ptr == nullptr) {
    //LOG(ERROR) << "send to actor is silently ignored";
    return;
  }
  auto &scheduler_context = *scheduler_context_ptr;
  core::ActorExecutor executor(actor_info, scheduler_context,
                               core::ActorExecutor::Options().with_has_poll(scheduler_context.has_poll()));
  executor.send(std::move(message));
}

inline void send_message(ActorRef actor_ref, core::ActorMessage message) {
  message.set_link_token(actor_ref.link_token);
  send_message(actor_ref.actor_info, std::move(message));
}

inline void send_message_later(core::ActorInfo &actor_info, core::ActorMessage message) {
  auto scheduler_context_ptr = core::SchedulerContext::get();
  if (scheduler_context_ptr == nullptr) {
    //LOG(ERROR) << "send to actor is silently ignored";
    return;
  }
  auto &scheduler_context = *scheduler_context_ptr;
  core::ActorExecutor executor(actor_info, scheduler_context,
                               core::ActorExecutor::Options().with_has_poll(scheduler_context.has_poll()));
  message.set_big();
  executor.send(std::move(message));
}

inline void send_message_later(ActorRef actor_ref, core::ActorMessage message) {
  message.set_link_token(actor_ref.link_token);
  send_message_later(actor_ref.actor_info, std::move(message));
}

template <class ClosureT>
auto run_on_current_actor(ClosureT &&closure) {
  using ActorType = typename std::remove_cvref_t<ClosureT>::ActorType;
  return closure.run(&current_actor<ActorType>());
}

template <class ExecuteF, class ToMessageF>
void send_immediate(ActorRef actor_ref, ExecuteF &&execute, ToMessageF &&to_message) {
  auto scheduler_context_ptr = core::SchedulerContext::get();
  if (scheduler_context_ptr == nullptr) {
    //LOG(ERROR) << "send to actor is silently ignored";
    return;
  }
  auto &scheduler_context = *scheduler_context_ptr;
  core::ActorExecutor executor(actor_ref.actor_info, scheduler_context,
                               core::ActorExecutor::Options().with_has_poll(scheduler_context.has_poll()));
  if (executor.can_send_immediate()) {
    return executor.send_immediate(execute, actor_ref.link_token);
  }
  auto message = to_message();
  message.set_link_token(actor_ref.link_token);
  executor.send(std::move(message));
}

template <class F>
void send_lambda_immediate(ActorRef actor_ref, F &&lambda) {
  send_immediate(actor_ref, lambda, [&lambda]() mutable { return ActorMessageCreator::lambda(std::move(lambda)); });
}
template <class F>
void send_lambda_later(ActorRef actor_ref, F &&lambda) {
  send_message_later(actor_ref, ActorMessageCreator::lambda(std::move(lambda)));
}

template <class F>
void send_lambda(ActorRef actor_ref, F &&lambda) {
  send_lambda_immediate(actor_ref, std::forward<F>(lambda));
}

template <class ClosureT>
void send_closure_impl(ActorRef actor_ref, ClosureT &&closure) {
  send_immediate(
      actor_ref, [&closure]() mutable { run_on_current_actor(closure); },
      [&closure]() mutable {
        return ActorMessageCreator::lambda(
            [closure = to_delayed_closure(std::move(closure))]() mutable { run_on_current_actor(closure); });
      });
}

template <class... ArgsT>
void send_closure(ActorRef actor_ref, ArgsT &&...args) {
  send_closure_impl(actor_ref, create_immediate_closure(std::forward<ArgsT>(args)...));
}

template <class ClosureT>
void send_closure_later_impl(ActorRef actor_ref, ClosureT &&closure) {
  send_message_later(
      actor_ref, ActorMessageCreator::lambda(
                     [closure = to_delayed_closure(std::move(closure))]() mutable { run_on_current_actor(closure); }));
}

// Helper to unwrap Result<T> to T, otherwise keep the type as is
template <class T>
struct unwrap_result {
  using type = T;
};
template <class T>
struct unwrap_result<td::Result<T>> {
  using type = T;
};
template <class T>
using unwrap_result_t = typename unwrap_result<T>::type;

template <class ClosureT, class PromiseT>
void send_closure_with_promise(ActorRef actor_ref, ClosureT &&closure, PromiseT &&promise) {
  using RawResultType = decltype(run_on_current_actor(closure));
  if constexpr (std::is_void_v<RawResultType>) {
    // Adapt void to td::Unit
    auto &&promise_i = promise_interface<td::Unit>(std::forward<PromiseT>(promise));
    send_immediate(
        actor_ref,
        [&closure, &promise = promise_i]() mutable {
          run_on_current_actor(closure);
          promise(td::Unit());
        },
        [&closure, &promise = promise_i]() mutable {
          return ActorMessageCreator::lambda(
              [closure = to_delayed_closure(std::move(closure)), promise = std::move(promise)]() mutable {
                run_on_current_actor(closure);
                promise(td::Unit());
              });
        });
  } else {
    using ResultType = unwrap_result_t<RawResultType>;
    auto &&promise_i = promise_interface<ResultType>(std::forward<PromiseT>(promise));
    send_immediate(
        actor_ref,
        [&closure, &promise = promise_i]() mutable { connect(std::move(promise), run_on_current_actor(closure)); },
        [&closure, &promise = promise_i]() mutable {
          return ActorMessageCreator::lambda(
              [closure = to_delayed_closure(std::move(closure)), promise = std::move(promise)]() mutable {
                connect(std::move(promise), run_on_current_actor(closure));
              });
        });
  }
}

template <class ClosureT, class PromiseT>
void send_closure_with_promise_later(ActorRef actor_ref, ClosureT &&closure, PromiseT &&promise) {
  using RawResultType = decltype(run_on_current_actor(closure));
  if constexpr (std::is_void_v<RawResultType>) {
    // Adapt void to td::Unit
    send_message_later(
        actor_ref,
        ActorMessageCreator::lambda([closure = to_delayed_closure(std::move(closure)),
                                     promise = promise_interface<td::Unit>(std::forward<PromiseT>(promise))]() mutable {
          run_on_current_actor(closure);
          promise(td::Unit());
        }));
  } else {
    using ResultType = unwrap_result_t<RawResultType>;
    send_message_later(actor_ref, ActorMessageCreator::lambda([closure = to_delayed_closure(std::move(closure)),
                                                               promise = promise_interface<ResultType>(
                                                                   std::forward<PromiseT>(promise))]() mutable {
                         connect(std::move(promise), run_on_current_actor(closure));
                       }));
  }
}

template <class... ArgsT>
void send_closure_later(ActorRef actor_ref, ArgsT &&...args) {
  send_closure_later_impl(actor_ref, create_delayed_closure(std::forward<ArgsT>(args)...));
}

inline void send_signals(ActorRef actor_ref, ActorSignals signals) {
  auto scheduler_context_ptr = core::SchedulerContext::get();
  if (scheduler_context_ptr == nullptr) {
    //LOG(ERROR) << "send to actor is silently ignored";
    return;
  }
  auto &scheduler_context = *scheduler_context_ptr;
  core::ActorExecutor executor(
      actor_ref.actor_info, scheduler_context,
      core::ActorExecutor::Options().with_has_poll(scheduler_context.has_poll()).with_signals(signals.raw()));
}

inline void send_signals_later(ActorRef actor_ref, ActorSignals signals) {
  auto scheduler_context_ptr = core::SchedulerContext::get();
  if (scheduler_context_ptr == nullptr) {
    //LOG(ERROR) << "send to actor is silently ignored";
    return;
  }
  auto &scheduler_context = *scheduler_context_ptr;
  core::ActorExecutor executor(actor_ref.actor_info, scheduler_context,
                               core::ActorExecutor::Options()
                                   .with_has_poll(scheduler_context.has_poll())
                                   .with_signals((signals | ActorSignals::pause()).raw()));
}

inline void register_actor_info_ptr(core::ActorInfoPtr actor_info_ptr) {
  auto state = actor_info_ptr->state().get_flags_unsafe();
  actor_info_ptr->on_add_to_queue();
  core::SchedulerContext::get()->add_to_queue(std::move(actor_info_ptr), state.get_scheduler_id(), !state.is_shared());
}

template <class T, class... ArgsT>
core::ActorInfoPtr create_actor(core::ActorOptions &options, ArgsT &&...args) noexcept {
  auto *scheduler_context = core::SchedulerContext::get();
  if (!options.has_scheduler()) {
    options.on_scheduler(scheduler_context->get_scheduler_id());
  }
  options.with_actor_stat_id(core::ActorTypeStatImpl::get_unique_id<T>());
  auto res =
      scheduler_context->get_actor_info_creator().create(std::make_unique<T>(std::forward<ArgsT>(args)...), options);
  register_actor_info_ptr(res);
  return res;
}
}  // namespace detail
}  // namespace actor
}  // namespace td
