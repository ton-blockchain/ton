#include "td/actor/TestScheduler.h"
#include "td/actor/coro_utils.h"
#include "td/utils/tests.h"

namespace td::actor {
namespace {

TEST(TestScheduler, BasicActorStartUp) {
  static bool started = false;
  started = false;

  struct MyActor : Actor {
    void start_up() override {
      started = true;
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    create_actor<MyActor>("test").release();
    co_await ts.wait_sync_work();
    EXPECT(started);
    co_return {};
  });
}

TEST(TestScheduler, AlarmWithVirtualTime) {
  static bool alarm_fired = false;
  alarm_fired = false;

  struct TimedActor : Actor {
    void start_up() override {
      alarm_timestamp() = Timestamp::in(5.0);
    }
    void alarm() override {
      alarm_fired = true;
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    create_actor<TimedActor>("timed").release();
    co_await ts.wait_sync_work();

    EXPECT(!alarm_fired);
    EXPECT(ts.next_timeout_in() == 5.0);

    ts.advance_time(5.0);
    co_await ts.wait_sync_work();

    EXPECT(alarm_fired);
    co_return {};
  });
}

TEST(TestScheduler, TimeFrozenDuringWork) {
  static double time_at_alarm = 0;
  time_at_alarm = 0;

  struct MyActor : Actor {
    void start_up() override {
      alarm_timestamp() = Timestamp::in(1.0);
    }
    void alarm() override {
      time_at_alarm = Time::now();
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    double t0 = Time::now();
    create_actor<MyActor>("test").release();
    co_await ts.wait_sync_work();

    EXPECT_EQ(Time::now(), t0);

    ts.advance_time(1.0);
    double t1 = Time::now();
    co_await ts.wait_sync_work();

    EXPECT_EQ(time_at_alarm, t1);
    EXPECT_EQ(Time::now(), t1);

    co_return {};
  });
}

TEST(TestScheduler, RepeatingAlarm) {
  static int alarm_count = 0;
  alarm_count = 0;

  struct RepeatingActor : Actor {
    void start_up() override {
      alarm_timestamp() = Timestamp::in(1.0);
    }
    void alarm() override {
      alarm_count++;
      if (alarm_count < 5) {
        alarm_timestamp() = Timestamp::in(1.0);
      }
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    create_actor<RepeatingActor>("repeater").release();
    co_await ts.wait_sync_work();

    for (int i = 0; i < 5; i++) {
      EXPECT_EQ(alarm_count, i);
      EXPECT_EQ(ts.next_timeout_in(), 1.0);
      ts.advance_time(ts.next_timeout_in());
      co_await ts.wait_sync_work();
    }

    EXPECT_EQ(alarm_count, 5);
    co_return {};
  });
}

TEST(TestScheduler, SendClosure) {
  static int value = 0;
  value = 0;

  struct Worker : Actor {
    void set_value(int v) {
      value = v;
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    auto worker = create_actor<Worker>("worker");
    co_await ts.wait_sync_work();

    send_closure(worker, &Worker::set_value, 42);
    co_await ts.wait_sync_work();

    EXPECT_EQ(value, 42);
    co_return {};
  });
}

TEST(TestScheduler, CoroSleep) {
  static bool woke_up = false;
  woke_up = false;

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    auto sleeper = [&]() -> Task<Unit> {
      co_await coro_sleep(Timestamp::in(3.0));
      woke_up = true;
      co_return {};
    }();
    auto started = std::move(sleeper).start();

    co_await ts.wait_sync_work();
    EXPECT(!woke_up);
    EXPECT(ts.next_timeout_in() == 3.0);

    ts.advance_time(3.0);
    co_await ts.wait_sync_work();
    EXPECT(woke_up);

    co_return {};
  });
}

