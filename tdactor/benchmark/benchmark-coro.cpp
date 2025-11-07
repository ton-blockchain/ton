#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro.h"
#include "td/utils/benchmark.h"

using namespace td::actor;

class BenchmarkDatabase final : public td::actor::Actor {
 public:
  Task<size_t> square(size_t x) {
    co_return x* x;
  }
};

class CoroBenchmark final : public td::actor::Actor {
 public:
  void start_up() override {
    db_ = td::actor::create_actor<BenchmarkDatabase>("BenchmarkDatabase").release();
    flow().start_immediate().detach();
  }

  Task<td::Unit> run_benchmarks() {
    LOG(INFO) << "=== Running benchmarks ===";
    std::vector<size_t> thread_counts = {1, 10};
    size_t total_ops = 100000;

    for (size_t thread_count : thread_counts) {
      size_t ops_per_thread = std::max<size_t>(1, total_ops / thread_count);
      std::vector<StartedTask<td::Unit>> tasks;

      auto run_benchmark = [&](const char* name, bool immediate) -> Task<td::Unit> {
        td::Timer timer;

        for (size_t t = 0; t < thread_count; t++) {
          auto task_name = PSTRING() << name << "_" << t;
          tasks.push_back(
              spawn_actor(std::move(task_name), [](auto db, auto ops_per_thread, auto immediate) -> Task<td::Unit> {
                for (size_t i = 0; i < ops_per_thread; i++) {
                  if (immediate) {
                    (void)co_await ask_immediate(db, &BenchmarkDatabase::square, i);
                  } else {
                    (void)co_await ask(db, &BenchmarkDatabase::square, i);
                  }
                }
                co_return td::Unit();
              }(db_, ops_per_thread, immediate)));
        }

        for (auto& task : tasks) {
          co_await std::move(task);
        }
        tasks.clear();

        auto elapsed = timer.elapsed();
        auto ops_per_sec = static_cast<double>(total_ops) / elapsed;
        LOG(INFO) << name << " " << ops_per_thread << " ops: " << elapsed << "s (threads=" << thread_count << ", "
                  << static_cast<size_t>(ops_per_sec) << " ops/sec)";
        co_return td::Unit();
      };

      co_await run_benchmark("Immediate", true);
      co_await run_benchmark("Delayed", false);
    }

    LOG(INFO) << "=== Optimized benchmarks (direct) ===";
    constexpr size_t single_thread = 1;
    size_t ops_count = total_ops / single_thread;

    // Warm up
    for (size_t i = 0; i < 1000; i++) {
      (void)co_await ask(db_, &BenchmarkDatabase::square, i);
    }

    td::Timer timer;
    for (size_t i = 0; i < ops_count; i++) {
      (void)co_await ask(db_, &BenchmarkDatabase::square, i);
    }
    auto elapsed = timer.elapsed();
    auto ops_per_sec = static_cast<double>(total_ops) / elapsed;
    LOG(INFO) << "Direct delayed " << ops_count << " ops: " << elapsed << "s (" << static_cast<size_t>(ops_per_sec)
              << " ops/sec)";

    timer = {};
    for (size_t i = 0; i < ops_count; i++) {
      (void)co_await ask_immediate(db_, &BenchmarkDatabase::square, i);
    }
    elapsed = timer.elapsed();
    ops_per_sec = static_cast<double>(total_ops) / elapsed;
    LOG(INFO) << "Direct immediate " << ops_count << " ops: " << elapsed << "s (" << static_cast<size_t>(ops_per_sec)
              << " ops/sec)";

    timer = {};
    for (size_t i = 0; i < ops_count; i++) {
      auto local_square = [](size_t x) -> Task<size_t> { co_return x* x; };
      (void)co_await local_square(i);
    }
    elapsed = timer.elapsed();
    ops_per_sec = static_cast<double>(total_ops) / elapsed;
    LOG(INFO) << "Local coroutine " << ops_count << " ops: " << elapsed << "s (" << static_cast<size_t>(ops_per_sec)
              << " ops/sec)";

    co_return td::Unit();
  }

  Task<td::Unit> flow() {
    LOG(INFO) << "Starting benchmarks";
    (void)co_await run_benchmarks();
    LOG(INFO) << "Benchmarks completed";
    td::actor::SchedulerContext::get()->stop();
    stop();
    co_return td::Unit();
  }

 private:
  td::actor::ActorId<BenchmarkDatabase> db_;
};

int main() {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(INFO));
  td::actor::Scheduler scheduler({4});

  scheduler.run_in_context([&] { td::actor::create_actor<CoroBenchmark>("CoroBenchmark").release(); });

  scheduler.run();
  return 0;
}
