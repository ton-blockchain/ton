#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <memory>
#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro.h"

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <ctime>
#endif

using namespace td::actor;

struct SchedulerGuard {
  Scheduler sched{std::vector<Scheduler::NodeInfo>{Scheduler::NodeInfo{10}}};

  template <class T>
  void run_until_done(td::Slice name, Task<T> task) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    td::actor::ActorOwn<> actor_id;

    sched.run_in_context([&] {
      actor_id = td::actor::create_actor<td::actor::Actor>(name);
      task.set_executor(Executor::on_actor(actor_id.get()));

      auto wrapped_task = [](auto done, Task<T> task) -> Task<td::Unit> {
        auto r = co_await std::move(task).wrap();
        r.ensure();
        done->store(true, std::memory_order_release);
        co_return td::Unit{};
      }(done, std::move(task));

      (void)std::move(wrapped_task).start();
    });

    while (!done->load(std::memory_order_acquire)) {
      sched.run(0.001);
    }
  }
};

static SchedulerGuard& guard() {
  static SchedulerGuard g;
  return g;
}

static inline double process_cpu_seconds() {
#if defined(_WIN32)
  FILETIME creation, exit, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    auto to_seconds = [](const FILETIME& ft) {
      ULARGE_INTEGER uli{.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
      return uli.QuadPart * 1e-7;  // 100ns → seconds
    };
    return to_seconds(kernel) + to_seconds(user);
  }
  return 0.0;
#elif defined(__APPLE__) || defined(__linux__)
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    auto to_seconds = [](const timeval& tv) { return tv.tv_sec + tv.tv_usec * 1e-6; };
    return to_seconds(ru.ru_utime) + to_seconds(ru.ru_stime);
  }
  return 0.0;
#else
  return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

template <class F>
void coro_benchmark(benchmark::State& state, F&& benchmark_code) {
  const auto real_start = std::chrono::steady_clock::now();
  const double cpu_start = process_cpu_seconds();

  auto task = benchmark_code(state);
  guard().run_until_done(state.name(), std::move(task));

  const double cpu_elapsed = process_cpu_seconds() - cpu_start;
  const double real_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - real_start).count();
  const double iterations = static_cast<double>(state.iterations());

  if (iterations > 0 && cpu_elapsed > 0 && real_elapsed > 0) {
    state.counters["cpu_time"] = benchmark::Counter(cpu_elapsed / iterations);
    state.counters["cpu_speed"] = benchmark::Counter(iterations / cpu_elapsed, benchmark::Counter::kIsRate);
    state.counters["real_time"] = benchmark::Counter(real_elapsed / iterations);
    state.counters["real_speed"] = benchmark::Counter(iterations / real_elapsed, benchmark::Counter::kIsRate);
    state.counters.erase("items_per_second");
  }
}

class BenchActor : public td::actor::core::Actor {
 public:
  Task<int> compute_task(int x) {
    co_return x * 2;
  }

  td::Result<int> compute_sync(int x) {
    return x * 3;
  }

  void compute_promise(int x, td::Promise<int> promise) {
    promise.set_value(x * 4);
  }
};

Task<int> simple_task() {
  co_return 42;
}

struct TestAwaitable {
  int value;
  bool is_ready{false};

  bool await_ready() noexcept {
    return is_ready;
  }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    return h;
  }
  td::Result<int> await_resume() noexcept {
    return value;
  }
};

static void BM_RawTaskAwait(benchmark::State& state) {
  coro_benchmark(state, [&](auto& state) -> Task<td::Unit> {
    co_await detach_from_actor();
    td::int64 sum = 0;
    while (state.KeepRunning()) {
      sum += (co_await SkipAwaitTransform{TestAwaitable{42}}).move_as_ok();
    }
    benchmark::DoNotOptimize(sum);
    co_return td::Unit();
  });
}
BENCHMARK(BM_RawTaskAwait)->UseRealTime()->MeasureProcessCPUTime();

