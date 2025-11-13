#pragma once

#include <concepts>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "td/actor/actor.h"
#include "td/utils/Badge.h"
#include "td/utils/buffer.h"
#include "td/utils/logging.h"

#include "TypeRegistry.h"
#include "TypeUtils.h"

namespace ton::runtime {

namespace detail {

class BusListeningActor;
class Runtime;

struct Bus {
  virtual ~Bus() = default;
};

template <typename T>
concept BusType = std::derived_from<T, Bus> && IsSpecializationOf<typename T::Events, TypeList>;

template <typename B>
concept BusWithParent = requires { typename B::Parent; };

template <typename E, typename B>
struct ValidEventHelper {
  static constexpr bool value = In<E, typename B::Events>;
};

template <typename E, BusWithParent B>
struct ValidEventHelper<E, B> {
  static constexpr bool value = ValidEventHelper<E, typename B::Parent>::value || In<E, typename B::Events>;
};

template <typename E, typename B>
concept ValidEventFor = ValidEventHelper<E, B>::value;

struct BusIdTag {};
using BusTypeId = ton::runtime::IdType<BusIdTag>;

template <BusType B>
BusTypeId get_bus_id() {
  return get_type_id<BusIdTag, B>();
}

template <BusType B>
class BusHandle;

template <BusType B>
class SpawnsWith;

template <typename E>
class ArmedEvent {
 public:
  explicit ArmedEvent(std::shared_ptr<E> event) : event_(std::move(event)) {
  }

  ArmedEvent(ArmedEvent&&) = default;
  ArmedEvent& operator=(ArmedEvent&&) = default;

  ~ArmedEvent() {
    LOG_CHECK(!event_) << "Event was lost because actor stopped too early. YOU HAVE A RACE.";
  }

  std::shared_ptr<E> disarm() {
    return std::move(event_);
  }

 private:
  std::shared_ptr<E> event_;
};

template <typename B, typename E>
class BusEventPublishImpl {
 public:
  size_t publish(std::shared_ptr<E> event, BusHandle<B> handle) {
    for (auto const& [actor, dispatcher_fn] : dispatchers) {
      td::actor::send_closure(actor, dispatcher_fn, handle, ArmedEvent(event));
    }
    return dispatchers.size();
  }

 private:
  friend class Runtime;

  struct EventDispatcher {
    using EventDispatcherFn = void (BusListeningActor::*)(BusHandle<B> bus, ArmedEvent<E> event);

    td::actor::ActorId<BusListeningActor> actor;
    EventDispatcherFn dispatcher_fn;
  };

  // Can only have actors that are owned by (non-strict) ancestor of the current bus.
  std::vector<EventDispatcher> dispatchers;
};

template <typename, typename>
struct BusPublishImpl;

template <BusType B>
using BusImpl = BusPublishImpl<B, typename B::Events>;

template <typename B, typename... Es>
struct BusPublishImpl<B, TypeList<Es...>> : BusEventPublishImpl<B, Es>... {
  using BusEventPublishImpl<B, Es>::publish...;
};

template <BusWithParent B, typename... Es>
struct BusPublishImpl<B, TypeList<Es...>> : BusImpl<typename B::Parent>, BusEventPublishImpl<B, Es>... {
  using BusImpl<typename B::Parent>::publish;
  using BusEventPublishImpl<B, Es>::publish...;
};

struct BusTreeNode {
  template <typename B>
  BusTreeNode(Runtime& runtime, std::string actor_name_prefix, std::shared_ptr<B> bus)
      : runtime(runtime)
      , actor_name_prefix(actor_name_prefix)
      , type_id(get_bus_id<B>())
      , bus(std::move(bus))
      , bus_impl(std::make_shared<BusImpl<B>>()) {
  }

  Runtime& runtime;
  std::string actor_name_prefix;
  BusTypeId type_id;
  std::shared_ptr<void> bus;
  std::shared_ptr<void> bus_impl;

