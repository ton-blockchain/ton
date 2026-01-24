#include <chrono>
#include <thread>

#include "td/utils/Status.h"
#include "td/utils/tests.h"
#include "tonlib/FFIAwaitable.h"

namespace tonlib {
namespace {

struct Tag {};

static constexpr Tag tags[2];
static constexpr const void* continuation_0 = &tags[0];
static constexpr const void* continuation_1 = &tags[1];

Continuation wait_for_continuation(FFIEventLoop& loop) {
  std::optional<Continuation> result;
  while (!result.has_value()) {
    result = loop.wait(-1);
  }
  return *result;
}

TEST(FFIAwaitable, CreateResolvedWithValue) {
  FFIEventLoop loop(1);
  auto awaitable = FFIAwaitable<int>::create_resolved(loop, 42);

  EXPECT(awaitable->await_ready());
  EXPECT(awaitable->result().is_ok());
  EXPECT_EQ(awaitable->result().ok(), 42);
}

TEST(FFIAwaitable, CreateResolvedWithError) {
  FFIEventLoop loop(1);
  auto awaitable = FFIAwaitable<int>::create_resolved(loop, td::Status::Error(123, "test error"));

  EXPECT(awaitable->await_ready());
  EXPECT(awaitable->result().is_error());
  EXPECT_EQ(awaitable->result().error().code(), 123);
}

TEST(FFIAwaitable, AwaitSuspendOnResolved) {
  FFIEventLoop loop(1);
  auto awaitable = FFIAwaitable<int>::create_resolved(loop, 42);

  awaitable->await_suspend({continuation_0});

  auto result = loop.wait(0);
  EXPECT(result.has_value());
  EXPECT_EQ(result->ptr(), continuation_0);
}

TEST(FFIAwaitable, CreateBridgeResolveWithoutSuspend) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x * 2; });

  EXPECT(!bridge.awaitable->await_ready());

  bridge.promise.set_value(21);

  EXPECT(bridge.awaitable->await_ready());
  EXPECT(bridge.awaitable->result().is_ok());
  EXPECT_EQ(bridge.awaitable->result().ok(), 42);
}

TEST(FFIAwaitable, CreateBridgeResolveAfterSuspend) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x * 2; });

  EXPECT(!bridge.awaitable->await_ready());

  bridge.awaitable->await_suspend({continuation_0});

  bridge.promise.set_value(21);

  auto result = loop.wait(0);
  EXPECT(result.has_value());
  EXPECT_EQ(result->ptr(), continuation_0);

  EXPECT(bridge.awaitable->await_ready());
  EXPECT(bridge.awaitable->result().is_ok());
  EXPECT_EQ(bridge.awaitable->result().ok(), 42);
}

TEST(FFIAwaitable, TransformString) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<std::string>::create_bridge<int>(loop, [](int x) { return std::to_string(x); });

  bridge.promise.set_value(123);

  EXPECT(bridge.awaitable->await_ready());
  EXPECT(bridge.awaitable->result().is_ok());
  EXPECT_EQ(bridge.awaitable->result().ok(), "123");
}

TEST(FFIAwaitable, ResolveFromDifferentThread) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x + 10; });

  bridge.awaitable->await_suspend({continuation_1});

  loop.run_in_context([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bridge.promise.set_value(90);
  });

  auto result = wait_for_continuation(loop);
  EXPECT_EQ(result.ptr(), continuation_1);

  EXPECT(bridge.awaitable->result().is_ok());
  EXPECT_EQ(bridge.awaitable->result().ok(), 100);
}

TEST(FFIAwaitable, ConcurrentResolveAndSuspend) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x; });

  std::thread suspender([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bridge.awaitable->await_suspend({continuation_0});
  });

  std::thread resolver([promise = std::move(bridge.promise)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    promise.set_value(777);
  });

  auto result = wait_for_continuation(loop);
  EXPECT_EQ(result.ptr(), continuation_0);

  suspender.join();
  resolver.join();

  EXPECT(bridge.awaitable->await_ready());
  EXPECT_EQ(bridge.awaitable->result().ok(), 777);
}

TEST(FFIAwaitable, DestroyUnresolvedWithoutSuspend) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x; });

  bridge.awaitable->destroy();

  auto result = loop.wait(0);
  EXPECT(!result.has_value());
}

TEST(FFIAwaitable, DestroyUnresolvedAfterSuspend) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x; });

  bridge.awaitable->await_suspend({continuation_0});
  bridge.awaitable->destroy();

  auto result = loop.wait(0);
  EXPECT(result.has_value());
  EXPECT_EQ(result->ptr(), continuation_0);
}

TEST(FFIAwaitable, DestroyResolvedWithoutSuspend) {
  FFIEventLoop loop(1);

  auto awaitable = FFIAwaitable<int>::create_resolved(loop, 42);

  EXPECT(awaitable->await_ready());
  awaitable->destroy();
}

TEST(FFIAwaitable, DestroyResolvedAfterSuspend) {
  FFIEventLoop loop(1);

  auto awaitable = FFIAwaitable<int>::create_resolved(loop, 42);

  awaitable->await_suspend({continuation_1});
  awaitable->destroy();

  auto result = loop.wait(0);
  EXPECT(result.has_value());
  EXPECT_EQ(result->ptr(), continuation_1);
}

TEST(FFIAwaitable, DestroyConcurrentWithResolve) {
  FFIEventLoop loop(1);

  auto bridge = FFIAwaitable<int>::create_bridge<int>(loop, [](int x) { return x; });

  bridge.awaitable->await_suspend({continuation_0});

  std::thread resolver([promise = std::move(bridge.promise)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    promise.set_value(999);
  });

  std::thread destroyer([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bridge.awaitable->destroy();
  });

  auto result = wait_for_continuation(loop);

  resolver.join();
  destroyer.join();

  EXPECT_EQ(result.ptr(), continuation_0);
}

}  // namespace
}  // namespace tonlib