static void BM_DelayedTaskAwait(benchmark::State& state) {
  coro_benchmark(state, [&](auto& state) -> Task<td::Unit> {
    co_await detach_from_actor();
    td::int64 sum = 0;
    while (state.KeepRunning()) {
      sum += (co_await SkipAwaitTransform{simple_task()}).move_as_ok();
    }
    benchmark::DoNotOptimize(sum);
    co_return td::Unit();
  });
}
BENCHMARK(BM_DelayedTaskAwait)->UseRealTime()->MeasureProcessCPUTime();

static void BM_StartedTaskAwait(benchmark::State& state) {
  coro_benchmark(state, [&](auto& state) -> Task<td::Unit> {
    co_await detach_from_actor();
    td::int64 sum = 0;
    while (state.KeepRunning()) {
      sum += (co_await SkipAwaitTransform{simple_task().start_immediate()}).move_as_ok();
    }
    benchmark::DoNotOptimize(sum);

    co_return td::Unit();
  });
}
BENCHMARK(BM_StartedTaskAwait)->UseRealTime()->MeasureProcessCPUTime();

static void BM_ScheduledTaskAwait(benchmark::State& state) {
  coro_benchmark(state, [&](auto& state) -> Task<td::Unit> {
    co_await detach_from_actor();
    td::int64 sum = 0;
    while (state.KeepRunning()) {
      sum += (co_await SkipAwaitTransform{simple_task().start()}).move_as_ok();
    }
    benchmark::DoNotOptimize(sum);

    co_return td::Unit();
  });
}
BENCHMARK(BM_ScheduledTaskAwait)->UseRealTime()->MeasureProcessCPUTime();

enum class ResumeMethod { Raw, Pass, Try };
enum class ResumeLocation { Actor, Scheduler, Any };
enum class AwaitableState { Ready, Suspended };

static void BM_AwaitThenResume(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const auto method = static_cast<ResumeMethod>(state.range(0));
    const auto location = static_cast<ResumeLocation>(state.range(1));
    const auto awaitable = static_cast<AwaitableState>(state.range(2));

    // Set a clean label for this benchmark
    const char* awaitable_name[] = {"Ready", "Suspended"};
    const char* method_names[] = {"Raw", "Pass", "Try"};
    const char* location_names[] = {"Actor", "Scheduler", "Any"};
    auto label = PSTRING() << method_names[static_cast<int>(method)] << "_"
                           << location_names[static_cast<int>(location)] << "_"
                           << awaitable_name[static_cast<int>(awaitable)];
    state.SetLabel(label);
    // Create an actor to run on if needed
    td::actor::ActorOwn<BenchActor> actor;
    if (location == ResumeLocation::Actor) {
      actor = td::actor::create_actor<BenchActor>("bench_actor");
    }

    TestAwaitable aw{42, awaitable == AwaitableState::Ready};
    Executor executor;
    if (location == ResumeLocation::Actor) {
      executor = Executor::on_actor(actor.get());
    } else if (location == ResumeLocation::Scheduler) {
      executor = Executor::on_scheduler();
    } else {
      executor = Executor::on_any();
    }
    co_await resume_on(executor);

    td::int64 sum = 0;
    td::int32 total_iterations = 0;

    while (state.KeepRunning()) {
      int value = 0;

      switch (method) {
        case ResumeMethod::Raw: {
          // Raw awaitable without any resume_on wrapper
          value = (co_await SkipAwaitTransform{aw}).move_as_ok();
          break;
        }
        case ResumeMethod::Pass: {
          value = (co_await SkipAwaitTransform{wrap_and_resume_on_current(aw)}).move_as_ok();
          break;
        }
        case ResumeMethod::Try: {
          value = (co_await SkipAwaitTransform{unwrap_and_resume_on_current(aw)});
          break;
        }
      }

      sum += value;
      total_iterations++;
    }

    CHECK(state.iterations() == total_iterations);

    benchmark::DoNotOptimize(sum);
    co_return td::Unit();
  });
}

// Register all combinations
static void ApplyAwaitThenResumeArgs(benchmark::internal::Benchmark* b) {
  for (auto location : {ResumeLocation::Actor, ResumeLocation::Scheduler, ResumeLocation::Any}) {
    for (auto awaitable : {AwaitableState::Ready, AwaitableState::Suspended}) {
      for (auto method : {ResumeMethod::Pass, ResumeMethod::Try, ResumeMethod::Raw}) {
        b->Args({static_cast<int>(method), static_cast<int>(location), static_cast<int>(awaitable)});
      }
    }
  }
}
BENCHMARK(BM_AwaitThenResume)->Apply(ApplyAwaitThenResumeArgs)->UseRealTime()->MeasureProcessCPUTime();