  std::atomic<bool> is_stopping = false;

  std::shared_ptr<BusTreeNode> parent;
  std::vector<td::actor::ActorId<BusListeningActor>> owned_actors;
};

void log_published_event(BusTreeNode const& bus, std::type_info const& event_name);
void log_received_event(BusTreeNode const& bus, std::type_info const& event_name);

// A ref-counted nullable pointer of bus B.
template <BusType B>
class BusHandle {
 public:
  BusHandle() = default;
  BusHandle(BusHandle const&) = default;
  BusHandle(BusHandle&&) = default;
  BusHandle& operator=(BusHandle const&) = default;
  BusHandle& operator=(BusHandle&&) = default;

  BusHandle(std::nullptr_t) : BusHandle() {
  }

  template <BusType ChildB>
    requires std::derived_from<ChildB, B>
  BusHandle(BusHandle<ChildB> const& handle)
      : node_(handle.node_)
      , bus_(std::static_pointer_cast<B>(handle.bus_))
      , impl_(std::static_pointer_cast<BusImpl<B>>(handle.impl_)) {
  }

  // This is technically not constant but we give BusHandle const& to user code.
  template <ValidEventFor<B> E>
  size_t publish(std::shared_ptr<E> event) const {
    CHECK(*this);

    log_published_event(*node_, typeid(E));
    LOG_CHECK(!node_->is_stopping.load(std::memory_order_relaxed))
        << "Event was published to a degraded bus. THIS IS A BEST-EFFORT WARNING, YOU HAVE A RACE.";

    return impl_->publish(std::move(event), *this);
  }

  template <BusType Child>
  BusHandle<Child> create_child(std::string_view name, std::shared_ptr<Child> bus) const {
    CHECK(*this);

    auto name_prefix = node_->actor_name_prefix + std::string(name) + ".";
    auto child = std::make_shared<BusTreeNode>(node_->runtime, name_prefix, std::move(bus));
    child->parent = node_;
    child->type_id = get_bus_id<Child>();

    // Now, we just call `node_->runtime._wire_bus<B>({}, child);` but since Runtime is not defined
    // yet, we need a forward-declared method to do this for us.
    wire_bus(child);

    BusHandle<Child> child_handle(child, std::static_pointer_cast<Child>(child->bus),
                                  std::static_pointer_cast<BusImpl<Child>>(child->bus_impl));
    return child_handle;
  }

  void stop() const {
    CHECK(*this);
    // We let runtime do the work with (again, forward-declared) `node_->runtime._stop_bus(node_)`.
    stop_bus(node_);
  }

  template <BusType BNew>
    requires std::derived_from<BNew, B>
  BusHandle<BNew> unsafe_static_downcast_to() const {
    CHECK(*this);

    return BusHandle<BNew>(node_, std::static_pointer_cast<BNew>(bus_), std::static_pointer_cast<BusImpl<BNew>>(impl_));
  }

  B const* operator->() const& {
    CHECK(*this);

    return bus_.get();
  }

  explicit operator bool() const {
    return static_cast<bool>(bus_);
  }

  BusHandle(td::Badge<Runtime>, std::shared_ptr<BusTreeNode> node, std::shared_ptr<B> bus,
            std::shared_ptr<BusImpl<B>> impl)
      : node_(std::move(node)), bus_(std::move(bus)), impl_(std::move(impl)) {
  }

  template <OneOf<BusListeningActor, SpawnsWith<B>> T>
  BusTreeNode const& _node(td::Badge<T>) const {
    return *node_;
  }

 private:
  template <BusType Other>
  friend class BusHandle;

  BusHandle(std::shared_ptr<BusTreeNode> node, std::shared_ptr<B> bus, std::shared_ptr<BusImpl<B>> impl)
      : node_(std::move(node)), bus_(std::move(bus)), impl_(std::move(impl)) {
  }

  static void wire_bus(std::shared_ptr<BusTreeNode> node);
  static void stop_bus(std::shared_ptr<BusTreeNode> node);

