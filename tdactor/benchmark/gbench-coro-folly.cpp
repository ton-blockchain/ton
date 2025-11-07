#include <algorithm>
#include <benchmark/benchmark.h>
#include <folly/Executor.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Task.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using folly::coro::Task;
struct DatabaseService {
  folly::Executor::KeepAlive<> ex;

  Task<std::string> query_user(int user_id) {
    // Same "latency": cheap CPU work
    int sum = 0;
    for (int i = 0; i < 100; ++i) {
      sum += user_id * i;
    }
    co_return "user_" + std::to_string(user_id) + "_data_" + std::to_string(sum);
  }

  Task<bool> check_auth(int user_id) {
    co_return (user_id % 2) == 0;  // Even IDs are authorized
  }
};

struct RequestHandler {
  DatabaseService& db;
  folly::Executor::KeepAlive<> ex;

  Task<std::string> handle_request(int request_id) {
    const int user_id = request_id % 1000;

    const bool authorized = co_await co_withExecutor(db.ex, db.check_auth(user_id));
    if (!authorized) {
      co_return "401 Unauthorized";
    }

    std::string user_data = co_await co_withExecutor(db.ex, db.query_user(user_id));
    co_return "200 OK: " + user_data;
  }
};

}  // namespace

// Real-world benchmark: HTTP-like request handler with database lookups
// Simulates: Request -> Auth Check -> DB Query -> Response
static void BM_HttpRequestHandler_Folly(benchmark::State& state) {
  const int concurrent_requests = static_cast<int>(state.range(0));
  state.SetLabel("HttpHandler_Folly_" + std::to_string(concurrent_requests) + "_requests");

  // Executor sized to available HW, capped by requested concurrency (avoid oversubscribe)
  const int hw = std::max(1u, std::thread::hardware_concurrency());
  const int threads = 4;
  folly::CPUThreadPoolExecutor exec(threads);
  auto ex = folly::getKeepAliveToken(&exec);

  DatabaseService db{folly::SerialExecutor::create(ex)};
  RequestHandler handler{db, ex};

  int request_counter = 0;

  while (state.KeepRunningBatch(concurrent_requests)) {
    std::vector<folly::coro::TaskWithExecutor<std::string>> requests;
    requests.reserve(concurrent_requests);

    // Launch concurrent requests on the executor
    for (int i = 0; i < concurrent_requests; ++i) {
      requests.emplace_back(co_withExecutor(ex, handler.handle_request(request_counter++)));
    }

    // Await all responses concurrently
    auto responses = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(requests)));

    for (auto& r : responses) {
      benchmark::DoNotOptimize(r);
    }
  }
}

// Register HTTP handler benchmark with various concurrency levels
BENCHMARK(BM_HttpRequestHandler_Folly)->Arg(1)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Folly)->Arg(10)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Folly)->Arg(100)->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_HttpRequestHandler_Folly)->Arg(1000)->UseRealTime()->MeasureProcessCPUTime();

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  benchmark::SetBenchmarkFilter("Http");
  benchmark::RunSpecifiedBenchmarks();
  return 0;
}