// Simple benchmarks
static void BM_TaskCreation(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto task = []() -> Task<int> { co_return 42; }();
      benchmark::DoNotOptimize(task);
      // Task is not started - just measuring creation overhead
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_TaskCreation)->UseRealTime()->MeasureProcessCPUTime();

static void BM_SimpleCompute(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto task = []() -> Task<int> {
        int sum = 0;
        for (int j = 0; j < 100; j++) {
          sum += j;
        }
        co_return sum;
      }();
      // Task will auto-start when awaited
      auto result = co_await std::move(task);
      benchmark::DoNotOptimize(result);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_SimpleCompute)->UseRealTime()->MeasureProcessCPUTime();

static void BM_TaskChain(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto task1 = []() -> Task<int> { co_return 10; }();
      auto task2 = [](Task<int> task1) -> Task<int> {
        auto v = co_await std::move(task1);
        co_return v * 2;
      }(std::move(task1));
      auto result = co_await std::move(task2);
      benchmark::DoNotOptimize(result);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_TaskChain)->UseRealTime()->MeasureProcessCPUTime();

static void BM_ErrorHandling(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto error_task = []() -> Task<int> { co_return td::Status::Error("test error"); }();
      auto result = co_await std::move(error_task).wrap();
      int value = result.is_error() ? 0 : result.ok();
      benchmark::DoNotOptimize(value);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_ErrorHandling)->UseRealTime()->MeasureProcessCPUTime();

static void BM_SpawnCoroutineOld(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto result = co_await spawn_actor("test", []() -> Task<int> { co_return 42; }());
      benchmark::DoNotOptimize(result);
    }

    co_return td::Unit();
  });
}
BENCHMARK(BM_SpawnCoroutineOld)->UseRealTime()->MeasureProcessCPUTime();

// Benchmark with multiple operations per iteration
static void BM_BatchTaskCreation(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    while (state.KeepRunning()) {
      auto task = []() -> Task<int> { co_return 42; }();
      benchmark::DoNotOptimize(task);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_BatchTaskCreation)->UseRealTime()->MeasureProcessCPUTime();

// Concurrent tasks
static void BM_ConcurrentTasks(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int num_tasks_int = static_cast<int>(state.range(0));
    while (state.KeepRunningBatch(num_tasks_int)) {
      std::vector<StartedTask<int>> tasks;
      tasks.reserve(num_tasks_int);
      for (int i = 0; i < num_tasks_int; ++i) {
        tasks.emplace_back([](int i) -> Task<int> { co_return i * 2; }(i).start());
      }
      int total = 0;
      for (auto& task : tasks) {
        auto result = co_await std::move(task);
        total += result;
      }
      benchmark::DoNotOptimize(total);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_ConcurrentTasks)->RangeMultiplier(2)->Range(1, 64)->UseRealTime()->MeasureProcessCPUTime();

// Memory pattern
static void BM_MemoryPattern(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int num_tasks = static_cast<int>(state.range(0));
    while (state.KeepRunningBatch(num_tasks)) {
      std::vector<Task<int>> tasks;
      tasks.reserve(num_tasks);
      for (int i = 0; i < num_tasks; ++i) {
        tasks.emplace_back([i]() -> Task<int> { co_return i; }());
      }
      int sum = 0;
      for (auto& task : tasks) {
        auto result = co_await std::move(task);
        sum += result;
      }
      benchmark::DoNotOptimize(sum);
    }
    co_return td::Unit();
  });
}
BENCHMARK(BM_MemoryPattern)->RangeMultiplier(4)->Range(1, 256)->UseRealTime()->MeasureProcessCPUTime();

// Unified benchmark for ask operations
enum class AskMethod { Task, TaskWrap, Promise, Sync, Call, TaskNew };
enum class AskMode { Scheduled, Immediate };

