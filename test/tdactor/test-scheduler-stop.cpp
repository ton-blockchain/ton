#include "td/actor/actor.h"
#include "td/actor/coro_utils.h"
#include "td/utils/tests.h"

// Tests for SchedulerContext::stop() behavior with active and stopping actors.
//
// tear_down() is NOT called for actors that are still alive (haven't called self.stop()) when
// SchedulerContext::stop() fires. Only C++ destructors run.

namespace td::actor {
namespace {

class StopSchedulerActor : public Actor {
 public:
  void start_up() override {
    yield();
  }
  void wake_up() override {
    SchedulerContext::get().stop();
  }
};

class CountdownStopActor : public Actor {
 public:
  CountdownStopActor(double timeout) : timeout_(timeout) {
  }

  void start_up() override {
    alarm_timestamp() = Timestamp::in(timeout_);
  }
  void alarm() override {
    SchedulerContext::get().stop();
  }

 private:
  double timeout_;
};

namespace test_stop_alive_actors {

// Verify that when SchedulerContext::stop() is called with active actors, all actors
// are destroyed (C++ destructors run) but tear_down() is not called.

int g_start_up_count;
int g_tear_down_count;
int g_destructor_count;

class AliveActor : public Actor {
 public:
  void start_up() override {
    g_start_up_count++;
  }
  void tear_down() override {
    g_tear_down_count++;
  }
  ~AliveActor() override {
    g_destructor_count++;
  }
};

TEST(SchedulerStop, AliveActors) {
  g_start_up_count = 0;
  g_tear_down_count = 0;
  g_destructor_count = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<AliveActor>("Alive1").release();
      create_actor<AliveActor>("Alive2").release();
      create_actor<AliveActor>("Alive3").release();
      create_actor<StopSchedulerActor>("Stopper").release();
    });
    scheduler.run();
  }

  EXPECT_EQ(g_start_up_count, 3);
  EXPECT_EQ(g_destructor_count, 3);
  EXPECT_EQ(g_tear_down_count, 0);
}

}  // namespace test_stop_alive_actors

namespace test_stop_self_stopped_actors {

// Verify that actors which called self.stop() before SchedulerContext::stop() DO get
// their tear_down() called (through the normal path), while others don't.

int g_torn_down;
int g_not_torn_down;

class SelfStoppingActor : public Actor {
 public:
  void start_up() override {
    stop();
  }
  void tear_down() override {
    g_torn_down++;
  }
};

class IdleActor : public Actor {
 public:
  void start_up() override {
    // Stay alive -- don't call stop().
  }
  void tear_down() override {
    g_not_torn_down++;
  }
};

TEST(SchedulerStop, SelfStoppedVsAlive) {
  g_torn_down = 0;
  g_not_torn_down = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<SelfStoppingActor>("SelfStop1").release();
      create_actor<SelfStoppingActor>("SelfStop2").release();
      create_actor<IdleActor>("Idle1").release();
      create_actor<IdleActor>("Idle2").release();
      create_actor<CountdownStopActor>("Stopper", 0.001).release();
    });
    scheduler.run();

    EXPECT_EQ(g_torn_down, 2);
  }

  EXPECT_EQ(g_not_torn_down, 0);
}

}  // namespace test_stop_self_stopped_actors

namespace test_stop_with_pending_alarm {

// An actor sets an alarm far in the future. SchedulerContext::stop() is called before the alarm
// fires. The alarm must NOT fire, but the actor must be cleaned up.

static bool g_alarm_fired;
static bool g_actor_destroyed;

class AlarmedActor : public Actor {
 public:
  void start_up() override {
    alarm_timestamp() = Timestamp::in(24 * 3600);
  }
  void alarm() override {
    g_alarm_fired = true;
  }
  ~AlarmedActor() override {
    g_actor_destroyed = true;
  }
};

TEST(SchedulerStop, PendingAlarm) {
  g_alarm_fired = false;
  g_actor_destroyed = false;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<AlarmedActor>("Alarmed").release();
      create_actor<StopSchedulerActor>("Stopper").release();
    });
    scheduler.run();
  }

  EXPECT(!g_alarm_fired);
  EXPECT(g_actor_destroyed);
}

}  // namespace test_stop_with_pending_alarm

namespace test_stop_with_repeating_alarm {

// An actor repeatedly re-arms its alarm. Verify that SchedulerContext::stop() terminates cleanly
// despite the actor continuously rescheduling itself.

int g_alarm_count;

class RepeatingAlarmActor : public Actor {
 public:
  void start_up() override {
    alarm_timestamp() = Timestamp::in(0.001);
  }
  void alarm() override {
    g_alarm_count++;
    alarm_timestamp() = Timestamp::in(0.001);
  }
};

TEST(SchedulerStop, RepeatingAlarm) {
  g_alarm_count = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<RepeatingAlarmActor>("Repeater").release();
      create_actor<CountdownStopActor>("Stopper", 0.01).release();
    });
    scheduler.run();

    EXPECT(g_alarm_count > 0);
  }
}

}  // namespace test_stop_with_repeating_alarm