  std::shared_ptr<BusTreeNode> node_;
  std::shared_ptr<B> bus_;
  std::shared_ptr<BusImpl<B>> impl_;
};

// Base class for all actors that handle bus events.
//
// Actor-type-erased `dispatch_event' method dispatches event to an actual handler. It is called
// from BusEventPublishImpl::publish and
//
// install_listeners_at implementation is provided by Runtime::BusListeningActorImpl<A>.
class BusListeningActor : public td::actor::Actor {
 private:
  friend class Runtime;

  template <typename A, typename B, typename BOrigin, typename E>
  void dispatch_event(BusHandle<BOrigin> bus, ArmedEvent<E> event) {
    log_received_event(bus._node(td::Badge<BusListeningActor>{}), typeid(E));
    if constexpr (!std::same_as<B, BOrigin>) {
      // When we install listeners, we guarantee that actual bus type is at most B.
      static_cast<A*>(this)->template handle<B, E>(bus.template unsafe_static_downcast_to<B>(), event.disarm());
    } else {
      static_cast<A*>(this)->template handle<B, E>(std::move(bus), event.disarm());
    }
  }

  void stop_with_bus() {
    this->td::actor::Actor::stop();
  }

  virtual void install_listeners_at(BusTypeId new_bus_type, std::shared_ptr<BusTreeNode> const& node,
                                    td::actor::ActorId<BusListeningActor> const& actor_id) = 0;

  td::BufferSlice name_;
};

// Base class for all user-created bus listeners. Template argument specifies which bus the actor
// spawns with. Note that you have to register actor using Runtime::register_actor for an actor to
// actually be created with the bus.
template <BusType B>
class SpawnsWith : public BusListeningActor {
 public:
  using SpawnWithBus = B;

  ~SpawnsWith() {
    LOG_CHECK(owning_bus_._node(td::Badge<SpawnsWith<B>>{}).is_stopping.load(std::memory_order_relaxed))
        << "Bus actors must not call stop() manually";
  }

 protected:
  BusHandle<B> const& owning_bus() {
    CHECK(owning_bus_);
    return owning_bus_;
  }

 private:
  friend class Runtime;

  BusHandle<B> owning_bus_;
};

// Bus listeners must also inherit ConnectsTo to specify a list of buses they subscribe to.
template <BusType... Bs>
struct ConnectsTo {
  using ConnectToBuses = TypeList<Bs...>;
};

template <typename T>
concept ActorType = std::derived_from<T, SpawnsWith<typename T::SpawnWithBus>> &&
                    IsSpecializationOf<typename T::ConnectToBuses, TypeList>;

template <typename A, typename B, typename E>
concept CanActorHandleEvent =
    requires(A& actor, BusHandle<B> bus, std::shared_ptr<E> event) { actor.template handle<B, E>(bus, event); };

class Runtime {
 public:
  template <ActorType A>
  void register_actor(std::string_view name) {
    LOG_CHECK(!started_) << "Actors can only be registered before starting runtime";

    using B = typename A::SpawnWithBus;
    BusTypeId spawn_bus_id = get_bus_id<B>();
    register_bus_parents<B>(spawn_bus_id);

    // Add actor to actors_to_spawn_for_ for bus to know which actors to spawn on creation.
    CreateInstanceFn create_instance_fn = &Runtime::create_actor_instance<A, B>;
    actors_to_spawn_for_[spawn_bus_id].push_back({
        .create_instance_fn = create_instance_fn,
        .name = std::string(name),
    });
  }

  template <BusType B>
  void set_root_bus(std::shared_ptr<B> bus) {
    LOG_CHECK(!started_) << "Root bus can only be set before starting runtime";

    root_bus_ = std::make_shared<BusTreeNode>(*this, "", std::move(bus));
  }