static void BM_Ask(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const auto method = static_cast<AskMethod>(state.range(0));
    const auto mode = static_cast<AskMode>(state.range(1));
    const int num_tasks = static_cast<int>(state.range(2));

    // Set a clean label for this benchmark
    const char* method_names[] = {"Task", "TaskTry", "Promise", "Sync", "Call", "New"};
    const char* mode_names[] = {"Scheduled", "Immediate"};
    auto label = PSTRING() << method_names[static_cast<int>(method)] << "_" << mode_names[static_cast<int>(mode)] << "_"
                           << num_tasks;
    state.SetLabel(label);
    td::int32 total_tasks = 0;
    std::vector<td::actor::ActorOwn<BenchActor>> actors;
    for (int i = 0; i < num_tasks; ++i) {
      actors.push_back(td::actor::create_actor<BenchActor>("bench_actor"));

      // just ensure that the following ask_immediate will be immediate
      while (true) {
        auto task = ask_immediate(actors.back(), &BenchActor::compute_task, 42);
        if (task.await_ready()) {
          break;
        }
      }
    }
    co_await Yield{};

    while (state.KeepRunningBatch(num_tasks)) {
      if (num_tasks == 1) {
        // Single task - direct execution
        int result = 0;

        if (mode == AskMode::Immediate) {
          switch (method) {
            case AskMethod::Task: {
              result = co_await ask_immediate(actors[0], &BenchActor::compute_task, 42);
              break;
            }
            case AskMethod::TaskWrap: {
              result = (co_await ask_immediate(actors[0], &BenchActor::compute_task, 42).wrap()).move_as_ok();
              break;
            }
            case AskMethod::TaskNew: {
              result = co_await ask_new_immediate(actors[0], &BenchActor::compute_task, 42);
              break;
            }
            case AskMethod::Promise: {
              result = co_await ask_immediate(actors[0], &BenchActor::compute_promise, 42);
              break;
            }
            case AskMethod::Sync: {
              result = co_await ask_immediate(actors[0], &BenchActor::compute_sync, 42);
              break;
            }
            case AskMethod::Call: {
              result = co_await actors[0].get_actor_unsafe().compute_task(42);
              break;
            }
          }
        } else {
          switch (method) {
            case AskMethod::Task: {
              result = co_await ask(actors[0], &BenchActor::compute_task, 42);
              break;
            }
            case AskMethod::TaskWrap: {
              result = (co_await ask(actors[0], &BenchActor::compute_task, 42).wrap()).ok();
              break;
            }
            case AskMethod::TaskNew: {
              result = co_await ask_new(actors[0], &BenchActor::compute_task, 42);
              break;
            }
            case AskMethod::Promise: {
              result = co_await ask(actors[0], &BenchActor::compute_promise, 42);
              break;
            }
            case AskMethod::Sync: {
              result = co_await ask(actors[0], &BenchActor::compute_sync, 42);
              break;
            }
            case AskMethod::Call: {
              UNREACHABLE();
            }
          }
        }

        total_tasks++;
        benchmark::DoNotOptimize(result);
      } else {
        // Multiple tasks - launch concurrently
        std::vector<StartedTask<int>> tasks;
        tasks.reserve(num_tasks);

        // Launch all tasks
        for (int i = 0; i < num_tasks; ++i) {
          if (mode == AskMode::Immediate) {
            switch (method) {
              case AskMethod::Task:
              case AskMethod::TaskWrap:
                tasks.emplace_back(ask_immediate(actors[i], &BenchActor::compute_task, 42 + i));
                break;
              case AskMethod::TaskNew:
                tasks.emplace_back(ask_new_immediate(actors[i], &BenchActor::compute_task, 42 + i));
                break;
              case AskMethod::Promise:
                tasks.emplace_back(ask_immediate(actors[i], &BenchActor::compute_promise, 42 + i));
                break;
              case AskMethod::Sync:
                tasks.emplace_back(ask_immediate(actors[i], &BenchActor::compute_sync, 42 + i));
                break;
              case AskMethod::Call: {
                UNREACHABLE();
              }
            }
            CHECK(tasks.back().await_ready());
          } else if (mode == AskMode::Scheduled) {
            switch (method) {
              case AskMethod::Task:
              case AskMethod::TaskWrap:
                tasks.emplace_back(ask(actors[i], &BenchActor::compute_task, 42 + i));
                break;
              case AskMethod::TaskNew:
                tasks.emplace_back(ask_new(actors[i], &BenchActor::compute_task, 42 + i));
                break;
              case AskMethod::Promise:
                tasks.emplace_back(ask(actors[i], &BenchActor::compute_promise, 42 + i));
                break;
              case AskMethod::Sync:
                tasks.emplace_back(ask(actors[i], &BenchActor::compute_sync, 42 + i));
                break;
              case AskMethod::Call: {
                UNREACHABLE();
              }
            }
          } else {
            UNREACHABLE();
          }
        }

        // Await all results
        int total = 0;
        for (auto& task : tasks) {
          if (method == AskMethod::TaskWrap) {
            total += (co_await std::move(task).wrap()).ok();
          } else {
            auto result = co_await std::move(task);
            total += result;
          }
          total_tasks++;
        }
        benchmark::DoNotOptimize(total);
      }
    }

    CHECK(state.iterations() == total_tasks);
    co_return td::Unit();
  });
}

