#include "td/utils/tests.h"
#include "validator/consensus/runtime.h"

namespace ton::runtime::test_simple_message_to_self {
namespace {

// We want to spawn a MainBus and SampleActor with it. SampleActor publishes an event, which it then
// receives and stops the bus.

struct MainBus : Bus {
  MainBus(int cookie) : bus_cookie(cookie) {
  }

  ~MainBus() {
    td::actor::SchedulerContext::get().stop();
  }

  int bus_cookie;

  struct SampleEvent {
    int event_cookie;
  };

  using Events = td::TypeList<SampleEvent>;
};

bool g_event_received = false;

class SampleActor : public SpawnsWith<MainBus>, public ConnectsTo<MainBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    EXPECT_EQ(owning_bus()->bus_cookie, 42);
    EXPECT_EQ(get_name(), td::Slice("SampleActor"));
    owning_bus().publish<MainBus::SampleEvent>(43);
  }

  template <>
  void handle(BusHandle<MainBus> bus, std::shared_ptr<const MainBus::SampleEvent> event) {
    EXPECT_EQ(event->event_cookie, 43);
    EXPECT_EQ(bus->bus_cookie, 42);
    g_event_received = true;
    stop();
  }
};

TEST(Runtime, SimpleMessageToSelf) {
  td::actor::Scheduler scheduler({1});

  Runtime runtime;
  runtime.register_actor<SampleActor>("SampleActor");

  scheduler.run_in_context([&] { runtime.start(std::make_shared<MainBus>(42)); });
  scheduler.run();

  EXPECT(g_event_received);
}

}  // namespace
}  // namespace ton::runtime::test_simple_message_to_self

namespace ton::runtime::test_bus_tree {
namespace {

// We want to create and then destroy the following bus tree:
//  RootBus (root)  -> RootController
//  ├── Level1Bus (id=0)  -> Level1Controller, Level1Worker
//  │   └── Level2Bus  -> Level2Worker
//  └── Level1Bus (id=1)  -> Level1Controller, Level1Worker

struct RootBus : Bus {
  ~RootBus() {
    td::actor::SchedulerContext::get().stop();
  }

  using Events = td::TypeList<>;
};

struct Level1Bus : Bus {
  Level1Bus(int id) : id(id) {
  }

  int id;

  struct StopRequested {};

  using Events = td::TypeList<StopRequested>;
};

struct Level2Bus : Bus {
  struct StopRequested {};

  using Events = td::TypeList<StopRequested>;
};

class RootController : public SpawnsWith<RootBus>, public ConnectsTo<RootBus, Level1Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    EXPECT_EQ(get_name(), td::Slice("RootController"));

    children_.push_back(owning_bus().create_child("Level1[0]", std::make_shared<Level1Bus>(0)));
    children_.push_back(owning_bus().create_child("Level1[1]", std::make_shared<Level1Bus>(1)));
    for (auto& child : children_) {
      child.publish<Level1Bus::StopRequested>();
    }
    stop();
  }

 private:
  std::vector<BusHandle<Level1Bus>> children_;
};

class Level1Controller : public SpawnsWith<Level1Bus>, public ConnectsTo<Level1Bus, Level2Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto bus = owning_bus();

    if (bus->id == 0) {
      EXPECT_EQ(get_name(), td::Slice("Level1[0].Level1Controller"));
    } else if (bus->id == 1) {
      EXPECT_EQ(get_name(), td::Slice("Level1[1].Level1Controller"));
    } else {
      EXPECT(false);
    }

    if (bus->id == 0) {
      child_ = owning_bus().create_child("Level2", std::make_shared<Level2Bus>());
    }
  }

  template <>
  void handle(BusHandle<Level1Bus> bus, std::shared_ptr<const Level1Bus::StopRequested>) {
    if (child_) {
      child_.publish<Level2Bus::StopRequested>();
    }
    stop();
  }

 private:
  BusHandle<Level2Bus> child_;
};

class Level1Worker : public SpawnsWith<Level1Bus>, public ConnectsTo<Level1Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    EXPECT_EQ(get_name(), PSTRING() << "Level1[" << owning_bus()->id << "].Level1Worker");
  }

  template <>
  void handle(BusHandle<Level1Bus> bus, std::shared_ptr<const Level1Bus::StopRequested>) {
    stop();
  }
};

class Level2Worker : public SpawnsWith<Level2Bus>, public ConnectsTo<Level2Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    EXPECT_EQ(get_name(), td::Slice("Level1[0].Level2.Level2Worker"));
  }

  template <>
  void handle(BusHandle<Level2Bus> bus, std::shared_ptr<const Level2Bus::StopRequested>) {
    stop();
  }
};