  void start() {
    LOG_CHECK(!started_) << "Runtime::start must not be called twice";
    LOG_CHECK(root_bus_) << "Root bus is not set";
    started_ = true;

    CHECK(root_bus_->bus_impl);
    wire_bus(std::move(root_bus_));
  }

  template <typename B>
  void _wire_bus(td::Badge<BusHandle<B>>, std::shared_ptr<BusTreeNode> node) {
    wire_bus(node);
  }

  template <typename B>
  void _stop_bus(td::Badge<BusHandle<B>>, std::shared_ptr<BusTreeNode> node) {
    node->is_stopping.store(true, std::memory_order_relaxed);
    for (auto const& owned_actor : node->owned_actors) {
      td::actor::send_closure(owned_actor, &BusListeningActor::stop_with_bus);
    }
  }

 private:
  bool started_ = false;

  std::shared_ptr<BusTreeNode> root_bus_;

  // ===== Bus inheritance =====
  template <typename B>
  void register_bus_parents(BusTypeId id) {
    if (bus_parents_.contains(id)) {
      return;
    }

    if constexpr (requires { typename B::Parent; }) {
      auto parent_id = get_bus_id<typename B::Parent>();
      bus_parents_[id] = parent_id;
      register_bus_parents<typename B::Parent>(parent_id);
    } else {
      bus_parents_[id] = std::nullopt;
    }
  }

  std::map<BusTypeId, std::optional<BusTypeId>> bus_parents_;

  // ===== Actor creation for a particular bus =====
  template <typename A>
  class BusListeningActorImpl final : public A {
   public:
    struct Params {
      BusListeningActorImpl& self;
      BusTypeId new_bus_type;
      std::shared_ptr<BusTreeNode> const& node;
      td::actor::ActorId<BusListeningActor> const& actor_id;
    };

    virtual void install_listeners_at(BusTypeId new_bus_type, std::shared_ptr<BusTreeNode> const& node,
                                      td::actor::ActorId<BusListeningActor> const& actor_id) override {
      // What we are doing with ActorRegistrar is effectively:
      // for (auto B : A::ConnectToBuses) {
      //   for (auto (BOrigin, E) : B::Events) {
      //     register_event_listener<B, BOrigin, E>(node);
      //   }
      // }
      Registrar<typename A::ConnectToBuses>{}({*this, new_bus_type, node, actor_id});
    }

    template <typename>
    struct Registrar {};

    template <typename... Bs>
    struct Registrar<TypeList<Bs...>> {
      void operator()(Params params) {
        (EventRegistrar<Bs, Bs, typename Bs::Events>{}(params), ...);
      }
    };

    template <typename, typename, typename>
    struct EventRegistrar {};

    template <typename B, typename BOrigin, typename... Es>
    struct EventRegistrar<B, BOrigin, TypeList<Es...>> {
      void operator()(Params params) {
        (params.self.template register_event_listener<B, BOrigin, Es>(params), ...);
        if constexpr (BusWithParent<BOrigin>) {
          using Parent = BOrigin::Parent;
          EventRegistrar<B, Parent, typename Parent::Events>{}(params);
        }
      }
    };

    template <typename B, typename BOrigin, typename E>
    void register_event_listener(Params params) {
      if constexpr (CanActorHandleEvent<A, B, E>) {
        // handle accepts (BusHandle<B>, std::shared_ptr<BOrigin::E>) while we treat newly installed
        // bus (of type BNew) as having type new_bus_type (one of types in its inheritance chain).
        if (get_bus_id<B>() == params.new_bus_type) {
          // node->bus_impl is BusImpl<BNew>. Since BNew <= B == new_bus_type, it is safe to
          // downcast node->bus_impl to BusImpl<B>.
          auto bus_impl =
              static_cast<BusEventPublishImpl<BOrigin, E>*>(static_cast<BusImpl<B>*>(params.node->bus_impl.get()));

          bus_impl->dispatchers.push_back({
              .actor = params.actor_id,
              .dispatcher_fn = &BusListeningActor::dispatch_event<A, B, BOrigin, E>,
          });
        }
      }
    }
  };