TEST(TestScheduler, MultipleActorsInteracting) {
  static int pong_count = 0;
  pong_count = 0;

  struct Ponger : Actor {
    void pong() {
      pong_count++;
    }
  };

  struct Pinger : Actor {
    ActorId<Ponger> ponger_;
    int remaining_;

    Pinger(ActorId<Ponger> ponger, int count) : ponger_(ponger), remaining_(count) {
    }

    void start_up() override {
      fire();
    }
    void alarm() override {
      fire();
    }
    void fire() {
      if (remaining_-- > 0) {
        send_closure(ponger_, &Ponger::pong);
        alarm_timestamp() = Timestamp::in(1.0);
      }
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    auto ponger = create_actor<Ponger>("ponger");
    create_actor<Pinger>("pinger", ponger.get(), 3).release();
    ponger.release();

    co_await ts.wait_sync_work();
    EXPECT_EQ(pong_count, 1);

    for (int i = 2; i <= 3; i++) {
      ts.advance_time(ts.next_timeout_in());
      co_await ts.wait_sync_work();
      EXPECT_EQ(pong_count, i);
    }

    co_return {};
  });
}

TEST(TestScheduler, NextTimeoutInfinity) {
  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    EXPECT(ts.next_timeout_in() == std::numeric_limits<double>::infinity());

    struct IdleActor : Actor {
      void start_up() override {
      }
    };
    create_actor<IdleActor>("idle").release();
    co_await ts.wait_sync_work();

    EXPECT(ts.next_timeout_in() == std::numeric_limits<double>::infinity());
    co_return {};
  });
}

TEST(TestScheduler, ActorStopAndTearDown) {
  static bool torn_down = false;
  torn_down = false;

  struct MyActor : Actor {
    void start_up() override {
      stop();
    }
    void tear_down() override {
      torn_down = true;
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    create_actor<MyActor>("self-stopper").release();
    co_await ts.wait_sync_work();
    EXPECT(torn_down);
    co_return {};
  });
}

TEST(TestScheduler, AdvanceTimePrecise) {
  static bool alarm_a = false;
  static bool alarm_b = false;
  alarm_a = alarm_b = false;

  struct ActorA : Actor {
    void start_up() override {
      alarm_timestamp() = Timestamp::in(2.0);
    }
    void alarm() override {
      alarm_a = true;
    }
  };
  struct ActorB : Actor {
    void start_up() override {
      alarm_timestamp() = Timestamp::in(5.0);
    }
    void alarm() override {
      alarm_b = true;
    }
  };

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    create_actor<ActorA>("a").release();
    create_actor<ActorB>("b").release();
    co_await ts.wait_sync_work();

    EXPECT(!alarm_a);
    EXPECT(!alarm_b);
    EXPECT(ts.next_timeout_in() == 2.0);

    ts.advance_time(2.0);
    co_await ts.wait_sync_work();
    EXPECT(alarm_a);
    EXPECT(!alarm_b);

    EXPECT(ts.next_timeout_in() == 3.0);

    ts.advance_time(3.0);
    co_await ts.wait_sync_work();
    EXPECT(alarm_b);

    co_return {};
  });
}

TEST(TestScheduler, CleanupDrainsPendingAlarms) {
  static int iterations = 0;
  iterations = 0;

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    auto looper = []() -> Task<> {
      for (int i = 0; i < 5; i++) {
        iterations++;
        co_await coro_sleep(Timestamp::in(1.0));
      }
      co_return {};
    };
    std::move(looper()).start().detach();

    co_await ts.wait_sync_work();
    EXPECT_EQ(iterations, 1);

    co_return {};
  });

  EXPECT_EQ(iterations, 5);
}

TEST(TestScheduler, CleanupDrainsPendingAlarmsWithActor) {
  static int iterations = 0;
  iterations = 0;

  TestScheduler ts;
  ts.run([&]() -> Task<Unit> {
    struct ActorB : Actor {};

    struct LoopActor : Actor {
      void start_up() {
        run().start().detach();
      }

      Task<> run() {
        auto dummy = create_actor<ActorB>("dummy");

        for (int i = 0; i < 5; i++) {
          iterations++;
          co_await coro_sleep(Timestamp::in(1.0));
        }
        co_return {};
      };
    };

    auto actor = create_actor<LoopActor>("looper");

    co_await ts.wait_sync_work();
    EXPECT_EQ(iterations, 1);

    co_return {};
  });
}

}  // namespace
}  // namespace td::actor