TEST(Runtime, BusTree) {
  td::actor::Scheduler scheduler({1});

  Runtime runtime;
  runtime.register_actor<RootController>("RootController");
  runtime.register_actor<Level1Controller>("Level1Controller");
  runtime.register_actor<Level1Worker>("Level1Worker");
  runtime.register_actor<Level2Worker>("Level2Worker");

  scheduler.run_in_context([&] { runtime.start(std::make_shared<RootBus>()); });
  scheduler.run();
}

}  // namespace
}  // namespace ton::runtime::test_bus_tree

namespace ton::runtime::test_inheritance {
namespace {

struct ParentBus : Bus {
  ParentBus(int parent) : parent(parent) {
  }

  ~ParentBus() {
    td::actor::SchedulerContext::get().stop();
  }

  int parent;

  struct ParentEvent {
    int value;
  };

  struct ParentEvent2 {
    int value;
  };

  struct ParentEvent3 {
    int value;
  };

  using Events = td::TypeList<ParentEvent, ParentEvent2, ParentEvent3>;
};

struct ChildBus : ParentBus {
  ChildBus(int parent, int child) : ParentBus(parent), child(child) {
  }

  int child;

  struct ChildEvent {
    int value;
  };

  using Parent = ParentBus;
  using Events = td::TypeList<ChildEvent>;
};

bool g_ran_1st = false;
bool g_ran_2nd = false;
bool g_ran_3rd = false;
bool g_ran_4th_in_parent = false;
bool g_ran_4th_in_child = false;
bool g_ran_5th = false;

class ParentBusActor : public SpawnsWith<ParentBus>, public ConnectsTo<ParentBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {  // 1st
    check_bus(owning_bus());
    g_ran_1st = true;

    owning_bus().publish<ParentBus::ParentEvent>(100);
  }

  template <>
  void handle(BusHandle<ParentBus> bus, std::shared_ptr<const ParentBus::ParentEvent2> event) {  // 4th
    check_bus(bus);
    EXPECT_EQ(event->value, 102);
    g_ran_4th_in_parent = true;

    owning_bus().publish<ParentBus::ParentEvent3>(103);

    stop();
  }

 private:
  void check_bus(BusHandle<ParentBus> bus) {
    EXPECT_EQ(bus->parent, 228);
  }
};

class ChildBusActor : public SpawnsWith<ChildBus>, public ConnectsTo<ChildBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle<ChildBus> bus, std::shared_ptr<const ParentBus::ParentEvent> event) {  // 2nd
    check_bus(bus);
    EXPECT_EQ(event->value, 100);
    g_ran_2nd = true;

    owning_bus().publish<ChildBus::ChildEvent>(101);
  }

  template <>
  void handle(BusHandle<ChildBus> bus, std::shared_ptr<const ChildBus::ChildEvent> event) {  // 3rd
    check_bus(bus);
    EXPECT_EQ(event->value, 101);
    g_ran_3rd = true;

    owning_bus().publish<ParentBus::ParentEvent2>(102);
  }

  template <>
  void handle(BusHandle<ChildBus> bus, std::shared_ptr<const ChildBus::ParentEvent2> event) {  // 4th
    check_bus(bus);
    EXPECT_EQ(event->value, 102);
    g_ran_4th_in_child = true;
  }

  template <>
  void handle(BusHandle<ChildBus> bus, std::shared_ptr<const ParentBus::ParentEvent3> event) {  // 5th
    check_bus(bus);
    EXPECT_EQ(event->value, 103);
    g_ran_5th = true;

    stop();
  }

 private:
  void check_bus(BusHandle<ChildBus> bus) {
    EXPECT_EQ(bus->parent, 228);
    EXPECT_EQ(bus->child, 229);
  }
};

TEST(Runtime, Inheritance) {
  td::actor::Scheduler scheduler({1});

  Runtime runtime;
  runtime.register_actor<ParentBusActor>("ParentBusActor");
  runtime.register_actor<ChildBusActor>("ChildBusActor");

  scheduler.run_in_context([&] { runtime.start(std::make_shared<ChildBus>(228, 229)); });
  scheduler.run();

  EXPECT(g_ran_1st);
  EXPECT(g_ran_2nd);
  EXPECT(g_ran_3rd);
  EXPECT(g_ran_4th_in_parent);
  EXPECT(g_ran_4th_in_child);
  EXPECT(g_ran_5th);
}

}  // namespace
}  // namespace ton::runtime::test_inheritance