  using CreateInstanceFn = std::unique_ptr<BusListeningActor> (Runtime::*)(std::shared_ptr<BusTreeNode> node);

  struct ActorSpawnInfo {
    CreateInstanceFn create_instance_fn;
    std::string name;
  };

  template <typename A, typename B>
  std::unique_ptr<BusListeningActor> create_actor_instance(std::shared_ptr<BusTreeNode> node) {
    auto instance = std::make_unique<BusListeningActorImpl<A>>();
    auto bus = std::static_pointer_cast<B>(node->bus);
    auto bus_impl = std::static_pointer_cast<BusImpl<B>>(node->bus_impl);
    instance->owning_bus_ = BusHandle<B>({}, std::move(node), bus, bus_impl);
    return instance;
  }

  std::map<BusTypeId, std::vector<ActorSpawnInfo>> actors_to_spawn_for_;

  // ===== Event wiring =====
  void wire_bus(std::shared_ptr<BusTreeNode> node) {
    CHECK(node);

    // First, we create all actors that spawn on the added bus ...
    std::vector<std::unique_ptr<BusListeningActor>> spawned_actors;
    std::vector<BusTypeId> bus_inheritance_chain;

    for (std::optional bus_type = node->type_id; bus_type; bus_type = bus_parents_[*bus_type]) {
      bus_inheritance_chain.push_back(*bus_type);

      for (auto const& [create_instance_fn, base_name] : actors_to_spawn_for_[*bus_type]) {
        auto instance = (this->*create_instance_fn)(node);
        std::string name = node->actor_name_prefix + base_name;
        instance->name_ = td::BufferSlice(name.data(), name.size());
        auto actor_info =
            td::actor::detail::create_actor_info(td::actor::ActorOptions{}.with_name(name), std::move(instance));
        node->owned_actors.push_back(td::actor::ActorId<BusListeningActor>::unsafe_create_from_info(actor_info));
      }
    }

    // Then, we wire events from the bus to all actors that are subscribed to it. Note that we have
    // not returned BusHandle of a newly created bus to outside world yet, so the outside world
    // cannot publish events on the bus, and actors that have the current bus injected as a owning
    // bus are not started yet.
    auto it = node;
    while (it) {
      LOG_CHECK(!it->is_stopping.load(std::memory_order_relaxed))
          << "Race between child and parent buses detected. THIS IS A BEST-EFFORT WARNING, YOU HAVE A RACE.";

      for (auto const& owned_actor : it->owned_actors) {
        for (auto new_bus_type : bus_inheritance_chain) {
          owned_actor.get_actor_unsafe().install_listeners_at(new_bus_type, node, owned_actor);
        }
      }
      it = it->parent;
    }

    // And lastly, we start all newly created actors
    for (auto const& actor : node->owned_actors) {
      td::actor::detail::register_actor_info_ptr(actor.actor_info_ptr());
    }
  }
};

template <BusType B>
void BusHandle<B>::wire_bus(std::shared_ptr<BusTreeNode> node) {
  node->runtime._wire_bus<B>({}, std::move(node));
}

template <BusType B>
void BusHandle<B>::stop_bus(std::shared_ptr<BusTreeNode> node) {
  node->runtime._stop_bus<B>({}, std::move(node));
}

}  // namespace detail

using detail::Bus;
using detail::BusHandle;
using detail::BusType;
using detail::ConnectsTo;
using detail::Runtime;
using detail::SpawnsWith;

#define TON_RUNTIME_DEFINE_EVENT_HANDLER()                    \
  template <::ton::runtime::In<ConnectToBuses> B, typename E> \
    requires ::ton::runtime::detail::ValidEventFor<E, B>      \
  constexpr void handle(::ton::runtime::BusHandle<B> bus, ::std::shared_ptr<E> event) = delete;

}  // namespace ton::runtime