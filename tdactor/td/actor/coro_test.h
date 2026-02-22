#pragma once

#include "td/actor/actor.h"
#include "td/actor/coro.h"
#include "td/utils/tests.h"

inline void run_coro_test(td::actor::Task<td::Unit> task) {
  class Runner final : public td::actor::Actor {
   public:
    explicit Runner(td::actor::Task<td::Unit> t) : task_(std::move(t)) {
    }
    void start_up() override {
      [](td::actor::Task<td::Unit> test) -> td::actor::Task<td::Unit> {
        (co_await std::move(test).wrap()).ensure();
        co_await td::actor::yield_on_current();
        td::actor::SchedulerContext::get().stop();
        co_return td::Unit{};
      }(std::move(task_))
                                                .start_immediate_without_scope()
                                                .detach("CoroTestRunner");
    }

   private:
    td::actor::Task<td::Unit> task_;
  };
  td::actor::create_actor<Runner>("CoroTestRunner", std::move(task)).release();
}

#define TEST_CORO_IMPL(test_name)                                                         \
  static ::td::actor::Task<td::Unit> TD_CONCAT(coro_body_, test_name)();                  \
  TEST_IMPL(test_name) {                                                                  \
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));                                            \
    td::actor::Scheduler scheduler({4});                                                  \
    scheduler.run_in_context([&] { run_coro_test(TD_CONCAT(coro_body_, test_name)()); }); \
    scheduler.run();                                                                      \
  }                                                                                       \
  static ::td::actor::Task<td::Unit> TD_CONCAT(coro_body_, test_name)()

#define TEST_CORO(test_case_name, test_name) TEST_CORO_IMPL(TEST_NAME(test_case_name, test_name))