// Register all combinations - the label is set inside the benchmark
static void ApplyBMAskArgs(benchmark::internal::Benchmark* b) {
  for (auto method : {AskMethod::TaskNew, AskMethod::TaskWrap, AskMethod::Task, AskMethod::Promise, AskMethod::Sync}) {
    for (auto mode : {AskMode::Scheduled, AskMode::Immediate}) {
      for (int n : {1, 10, 100}) {
        b->Args({static_cast<int>(method), static_cast<int>(mode), n});
      }
    }
  }
  // Call method is only meaningful for single-task immediate mode
  b->Args({static_cast<int>(AskMethod::Call), static_cast<int>(AskMode::Immediate), 1});
}
BENCHMARK(BM_Ask)->Apply(ApplyBMAskArgs)->UseRealTime()->MeasureProcessCPUTime()->Repetitions(10000);

// Benchmark send_closure with promise callback using Worker pattern
static void BM_SendClosureWorker(benchmark::State& state) {
  struct Worker : public td::actor::core::Actor {
   public:
    Worker(benchmark::State& state, td::Promise<int> promise, bool immediate, int num_tasks)
        : state_(state)
        , promise_(std::move(promise))
        , immediate_(immediate)
        , num_actors_(num_tasks)
        , tasks_completed_(0) {
      for (int i = 0; i < num_actors_; i++) {
        childs_.push_back(td::actor::create_actor<BenchActor>("bench_actor"));
      }
    }

   private:
    benchmark::State& state_;
    td::Promise<int> promise_;
    std::vector<td::actor::ActorOwn<BenchActor>> childs_;
    bool immediate_;
    int num_actors_;
    int tasks_completed_;
    int total_ = 0;

    void loop() override {
      if (state_.KeepRunningBatch(num_actors_)) {
        tasks_completed_ = 0;
        total_ = 0;
        for (int i = 0; i < num_actors_; ++i) {
          auto promise = td::promise_send_closure(actor_id(this), &Worker::done, i);
          if (immediate_) {
            send_closure_immediate(childs_[i], &BenchActor::compute_promise, 42 + i, std::move(promise));
          } else {
            send_closure(childs_[i], &BenchActor::compute_promise, 42 + i, std::move(promise));
          }
        }
      } else {
        promise_.set_value(7);
      }
    }

    void done(int task_id, td::Result<int> result) {
      if (result.is_ok()) {
        total_ += result.ok();
      }
      tasks_completed_++;
      benchmark::DoNotOptimize(total_);

      if (tasks_completed_ == num_actors_) {
        loop();
      }
    }
  };

  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const bool immediate = static_cast<bool>(state.range(0));
    const int num_tasks = static_cast<int>(state.range(1));

    // Set a clean label for this benchmark
    state.SetLabel(PSTRING() << "SendClosure_" << (immediate ? "Immediate" : "Scheduled") << "_" << num_tasks);

    auto [task, promise] = StartedTask<int>::make_bridge();
    auto td_promise =
        td::Promise<int>([p = std::move(promise)](td::Result<int> r) mutable { p.set_result(std::move(r)); });
    auto worker = td::actor::create_actor<Worker>("worker", state, std::move(td_promise), immediate, num_tasks);
    auto result = co_await std::move(task);
    benchmark::DoNotOptimize(result);

    co_return td::Unit();
  });
}

