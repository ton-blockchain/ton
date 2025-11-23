#include <chrono>
#include <thread>
#include <vector>

#include "td/utils/tests.h"
#include "tonlib/FFIEventLoop.h"

namespace tonlib {
namespace {

#define EXPECT_APPROXIMATE_TIME(elapsed_ms, expected_ms, tolerance_ms) \
  EXPECT((elapsed_ms) >= (expected_ms) - (tolerance_ms) && (elapsed_ms) <= (expected_ms) + (tolerance_ms))

struct Tag {};

static constexpr Tag tags[3];
static constexpr const void* continuation_0 = &tags[0];
static constexpr const void* continuation_1 = &tags[1];
static constexpr const void* continuation_2 = &tags[2];

auto measure_time(auto&& func) {
  auto start = std::chrono::steady_clock::now();
  func();
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

Continuation wait_for_continuation(FFIEventLoop& loop) {
  std::optional<Continuation> result;
  while (!result.has_value()) {
    result = loop.wait(-1);
  }
  return *result;
}

TEST(FFIEventLoop, WaitTimeout) {
  FFIEventLoop loop(1);
  auto elapsed = measure_time([&] {
    auto result = loop.wait(0.02);
    EXPECT(!result.has_value());
  });
  EXPECT_APPROXIMATE_TIME(elapsed, 20, 15);
}

TEST(FFIEventLoop, PutBeforeWait) {
  FFIEventLoop loop(1);
  loop.put({continuation_0});
  auto result = loop.wait(0);
  EXPECT(result.has_value());
  EXPECT_EQ(result->ptr(), continuation_0);
}

TEST(FFIEventLoop, WaitThenPut) {
  FFIEventLoop loop(1);

  std::thread producer([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.put({continuation_1});
  });

  auto elapsed = measure_time([&] {
    auto result = wait_for_continuation(loop);
    EXPECT_EQ(result.ptr(), continuation_1);
  });

  producer.join();
  EXPECT_APPROXIMATE_TIME(elapsed, 20, 15);
}

TEST(FFIEventLoop, CancelBeforeWait) {
  FFIEventLoop loop(1);
  loop.cancel();

  auto elapsed = measure_time([&] {
    auto result = loop.wait(1.0);
    EXPECT(!result.has_value());
  });

  EXPECT(elapsed < 10);

  auto result = loop.wait(1.0);
  EXPECT(!result.has_value());
}

TEST(FFIEventLoop, CancelDuringWait) {
  FFIEventLoop loop(1);

  std::thread canceller([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.cancel();
  });

  auto elapsed = measure_time([&] {
    auto result = loop.wait(2.0);
    EXPECT(!result.has_value());
  });

  canceller.join();
  EXPECT_APPROXIMATE_TIME(elapsed, 20, 15);
}

TEST(FFIEventLoop, MultiplePuts) {
  FFIEventLoop loop(1);

  loop.put({continuation_0});
  loop.put({continuation_1});
  loop.put({continuation_2});

  auto result1 = loop.wait(0);
  auto result2 = loop.wait(0);
  auto result3 = loop.wait(0);
  auto result4 = loop.wait(0);

  EXPECT(result1.has_value() && result1->ptr() == continuation_0);
  EXPECT(result2.has_value() && result2->ptr() == continuation_1);
  EXPECT(result3.has_value() && result3->ptr() == continuation_2);
  EXPECT(!result4.has_value());
}

TEST(FFIEventLoop, ActorCounterBlocksDestructor) {
  bool guard_destroyed = false;

  std::thread actor_thread;

  auto elapsed = measure_time([&] {
    FFIEventLoop loop(1);

    auto guard = loop.new_actor();

    actor_thread = std::thread([guard = std::move(guard), &guard_destroyed]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      guard_destroyed = true;
      // The reset is supposed to happen before FFIEventLoop destructor exits, so TSAN won't not say
      // that `EXPECT(guard_destroyed)` later is a data race.
      guard.reset();
    });
  });

  EXPECT(guard_destroyed);
  EXPECT_APPROXIMATE_TIME(elapsed, 20, 15);

  actor_thread.join();
}

TEST(FFIEventLoop, MultipleActors) {
  bool all_destroyed = false;

  std::thread destroyer;

  auto elapsed = measure_time([&] {
    FFIEventLoop loop(1);

    std::vector<td::unique_ptr<td::Guard>> guards;
    guards.push_back(loop.new_actor());
    guards.push_back(loop.new_actor());
    guards.push_back(loop.new_actor());

    destroyer = std::thread([guards = std::move(guards), &all_destroyed]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      guards[0].reset();
      guards[1].reset();
      all_destroyed = true;
      guards[2].reset();
    });
  });