namespace test_stop_from_start_up {

// An actor calls SchedulerContext::stop() directly in start_up(). Other actors created alongside it
// may or may not have their start_up() called (depends on queue ordering), but all must be cleaned
// up without crashes.

static bool g_other_destroyed;

class OtherActor : public Actor {
 public:
  ~OtherActor() override {
    g_other_destroyed = true;
  }
};

class ImmediateStopActor : public Actor {
 public:
  void start_up() override {
    SchedulerContext::get().stop();
  }
};

TEST(SchedulerStop, FromStartUp) {
  g_other_destroyed = false;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<OtherActor>("Other").release();
      create_actor<ImmediateStopActor>("ImmediateStop").release();
    });
    scheduler.run();
  }

  EXPECT(g_other_destroyed);
}

}  // namespace test_stop_from_start_up

namespace test_stop_with_owned_children {

// A parent actor holds ActorOwn<> references to child actors. When the scheduler stops, the
// parent's C++ destructor runs and ActorOwn sends hangup messages. Since there is no
// SchedulerContext during cleanup, these messages are silently dropped. Verify this doesn't crash
// and all actors are destroyed.

int g_parent_destroyed;
int g_child_destroyed;
int g_child_hangup_count;
int g_child_tear_down_count;

class ChildActor : public Actor {
 public:
  void hangup() override {
    g_child_hangup_count++;
    stop();
  }
  void tear_down() override {
    g_child_tear_down_count++;
  }
  ~ChildActor() override {
    g_child_destroyed++;
  }
};

class ParentActor : public Actor {
 public:
  void start_up() override {
    children_.push_back(create_actor<ChildActor>("Child1"));
    children_.push_back(create_actor<ChildActor>("Child2"));
    children_.push_back(create_actor<ChildActor>("Child3"));
  }
  ~ParentActor() override {
    g_parent_destroyed++;
  }

 private:
  std::vector<ActorOwn<ChildActor>> children_;
};

TEST(SchedulerStop, OwnedChildren) {
  g_parent_destroyed = 0;
  g_child_destroyed = 0;
  g_child_hangup_count = 0;
  g_child_tear_down_count = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<ParentActor>("Parent").release();
      create_actor<StopSchedulerActor>("Stopper").release();
    });
    scheduler.run();
  }

  EXPECT_EQ(g_parent_destroyed, 1);
  EXPECT_EQ(g_child_destroyed, 3);
  // During cleanup, ActorOwn destructors fire but there is no SchedulerContext, so hangup messages
  // are silently dropped. Children don't get hangup/tear_down.
  EXPECT_EQ(g_child_hangup_count, 0);
  EXPECT_EQ(g_child_tear_down_count, 0);
}

}  // namespace test_stop_with_owned_children

namespace test_stop_with_pending_closures {

int g_closure_executed_count;

class ReceiverActor : public Actor {
 public:
  void on_message() {
    g_closure_executed_count++;
  }
};

class SenderAndStopActor : public Actor {
 public:
  explicit SenderAndStopActor(ActorId<ReceiverActor> receiver) : receiver_(receiver) {
  }

  void start_up() override {
    for (int i = 0; i < 500; i++) {
      send_closure(receiver_, &ReceiverActor::on_message);
    }
    SchedulerContext::get().stop();
  }

 private:
  ActorId<ReceiverActor> receiver_;
};

TEST(SchedulerStop, PendingClosures) {
  g_closure_executed_count = 0;

  {
    Scheduler scheduler({2});
    scheduler.run_in_context([&] {
      auto receiver = create_actor<ReceiverActor>("Receiver");
      auto receiver_id = receiver.get();
      receiver.release();
      create_actor<SenderAndStopActor>("Sender", receiver_id).release();
    });
    scheduler.run();
  }

  EXPECT(g_closure_executed_count == 500);
}

}  // namespace test_stop_with_pending_closures

namespace test_stop_with_yielding_actor {

// An actor that continuously yields (re-enqueues itself). Verify that SchedulerContext::stop()
// terminates despite the actor always being in the queue.

int g_loop_count;

class YieldingActor : public Actor {
 public:
  void start_up() override {
    yield();
  }
  void wake_up() override {
    g_loop_count++;
    yield();
  }
};

TEST(SchedulerStop, YieldingActor) {
  g_loop_count = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<YieldingActor>("Yielder").release();
      create_actor<CountdownStopActor>("Stopper", 0.01).release();
    });
    scheduler.run();
  }

  EXPECT(g_loop_count > 0);
}

}  // namespace test_stop_with_yielding_actor