// Register send_closure benchmarks - the label is set inside the benchmark
static void ApplySendClosureArgs(benchmark::internal::Benchmark* b) {
  for (int immediate : {0, 1}) {
    for (int n : {1, 10, 100}) {
      b->Args({immediate, n});
    }
  }
}
BENCHMARK(BM_SendClosureWorker)->Apply(ApplySendClosureArgs)->UseRealTime()->MeasureProcessCPUTime();

// Real-world benchmark: HTTP-like request handler with database lookups
// Simulates: Request -> Auth Check -> DB Query -> Response
static void BM_HttpRequestHandler(benchmark::State& state) {
  // Simulated database service
  struct DatabaseService : public td::actor::core::Actor {
    Task<std::string> query_user(int user_id) {
      // Simulate DB latency with a small computation
      int sum = 0;
      for (int i = 0; i < 100; ++i) {
        sum += user_id * i;
      }
      co_return PSTRING() << "user_" << user_id << "_data_" << sum;
    }

    Task<bool> check_auth(int user_id) {
      // Simulate auth check
      co_return (user_id % 2) == 0;  // Even IDs are authorized
    }
  };

  // Request handler service
  struct RequestHandler {
    static Task<std::string> handle_request(ActorId<DatabaseService> db, int request_id) {
      int user_id = request_id % 1000;

      // Step 1: Check authorization
      auto authorized = co_await ask_immediate(db, &DatabaseService::check_auth, user_id);
      if (!authorized) {
        co_return "401 Unauthorized";
      }

      // Step 2: Query database
      auto user_data = co_await ask_immediate(db, &DatabaseService::query_user, user_id);

      // Step 3: Process and return response
      co_return PSTRING() << "200 OK: " << user_data;
    }
  };

  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int concurrent_requests = static_cast<int>(state.range(0));
    state.SetLabel(PSTRING() << "HttpHandler_" << concurrent_requests << "_requests");

    auto db = td::actor::create_actor<DatabaseService>("database");

    int request_counter = 0;

    while (state.KeepRunningBatch(concurrent_requests)) {
      std::vector<StartedTask<std::string>> requests;
      requests.reserve(concurrent_requests);

      // Launch concurrent requests
      for (int i = 0; i < concurrent_requests; ++i) {
        // TODO or lazy coroutine?
        auto task = RequestHandler::handle_request(db.get(), request_counter).start();
        requests.emplace_back(std::move(task));
      }

      // Await all responses
      for (auto& request : requests) {
        auto response = co_await std::move(request);
        benchmark::DoNotOptimize(response);
      }
    }

    co_return td::Unit();
  });
}

// Register HTTP handler benchmark with various concurrency levels
BENCHMARK(BM_HttpRequestHandler)->RangeMultiplier(10)->Range(1, 1000)->UseRealTime()->MeasureProcessCPUTime();

