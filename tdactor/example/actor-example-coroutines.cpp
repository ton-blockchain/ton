#include "absl/status/status.h"
#include "td/actor/coro.h"
#include "td/actor/actor.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/port/sleep.h"

#include <future>
#include <string>
#include <memory>
#include <numeric>
#include <random>
#include <thread>

using namespace td::actor;

Task<td::Unit> example_create() {
  LOG(INFO) << "Detach";
  Task<int> value = []() -> Task<int> {
    LOG(FATAL) << "This line will not be executed";
    co_return 17;
  }();
  value.detach();

  LOG(INFO) << "Simple co_await";
  // Task will be started after co await has been called
  Task<int> value2 = []() -> Task<int> { co_return 17; }();
  CHECK(17 == co_await std::move(value2));

  LOG(INFO) << "start_immediate than co_await";
  // Task is started immediately
  StartedTask<int> value3 = []() -> Task<int> {
    td::usleep_for(1000000);
    co_return 17;
  }()
                                        .start_immediate();
  CHECK(value3.await_ready());
  CHECK(17 == (co_await std::move(value3)));

  LOG(INFO) << "start than co_await";
  // Task is started on scheduler
  StartedTask<int> value4 = []() -> Task<int> {
    td::usleep_for(1000000);
    co_return 17;
  }()
                                        .start();
  CHECK(!value4.await_ready());
  CHECK(17 == (co_await std::move(value4)));

  StartedTask<int> value5 = spawn_actor("worker", []() -> Task<int> {
    // This code will be run on some actor
    // Main reason to use it is actor statistics
    co_return 17;
  }());
  CHECK(17 == (co_await std::move(value5)));
  co_return td::Unit();
}

Task<td::Unit> example_communicate() {
  LOG(INFO) << "Communicate with actor";
  struct Worker : public Actor {
    int square(int x) {
      return x * x;
    }
    Task<int> square_task(int x) {
      co_return square(x);
    }
    void square_promise(int x, td::Promise<int> promise) {
      send_closure(actor_id(this), &Worker::square_task, x, std::move(promise));
    }
  };
  auto worker = create_actor<Worker>("worker");

  StartedTask<int> value6 = ask(worker, &Worker::square, 17);
  CHECK(289 == (co_await std::move(value6)));
  StartedTask<int> value7 = ask(worker, &Worker::square_promise, 17);
  CHECK(289 == (co_await std::move(value7)));
  StartedTask<int> value8 = ask(worker, &Worker::square_task, 17);
  CHECK(289 == (co_await std::move(value8)));
  co_return td::Unit();
}

Task<int> task_error() {
  co_return td::Status::Error("test error");
}
td::Result<int> result_error() {
 return td::Status::Error("test error");
}
Task<int> pass_task_error() {
  co_await task_error();
  co_return 17;
}
Task<int> pass_result_error() {
  co_await result_error();
  co_return 17;
}

Task<td::Unit> example_error_handling() {
  // Error handling
  (co_await pass_task_error().wrap()).ensure_error();
  (co_await pass_result_error().wrap()).ensure_error();
  (co_await result_error().wrap()).ensure_error();

  co_return td::Unit();
}

Task<td::Unit> example_actor() {
  struct TaskActor : public Actor {
    TaskActor(td::Promise<int> promise) : promise_(std::move(promise)) {

    }
    void start_up() override {
      // it is usual actor all coroutines create FROM actor, will be executed ON actor
      run().start().detach();
    }
    Task<td::Unit> run() {
      state_ = 19;
      finish();
      co_return td::Unit();
    }
  private:
    td::Promise<int> promise_;
    int state_ {17};

    void finish() {
      promise_.set_result(state_);
      stop();
    }
  };

  auto [task, promise] = StartedTask<int>::make_bridge();
  auto task_actor = create_actor<TaskActor>("task_actor", std::move(promise));
  CHECK(19 == (co_await std::move(task)));
  co_return td::Unit();

}

Task<td::Unit> example_all() {
  std::vector<StartedTask<int>> v;
  int n = std::thread::hardware_concurrency();
  for (int i = 0; i < n; i++) {
    v.push_back([](int i) -> Task<int> {
      td::usleep_for(1000000);
      co_return i* i;
    }(i)
                                 .start());
  }
  auto vv = co_await all(std::move(v));
  for (int i = 0; i < n; i++) {
    CHECK(vv[i] == i * i);
  }
  co_return td::Unit();
}

Task<td::Unit> run_all_examples() {
  co_await example_create();
  co_await example_communicate();
  co_await example_error_handling();
  co_await example_actor();
  co_await example_all();
  co_return td::Unit();
}

Task<td::Unit> example() {
  LOG(INFO) << "Start example coroutine";

  (co_await run_all_examples().wrap()).ensure();

  LOG(INFO) << "Finish example coroutine and stop scheduler";
  td::actor::SchedulerContext::get()->stop();
  co_return td::Unit();
}

int main() {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));
  td::actor::Scheduler scheduler({std::thread::hardware_concurrency()});

  scheduler.run_in_context([&] { (void)example().start(); });

  scheduler.run();
  LOG(INFO) << "DONE";
  return 0;
}