namespace test_stop_idempotent {

// Calling SchedulerContext::stop() multiple times must be safe (idempotent). The second call is a
// no-op due to the compare_exchange_strong guard.

class DoubleStopActor : public Actor {
 public:
  void start_up() override {
    SchedulerContext::get().stop();
    SchedulerContext::get().stop();
  }
};

TEST(SchedulerStop, Idempotent) {
  Scheduler scheduler({1});
  scheduler.run_in_context([&] { create_actor<DoubleStopActor>("DoubleStop").release(); });
  scheduler.run();
}

}  // namespace test_stop_idempotent

namespace test_stop_with_multiple_alarm_actors {

// Multiple actors with pending alarms at different times. Verify clean shutdown.

int g_actors_destroyed;

class TimedActor : public Actor {
 public:
  explicit TimedActor(double delay) : delay_(delay) {
  }
  void start_up() override {
    alarm_timestamp() = Timestamp::in(delay_);
  }
  void alarm() override {
    alarm_timestamp() = Timestamp::in(delay_);
  }
  ~TimedActor() override {
    g_actors_destroyed++;
  }

 private:
  double delay_;
};

TEST(SchedulerStop, MultipleAlarmActors) {
  g_actors_destroyed = 0;

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] {
      create_actor<TimedActor>("T1", 100.0).release();
      create_actor<TimedActor>("T2", 200.0).release();
      create_actor<TimedActor>("T3", 300.0).release();
      create_actor<TimedActor>("T4", 0.001).release();
      create_actor<StopSchedulerActor>("Stopper").release();
    });
    scheduler.run();
  }

  EXPECT_EQ(g_actors_destroyed, 4);
}

}  // namespace test_stop_with_multiple_alarm_actors

namespace test_scheduler_stops_in_tear_down {

// When one actor stops normally (calling self.stop()), its tear_down may interact
// with other actors. If the scheduler is then stopped, remaining actors must still
// be cleaned up.

static std::vector<int> g_sequence;

class WorkerActor : public Actor {
 public:
  void start_up() override {
    g_sequence.push_back(1);
  }
  void do_work() {
    g_sequence.push_back(2);
    stop();
  }
  void tear_down() override {
    g_sequence.push_back(3);
    SchedulerContext::get().stop();
  }
};

class OrchestratorActor : public Actor {
 public:
  void start_up() override {
    auto worker = create_actor<WorkerActor>("Worker");
    send_closure(worker, &WorkerActor::do_work);
    worker.release();

    alarm_timestamp() = Timestamp::in(24 * 3600);
  }

  void tear_down() override {
    g_sequence.push_back(-1);
  }

  void alarm() override {
    g_sequence.push_back(-1);
  }
};

TEST(SchedulerStop, SchedulerStopInTearDown) {
  g_sequence.clear();

  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] { create_actor<OrchestratorActor>("Orchestrator").release(); });
    scheduler.run();
  }

  EXPECT_EQ(g_sequence, (std::vector{1, 2, 3}));
}

}  // namespace test_scheduler_stops_in_tear_down

namespace test_normal_flow {

int g_tear_down_child = 0;
int g_tear_down_parent = 0;

class ChildActor : public Actor {
 public:
  void tear_down() override {
    ++g_tear_down_child;
  }
};

class SleepyActor : public Actor {
 public:
  void start_up() override {
    task().start().detach();
  }

  void tear_down() override {
    ++g_tear_down_child;
  }

 private:
  Task<> task() {
    co_await coro_sleep(td::Timestamp::in(24 * 3600));
    co_return {};
  }
};

class ParentActor : public Actor {
 public:
  void start_up() override {
    actors_.push_back(create_actor<ChildActor>("Child"));
    actors_.push_back(create_actor<SleepyActor>("SleepyChild"));
    alarm_timestamp() = Timestamp::in(0.01);
  }

  void tear_down() override {
    ++g_tear_down_parent;
  }

  void alarm() override {
    stop();
    SchedulerContext::get().stop();
  }

 private:
  std::vector<ActorOwn<>> actors_;
};

TEST(SchedulerStop, NormalFlow) {
  {
    Scheduler scheduler({1});
    scheduler.run_in_context([&] { create_actor<ParentActor>("Parent").release(); });
    scheduler.run();
  }

  EXPECT_EQ(g_tear_down_child, 2);
  EXPECT_EQ(g_tear_down_parent, 1);
}

}  // namespace test_normal_flow

}  // namespace
}  // namespace td::actor
