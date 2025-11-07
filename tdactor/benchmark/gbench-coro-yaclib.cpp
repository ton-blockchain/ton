#include <array>
#include <benchmark/benchmark.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <yaclib/async/connect.hpp>
#include <yaclib/async/contract.hpp>
#include <yaclib/async/make.hpp>
#include <yaclib/async/run.hpp>
#include <yaclib/async/shared_contract.hpp>
#include <yaclib/async/when_all.hpp>
#include <yaclib/async/when_any.hpp>
#include <yaclib/coro/await.hpp>
#include <yaclib/coro/await_on.hpp>
#include <yaclib/coro/future.hpp>
#include <yaclib/coro/on.hpp>
#include <yaclib/coro/task.hpp>
#include <yaclib/exe/manual.hpp>
#include <yaclib/exe/strand.hpp>
#include <yaclib/lazy/make.hpp>
#include <yaclib/runtime/fair_thread_pool.hpp>
#include <yaclib_std/thread>

struct DatabaseService {
  DatabaseService(yaclib::IExecutorPtr executor) : strand_(executor) {
  }
  yaclib::Strand strand_;

  yaclib::Task<bool> check_auth(int user_id) {
    co_await On(strand_);
    // Simulate auth check
    co_return (user_id % 2) == 0;  // Even IDs are authorized
  }
};

static void BM_HttpRequestHandler_Yaclib(benchmark::State& state) {
  const int concurrent_requests = static_cast<int>(state.range(0));
  state.SetLabel("HttpHandler_" + std::to_string(concurrent_requests) + "_requests");

  // Thread pool backing the "DB"; strand enforces single-coro entry to DB section
  auto db_pool = yaclib::MakeFairThreadPool(10);
  auto db_strand = yaclib::MakeStrand(db_pool);

  auto query_user = [&](int user_id) -> yaclib::Task<std::string> {
    co_await On(*db_strand);
    // Simulate DB latency with a small computation
    int sum = 0;
    for (int i = 0; i < 100; ++i) {
      sum += user_id * i;
    }
    std::stringstream ss;
    ss << "user_" << user_id << "_data_" << sum;
    co_return ss.str();
  };
  auto check_auth = [&](int user_id) -> yaclib::Task<bool> {
    co_await On(*db_strand);
    // Simulate auth check
    co_return (user_id % 2) == 0;  // Even IDs are authorized
  };

  std::vector<yaclib::IExecutorPtr> executors;
  for (size_t i = 0; i < concurrent_requests; ++i) {
    executors.emplace_back(yaclib::MakeStrand(db_pool));
  }
  // "RequestHandler"
  auto handle_request = [&](int request_id) -> yaclib::Task<std::string> {
    co_await On(*executors[request_id % executors.size()]);
    const int user_id = request_id % 1000;

    const bool authorized = co_await check_auth(user_id);
    if (!authorized)
      co_return std::string{"401 Unauthorized"};

    const std::string user_data = co_await query_user(user_id);
    co_return std::string{"200 OK: "} + user_data;
  };

  auto run_test = [&]() -> yaclib::Task<int> {
    int request_counter = 0;
    while (state.KeepRunningBatch(concurrent_requests)) {
      std::vector<yaclib::Future<std::string>> batch;
      batch.reserve(concurrent_requests);

      for (int i = 0; i < concurrent_requests; ++i) {
        // Convert lazy Task -> started Future
        batch.emplace_back(handle_request(request_counter++).ToFuture());
      }

      // Wait for all; keeps concurrency
      auto all = co_await yaclib::WhenAll(batch.begin(), batch.end());
      benchmark::DoNotOptimize(all);
    }
    co_return 0;
  };

  benchmark::DoNotOptimize(run_test().Get());

  db_pool->Stop();
  db_pool->Wait();
}

// Register with various concurrency levels
BENCHMARK(BM_HttpRequestHandler_Yaclib)->Arg(1)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Yaclib)->Arg(10)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Yaclib)->Arg(100)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Yaclib)->Arg(1000)->UseRealTime()->MeasureProcessCPUTime();

static void BM_AskYaclib(benchmark::State& state) {
  auto pool = yaclib::MakeFairThreadPool(10);
  const int num_tasks = static_cast<int>(state.range(0));
  std::vector<yaclib::IExecutorPtr> executors;
  for (int i = 0; i < num_tasks; ++i) {
    executors.emplace_back(yaclib::MakeStrand(pool));
  }

  auto get = [&](size_t i) -> yaclib::Task<int> {
    co_await On(*executors[i % executors.size()]);
    co_return 42;
  };

  // Set a clean label for this benchmark
  auto run_test = [&]() -> yaclib::Task<int> {
    int request_counter = 0;
    while (state.KeepRunningBatch(num_tasks)) {
      if (num_tasks == 1) {
        int result = co_await get(0);
        benchmark::DoNotOptimize(result);
      } else {
        std::vector<yaclib::Task<int>> tasks;
        tasks.reserve(num_tasks);
        for (int i = 0; i < num_tasks; ++i) {
          auto task = get(i);
          tasks.push_back(std::move(task));
        }
        for (auto& task : tasks) {
          co_await std::move(task);
        }
      }
    }
    co_return 0;
  };

  benchmark::DoNotOptimize(run_test().Get());

  pool->Stop();
  pool->Wait();
}

// Register all combinations - the label is set inside the benchmark
// Single task
BENCHMARK(BM_AskYaclib)->Args({1})->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_AskYaclib)->Args({10})->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_AskYaclib)->Args({100})->UseRealTime()->MeasureProcessCPUTime();

yaclib::Task<int> simple_task() {
  co_return 42;
}
static void BM_TaskAwait(benchmark::State& state) {
  auto db_pool = yaclib::MakeFairThreadPool(1);
  auto run_test = [&]() -> yaclib::Task<int> {
    int sum = 0;
    while (state.KeepRunning()) {
      sum += co_await simple_task();
    }
    benchmark::DoNotOptimize(sum);
    co_return 0;
  };

  benchmark::DoNotOptimize(run_test().Get());

  db_pool->Stop();
  db_pool->Wait();
}

// Register with various concurrency levels
BENCHMARK(BM_TaskAwait)->UseRealTime()->MeasureProcessCPUTime();

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  return 0;
}