static void BM_HttpRequestHandlerOld(benchmark::State& state) {
  // Simulated database service
  struct DatabaseService : public td::actor::core::Actor {
    td::Result<std::string> query_user(int user_id) {
      // Simulate DB latency with a small computation
      int sum = 0;
      for (int i = 0; i < 100; ++i) {
        sum += user_id * i;
      }
      return PSTRING() << "user_" << user_id << "_data_" << sum;
    }

    td::Result<bool> check_auth(int user_id) {
      // Simulate auth check
      return (user_id % 2) == 0;  // Even IDs are authorized
    }
  };

  // Request handler service
  struct RequestHandlerOld : public td::actor::core::Actor {
    RequestHandlerOld(td::actor::ActorId<DatabaseService> db, td::Promise<std::string> promise, td::int32 request_id)
        : db_(db), promise_(std::move(promise)), user_id_(request_id % 1000) {
    }

    void start_up() override {
      // Step 1: Check authorization
      send_closure_immediate(db_, &DatabaseService::check_auth, user_id_,
                             td::promise_send_closure(actor_id(this), &RequestHandlerOld::on_authorized));
    }

    void on_authorized(td::Result<bool> r_authorized) {
      if (r_authorized.is_error()) {
        promise_.set_error(r_authorized.move_as_error());
        return stop();
      }
      auto authorized = r_authorized.ok();
      if (!authorized) {
        promise_.set_value("401 anauthorized");
        return stop();
      }

      send_closure_immediate(db_, &DatabaseService::query_user, user_id_,
                             td::promise_send_closure(actor_id(this), &RequestHandlerOld::on_user));
    }

    void on_user(td::Result<std::string> r_user_data) {
      if (r_user_data.is_error()) {
        promise_.set_error(r_user_data.move_as_error());
        return stop();
      }
      auto user_data = r_user_data.ok();
      promise_.set_value(PSTRING() << "200 OK: " << user_data);
      return stop();
    }

   private:
    td::actor::ActorId<DatabaseService> db_;
    td::Promise<std::string> promise_;
    td::int32 user_id_{};
  };

  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int concurrent_requests = static_cast<int>(state.range(0));
    state.SetLabel(PSTRING() << "HttpHandlerOld_" << concurrent_requests << "_requests");

    auto db = td::actor::create_actor<DatabaseService>("database");

    int request_counter = 0;

    while (state.KeepRunningBatch(concurrent_requests)) {
      std::vector<StartedTask<std::string>> requests;
      requests.reserve(concurrent_requests);

      // Launch concurrent requests
      for (int i = 0; i < concurrent_requests; ++i) {
        auto [task, promise] = StartedTask<std::string>::make_bridge();
        td::actor::create_actor<RequestHandlerOld>("handler", db.get(), std::move(promise), ++request_counter)
            .release();
        requests.emplace_back(std::move(task));
      }

      // Await all responses
      for (auto& request : requests) {
        auto response = co_await std::move(request);
        benchmark::DoNotOptimize(response);
      }
    }

    co_return td::Unit();
  });
}

// Register HTTP handler benchmark with various concurrency levels
BENCHMARK(BM_HttpRequestHandlerOld)->RangeMultiplier(10)->Range(1, 1000)->UseRealTime()->MeasureProcessCPUTime();