  EXPECT(all_destroyed);
  EXPECT_APPROXIMATE_TIME(elapsed, 10, 15);

  destroyer.join();
}

TEST(FFIEventLoop, RunInContext) {
  bool executed{false};

  {
    FFIEventLoop loop(1);

    loop.run_in_context([&]() {
      executed = true;
      EXPECT(td::actor::SchedulerContext::get() != nullptr);
    });
  }

  EXPECT(executed);
}

TEST(FFIEventLoop, ConcurrentPuts) {
  FFIEventLoop loop(1);

  const int num_threads = 5;
  const int puts_per_thread = 10;

  Tag continuations[num_threads * puts_per_thread];

  std::vector<std::thread> producers;
  for (int i = 0; i < num_threads; ++i) {
    producers.emplace_back([&, i]() {
      for (int j = 0; j < puts_per_thread; ++j) {
        loop.put({&continuations[i * puts_per_thread + j]});
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }

  std::set<const void*> received;
  for (int i = 0; i < num_threads * puts_per_thread; ++i) {
    auto result = wait_for_continuation(loop);
    received.insert(result.ptr());
  }

  EXPECT_EQ(received.size(), static_cast<size_t>(num_threads * puts_per_thread));
  for (int i = 0; i < num_threads * puts_per_thread; ++i) {
    EXPECT(received.contains(&continuations[i]));
  }

  auto result = loop.wait(0.01);
  EXPECT(!result.has_value());
}

TEST(FFIEventLoop, BackgroundThreadFlow) {
  FFIEventLoop loop(1);

  std::atomic<bool> background_running{true};
  std::vector<const void*> received;

  std::thread background([&]() {
    while (background_running) {
      auto result = loop.wait(0.01);
      if (result.has_value()) {
        received.push_back(result->ptr());
      }
    }
  });

  loop.put({continuation_0});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  loop.put({continuation_1});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  loop.put({continuation_2});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  background_running = false;
  loop.cancel();
  background.join();

  EXPECT_EQ(received.size(), 3u);
  EXPECT_EQ(received[0], continuation_0);
  EXPECT_EQ(received[1], continuation_1);
  EXPECT_EQ(received[2], continuation_2);
}

TEST(FFIEventLoop, PutFromSchedulerContext) {
  FFIEventLoop loop(1);

  loop.run_in_context([&]() { loop.put({continuation_0}); });

  auto result = wait_for_continuation(loop);
  EXPECT_EQ(result.ptr(), continuation_0);
}

TEST(FFIEventLoop, InterleavedPutsAndWaits) {
  FFIEventLoop loop(1);

  loop.put({continuation_0});
  auto result1 = loop.wait(0.01);
  EXPECT(result1.has_value() && result1->ptr() == continuation_0);

  loop.put({continuation_1});
  auto result2 = loop.wait(0.01);
  EXPECT(result2.has_value() && result2->ptr() == continuation_1);

  loop.put({continuation_2});
  auto result3 = loop.wait(0.01);
  EXPECT(result3.has_value() && result3->ptr() == continuation_2);

  auto result4 = loop.wait(0.01);
  EXPECT(!result4.has_value());
}

TEST(FFIEventLoop, CancelMultipleTimes) {
  FFIEventLoop loop(1);

  loop.cancel();
  loop.cancel();
  loop.cancel();

  auto result = loop.wait(0.01);
  EXPECT(!result.has_value());
}

TEST(FFIEventLoop, PutAfterCancel) {
  FFIEventLoop loop(1);

  loop.cancel();
  loop.put({continuation_0});

  auto result = loop.wait(0.01);
  EXPECT(!result.has_value());
}

}  // namespace
}  // namespace tonlib
