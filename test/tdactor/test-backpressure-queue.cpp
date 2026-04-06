#include "td/actor/BackpressureQueue.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"

namespace td::actor {
namespace {

TEST(BackpressureQueue, BasicPushPop) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 10);

    auto ok = co_await q.push(42);
    EXPECT(ok);
    auto val = co_await q.pop();
    EXPECT_EQ(42, val);

    for (int i = 0; i < 5; i++) {
      co_await q.push(i);
    }
    for (int i = 0; i < 5; i++) {
      auto v = co_await q.pop();
      EXPECT_EQ(i, v);
    }

    q.close();
    co_return {};
  });
}

TEST(BackpressureQueue, NonBlockingPushPop) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 3);

    auto r1 = co_await q.try_push(1);
    EXPECT(r1);
    auto r2 = co_await q.try_push(2);
    EXPECT(r2);
    auto r3 = co_await q.try_push(3);
    EXPECT(r3);
    auto r4 = co_await q.try_push(4);
    EXPECT(!r4);  // full

    auto v1 = co_await q.pop();
    EXPECT_EQ(1, v1);
    auto v2 = co_await q.pop();
    EXPECT_EQ(2, v2);
    auto v3 = co_await q.pop();
    EXPECT_EQ(3, v3);

    auto empty = co_await q.try_pop().wrap();
    EXPECT(empty.is_error());  // empty

    q.close();
    co_return {};
  });
}

TEST(BackpressureQueue, BackpressureBlocksProducer) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 2);

    co_await q.push(1);
    co_await q.push(2);
    auto full = co_await q.try_push(3);
    EXPECT(!full);

    // Push(3) will block until space is available
    auto push_task = q.push(3);

    // Pop to make space
    auto v1 = co_await q.pop();
    EXPECT_EQ(1, v1);
    co_await ts.wait_sync_work();

    auto ok = co_await std::move(push_task);
    EXPECT(ok);

    auto v2 = co_await q.pop();
    EXPECT_EQ(2, v2);
    auto v3 = co_await q.pop();
    EXPECT_EQ(3, v3);

    q.close();
    co_return {};
  });
}

TEST(BackpressureQueue, PopBlocksConsumer) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 10);

    // Pop on empty will block
    auto pop_task = q.pop();

    co_await q.push(42);
    co_await ts.wait_sync_work();

    auto val = co_await std::move(pop_task);
    EXPECT_EQ(42, val);

    q.close();
    co_return {};
  });
}

TEST(BackpressureQueue, CloseWakesBlockedPop) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 2);

    auto pop_task = q.pop();

    q.close();
    co_await ts.wait_sync_work();

    auto result = co_await std::move(pop_task).wrap();
    EXPECT(result.is_error());

    auto r = co_await q.push(42);
    EXPECT(!r);

    co_return {};
  });
}

TEST(BackpressureQueue, CloseWakesBlockedPush) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 1);

    co_await q.push(1);  // fill

    auto push_task = q.push(2);

    q.close();
    co_await ts.wait_sync_work();

    auto r = co_await std::move(push_task);
    EXPECT(!r);

    co_return {};
  });
}

TEST(BackpressureQueue, CloseAllowsDrainingRemainingItems) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 10);

    co_await q.push(1);
    co_await q.push(2);
    co_await q.push(3);
    q.close();
    co_await ts.wait_sync_work();

    auto v1 = co_await q.pop();
    EXPECT_EQ(1, v1);
    auto v2 = co_await q.pop();
    EXPECT_EQ(2, v2);
    auto v3 = co_await q.pop();
    EXPECT_EQ(3, v3);

    auto r = co_await q.pop().wrap();
    EXPECT(r.is_error());

    co_return {};
  });
}

TEST(BackpressureQueue, ProducerConsumerStreaming) {
  TestScheduler ts;
  int received = 0;

  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 4);

    auto producer = [](BackpressureQueue<int> q) -> Task<> {
      for (int i = 0; i < 20; i++) {
        if (!co_await q.push(i)) {
          break;
        }
      }
      q.close();
      co_return {};
    }(q);
    auto producer_started = std::move(producer).start();

    while (true) {
      auto result = co_await q.pop().wrap();
      if (result.is_error()) {
        break;
      }
      auto val = result.move_as_ok();
      EXPECT_EQ(received, val);
      received++;
    }

    co_await std::move(producer_started);
    co_return {};
  });

  EXPECT_EQ(20, received);
}

TEST(BackpressureQueue, MultipleProducers) {
  TestScheduler ts;
  int total = 0;

  ts.run([&]() -> Task<Unit> {
    BackpressureQueue<int> q("q", 4);
    constexpr int NUM_PRODUCERS = 3;
    constexpr int ITEMS_PER = 10;

    for (int p = 0; p < NUM_PRODUCERS; p++) {
      [](BackpressureQueue<int> q, int p, int items) -> Task<> {
        for (int i = 0; i < items; i++) {
          if (!co_await q.push(p * 100 + i)) {
            break;
          }
        }
        co_return {};
      }(q, p, ITEMS_PER)
                                                            .start()
                                                            .detach();
    }

    for (int i = 0; i < NUM_PRODUCERS * ITEMS_PER; i++) {
      co_await q.pop();
      total++;
    }

    q.close();
    co_return {};
  });

  EXPECT_EQ(30, total);
}

}  // namespace
}  // namespace td::actor