namespace ton::runtime::test_runtime_lifetime {
namespace {
// Lifetime of detail::Runtime should be extended while there are running actors even if user-facing
// Runtime is destroyed.

static std::vector<int> s_sequence;

class RuntimeRunner;

struct ParentBus : Bus {
  using Events = td::TypeList<>;

  ParentBus() {
    s_sequence.push_back(2);
  }

  ~ParentBus();

  td::actor::ActorId<RuntimeRunner> runner;
};

struct ChildBus : Bus {
  using Events = td::TypeList<>;
};

class BusActor : public SpawnsWith<ParentBus>, public ConnectsTo<ParentBus> {
 public:
  void start_up() override;

  void create_child() {
    s_sequence.push_back(5);
    // Creating child requires the runtime to be alive.
    owning_bus().create_child("ChildBus", std::make_shared<ChildBus>());
    stop();
  }
};

class RuntimeRunner : public td::actor::Actor {
 public:
  RuntimeRunner(std::shared_ptr<td::Destructor> watcher) : watcher_(std::move(watcher)) {
  }

  void start_up() override {
    s_sequence.push_back(1);
    Runtime runtime;
    runtime.register_actor<BusActor>("BusActor");
    auto bus = std::make_shared<ParentBus>();
    bus->runner = actor_id(this);
    runtime.start(std::move(bus));
  }

  void ensure_destroyed(td::actor::ActorId<BusActor> back) {
    s_sequence.push_back(4);
    td::actor::send_closure(back, &BusActor::create_child);
  }

  void on_parent_bus_destruction() {
    s_sequence.push_back(7);
    stop();
  }

 private:
  std::shared_ptr<td::Destructor> watcher_;
};

ParentBus::~ParentBus() {
  s_sequence.push_back(6);
  td::actor::send_closure(runner, &RuntimeRunner::on_parent_bus_destruction);
}

void BusActor::start_up() {
  s_sequence.push_back(3);
  auto runner = owning_bus()->runner;
  td::actor::send_closure(runner, &RuntimeRunner::ensure_destroyed, actor_id(this));
}

TEST(Runtime, Lifetime) {
  td::actor::Scheduler scheduler({1});

  auto watcher = td::create_shared_destructor([] { td::actor::SchedulerContext::get().stop(); });
  scheduler.run_in_context([&] { td::actor::create_actor<RuntimeRunner>("runner", std::move(watcher)).release(); });
  scheduler.run();
  EXPECT_EQ(s_sequence, (std::vector{1, 2, 3, 4, 5, 6, 7}));
}

}  // namespace
}  // namespace ton::runtime::test_runtime_lifetime

namespace ton::runtime::test_requests {
namespace {

struct MainBus : Bus {
  MainBus() = default;

  ~MainBus() {
    td::actor::SchedulerContext::get().stop();
  }

  struct MultiplyBy25Request {
    using ReturnType = int;

    int value;
  };

  using Events = td::TypeList<MultiplyBy25Request>;
};

bool g_request_processed = false;
bool g_response_received = false;
bool g_observer_triggered = false;

class Provider : public SpawnsWith<MainBus>, public ConnectsTo<MainBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  td::actor::Task<int> process(BusHandle<MainBus> bus, std::shared_ptr<MainBus::MultiplyBy25Request> request) {
    co_await td::actor::coro_sleep(td::Timestamp::in(0.001));
    stop();
    g_request_processed = true;
    co_return request->value * 25;
  }
};

class Consumer : public SpawnsWith<MainBus>, public ConnectsTo<MainBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    run().start().detach();
  }

 private:
  td::actor::Task<td::Unit> run() {
    int result = co_await owning_bus().publish<MainBus::MultiplyBy25Request>(2);
    EXPECT_EQ(result, 50);
    g_response_received = true;
    stop();
    co_return td::Unit{};
  }
};

class Observer : public SpawnsWith<MainBus>, public ConnectsTo<MainBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle<MainBus> bus, std::shared_ptr<const MainBus::MultiplyBy25Request> request) {
    EXPECT(g_request_processed);
    g_observer_triggered = true;
    stop();
  }
};

static_assert(detail::CanActorHandleEvent<Observer, MainBus, MainBus::MultiplyBy25Request>);

TEST(Runtime, Requests) {
  td::actor::Scheduler scheduler({1});

  Runtime runtime;
  runtime.register_actor<Provider>("Provider");
  runtime.register_actor<Consumer>("Consumer");
  runtime.register_actor<Observer>("Observer");

  scheduler.run_in_context([&] { runtime.start(std::make_shared<MainBus>()); });
  scheduler.run();

  EXPECT(g_response_received);
  EXPECT(g_observer_triggered);
}

}  // namespace
}  // namespace ton::runtime::test_requests