// Concurrent Pub-Sub benchmark: Publishers -> Broker -> Subscribers
// Multiple Publisher actors produce messages concurrently, Broker fans out to all Subscribers
static void BM_PubSubConcurrent(benchmark::State& state) {
  struct Message {
    std::string payload;
  };

  struct Subscriber : public td::actor::core::Actor {
    explicit Subscriber(int id) : id_(id) {
    }

    void process(Message m) {
      int sum = 0;
      for (char c : m.payload) {
        sum += static_cast<int>(c);
      }
      total_++;
      benchmark::DoNotOptimize(sum);
    }

    td::Result<td::int64> get_delivered_count() {
      return total_;
    }

   private:
    int id_;
    td::int64 total_{0};
  };

  struct Broker : public td::actor::core::Actor {
    void subscribe(td::actor::ActorId<Subscriber> sub) {
      subscribers_.push_back(sub);
    }

    Task<int> publish(Message m) {
      for (auto& sub : subscribers_) {
        send_closure(sub, &Subscriber::process, m);
      }
      co_return static_cast<int>(subscribers_.size());
    }

   private:
    std::vector<td::actor::ActorId<Subscriber>> subscribers_;
  };

  struct Publisher : public td::actor::core::Actor {
    Publisher(td::actor::ActorId<Broker> broker, int id) : broker_(broker), id_(id) {
    }

    Task<int> produce(int count) {
      int delivered_total = 0;
      for (int j = 0; j < count; ++j) {
        Message m{PSTRING() << "msg_" << id_ << "_" << j};
        auto delivered = co_await ask(broker_, &Broker::publish, std::move(m));
        delivered_total += delivered;
      }
      co_return delivered_total;
    }

   private:
    td::actor::ActorId<Broker> broker_;
    int id_;
  };

  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int num_publishers = static_cast<int>(state.range(0));
    const int num_subscribers = static_cast<int>(state.range(1));
    const int num_brokers = static_cast<int>(state.range(2));
    constexpr int messages_per_publisher = 10;
    state.SetLabel(PSTRING() << "PubSubConcurrent_P" << num_publishers << "_S" << num_subscribers << "_B"
                             << num_brokers);

    // Create brokers (shards)
    std::vector<td::actor::ActorOwn<Broker>> brokers;
    brokers.reserve(num_brokers);
    for (int b = 0; b < num_brokers; ++b) {
      brokers.push_back(td::actor::create_actor<Broker>("broker"));
    }

    // Create subscribers and subscribe them round-robin across brokers
    std::vector<td::actor::ActorOwn<Subscriber>> subscribers;
    subscribers.reserve(num_subscribers);
    for (int i = 0; i < num_subscribers; ++i) {
      auto sub = td::actor::create_actor<Subscriber>("subscriber", i);
      for (auto& broker : brokers) {
        send_closure(broker, &Broker::subscribe, sub.get());
      }
      subscribers.push_back(std::move(sub));
    }

    // Create publishers and assign each to a broker (round-robin)
    std::vector<td::actor::ActorOwn<Publisher>> publishers;
    publishers.reserve(num_publishers);
    for (int p = 0; p < num_publishers; ++p) {
      auto& broker = brokers[p % num_brokers];
      publishers.push_back(td::actor::create_actor<Publisher>("publisher", broker.get(), p));
    }

    const td::int64 total_messages = num_publishers * messages_per_publisher * num_subscribers;
    td::int64 iteration_count = 0;
    while (state.KeepRunningBatch(total_messages)) {
      iteration_count++;
      std::vector<StartedTask<int>> tasks;
      tasks.reserve(num_publishers);
      for (auto& pub : publishers) {
        tasks.emplace_back(ask(pub, &Publisher::produce, messages_per_publisher));
      }

      int delivered_sum = 0;
      for (auto& task : tasks) {
        delivered_sum += co_await std::move(task);
      }
      benchmark::DoNotOptimize(delivered_sum);
    }

    td::int64 total_delivered = 0;
    const td::int64 expected_per_subscriber = iteration_count * num_publishers * messages_per_publisher;
    for (auto& subscriber : subscribers) {
      while (true) {
        auto delivered_count = co_await ask(subscriber, &Subscriber::get_delivered_count);
        if (delivered_count != expected_per_subscriber) {
          LOG(ERROR) << "Subscriber delivered " << delivered_count << " != expected " << expected_per_subscriber;
          continue;
        }
        total_delivered += delivered_count;
        break;
      }
    }
    CHECK(state.iterations() == total_delivered);

    co_return td::Unit();
  });
}

static void ApplyPubSubArgs(benchmark::internal::Benchmark* b) {
  constexpr int combos[][3] = {{1, 10, 1}, {10, 10, 1}, {10, 100, 1}, {100, 100, 1}, {10, 100, 4}, {100, 100, 4}};
  for (const auto& combo : combos) {
    b->Args({combo[0], combo[1], combo[2]});
  }
}
BENCHMARK(BM_PubSubConcurrent)->Apply(ApplyPubSubArgs)->UseRealTime()->MeasureProcessCPUTime();

static void BM_ConcurrentAsks(benchmark::State& state) {
  coro_benchmark(state, [](auto& state) -> Task<td::Unit> {
    const int num_actors = static_cast<int>(state.range(0));
    std::vector<td::actor::ActorOwn<BenchActor>> actors;
    actors.reserve(num_actors);
    for (int i = 0; i < num_actors; ++i) {
      actors.push_back(td::actor::create_actor<BenchActor>("bench_actor"));
    }

    while (state.KeepRunningBatch(num_actors)) {
      std::vector<StartedTask<int>> tasks;
      tasks.reserve(num_actors);

      for (auto& actor : actors) {
        tasks.emplace_back(ask(actor, &BenchActor::compute_task, 42));
      }

      for (auto& task : tasks) {
        auto result = co_await std::move(task);
        benchmark::DoNotOptimize(result);
      }
    }

    co_return td::Unit();
  });
}
BENCHMARK(BM_ConcurrentAsks)->RangeMultiplier(4)->Range(1, 64)->UseRealTime()->MeasureProcessCPUTime();

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::SetBenchmarkFilter("BM_Ask/5/1/10/");
  //benchmark::SetBenchmarkFilter("BM_Ask");
  benchmark::RunSpecifiedBenchmarks();
  return 0;
}