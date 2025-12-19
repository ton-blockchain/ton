#pragma once

#include <concepts>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro_utils.h"
#include "td/utils/Badge.h"
#include "td/utils/TypeRegistry.h"
#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/type_traits.h"

namespace ton::runtime {

namespace detail {

class ListenerInstaller;
class BusListeningActor;
class Runtime;

struct Bus {
  virtual ~Bus() = default;
};

template <typename T>
concept BusType = std::derived_from<T, Bus> && td::IsSpecializationOf<typename T::Events, td::TypeList>;

template <typename B>
concept BusWithParent = requires { typename B::Parent; };

template <typename E, typename B>
struct ValidPublishTargetHelper {
  static constexpr bool value = td::In<E, typename B::Events>;
};

template <typename E, BusWithParent B>
struct ValidPublishTargetHelper<E, B> {
  static constexpr bool value = ValidPublishTargetHelper<E, typename B::Parent>::value || td::In<E, typename B::Events>;
};

template <typename E, typename B>
concept ValidPublishTargetFor = ValidPublishTargetHelper<E, B>::value;

template <typename E, typename B>
concept ValidEventFor = ValidPublishTargetFor<E, B> && !requires { typename E::ReturnType; };

template <typename E, typename B>
concept ValidRequestFor = ValidPublishTargetFor<E, B> && requires { typename E::ReturnType; };

struct BusIdTag {};
using BusTypeId = td::IdType<BusIdTag>;

template <BusType B>
BusTypeId get_bus_id() {
  return td::get_type_id<BusIdTag, B>();
}

template <BusType B>
class BusHandle;

template <BusType B>
class SpawnsWith;

template <typename B, typename E>
class BusEventPublishImplBase {
 public:
  struct EventDispatcher {
    using EventDispatcherFn = void (BusListeningActor::*)(BusHandle<B> bus, std::shared_ptr<const E> event);

    td::actor::ActorId<BusListeningActor> actor;
    EventDispatcherFn dispatcher_fn;
  };

  void publish(std::shared_ptr<E> event, BusHandle<B> handle) {
    for (const auto& [actor, dispatcher_fn] : dispatchers) {
      td::actor::send_closure(actor, dispatcher_fn, handle, event);
    }
  }

  void add_handler(EventDispatcher dispatcher) {
    dispatchers.push_back(std::move(dispatcher));
  }

 private:
  friend class Runtime;

  // Can only have actors that are owned by (non-strict) predecessor of the current bus.
  std::vector<EventDispatcher> dispatchers;
};

template <typename B, typename E>
class BusEventPublishImpl : public BusEventPublishImplBase<B, E> {};

template <typename B, ValidRequestFor<B> E>
class BusEventPublishImpl<B, E> : public BusEventPublishImplBase<B, E> {
 public:
  auto publish(std::shared_ptr<E> event, BusHandle<B> handle) {
    CHECK(dispatcher_fn != nullptr);
    return td::actor::ask(actor, dispatcher_fn, handle, event).then([this, event, handle](auto&& result) {
      static_cast<BusEventPublishImplBase<B, E>>(*this).publish(event, handle);
      return result;
    });
  }

 private:
  friend class Runtime;

  using EventDispatcherFn = td::actor::Task<typename E::ReturnType> (BusListeningActor::*)(BusHandle<B> bus,
                                                                                           std::shared_ptr<E> event);

  td::actor::ActorId<BusListeningActor> actor;
  EventDispatcherFn dispatcher_fn = nullptr;
};

template <typename, typename>
struct BusPublishImpl;

template <BusType B>
using BusImpl = BusPublishImpl<B, typename B::Events>;

template <typename B, typename... Es>
struct BusPublishImpl<B, td::TypeList<Es...>> : BusEventPublishImpl<B, Es>... {
  using BusEventPublishImpl<B, Es>::publish...;
};

template <BusWithParent B, typename... Es>
struct BusPublishImpl<B, td::TypeList<Es...>> : BusImpl<typename B::Parent>, BusEventPublishImpl<B, Es>... {
  using BusImpl<typename B::Parent>::publish;
  using BusEventPublishImpl<B, Es>::publish...;
};

struct BusTreeNode {
  struct OwnedActor {
    td::actor::ActorId<BusListeningActor> id;
    std::weak_ptr<ListenerInstaller> installer;
  };

  template <typename B>
  BusTreeNode(std::shared_ptr<Runtime> runtime, std::string actor_name_prefix, std::shared_ptr<B> bus)
      : runtime(std::move(runtime))
      , actor_name_prefix(actor_name_prefix)
      , type_id(get_bus_id<B>())
      , bus(std::move(bus))
      , bus_impl(std::make_shared<BusImpl<B>>()) {
  }

  std::shared_ptr<Runtime> runtime;
  std::string actor_name_prefix;
  BusTypeId type_id;
  std::shared_ptr<void> bus;
  std::shared_ptr<void> bus_impl;

  std::shared_ptr<BusTreeNode> parent;
  std::vector<OwnedActor> owned_actors;
};

template <typename E>
void log_event(bool published, const BusTreeNode& bus, const E& event) {
  std::string contents;
  if constexpr (requires {
                  { event.contents_to_string() } -> std::same_as<std::string>;
                }) {
    if (published) {
      contents = event.contents_to_string();
    }
  }
  std::string_view bus_name = bus.actor_name_prefix;
  if (bus_name.ends_with(".")) {
    bus_name = bus_name.substr(0, bus_name.size() - 1);
  } else if (bus_name == "") {
    bus_name = "root";
  }

  auto type_name = td::actor::core::ActorTypeStatManager::get_class_name(typeid(event).name());
  size_t last_colon = type_name.rfind("::");
  if (last_colon != std::string::npos) {
    type_name = type_name.substr(0, last_colon + 2) + TC_YELLOW + type_name.substr(last_colon + 2) + TC_CYAN;
  }

  LOG(INFO) << (published ? "Published event " : "Received event ") << type_name << "@" << &event << "\x1b[90m"
            << contents << TC_CYAN << " on " << td::Slice(bus_name.data(), bus_name.size()) << " bus";
}

// A ref-counted nullable pointer of bus B.
template <BusType B>
class BusHandle {
 public:
  BusHandle() = default;
  BusHandle(const BusHandle&) = default;
  BusHandle(BusHandle&&) = default;
  BusHandle& operator=(const BusHandle&) = default;
  BusHandle& operator=(BusHandle&&) = default;

  BusHandle(std::nullptr_t) : BusHandle() {
  }

  template <BusType ChildB>
    requires std::derived_from<ChildB, B>
  BusHandle(const BusHandle<ChildB>& handle)
      : node_(handle.node_)
      , bus_(std::static_pointer_cast<B>(handle.bus_))
      , impl_(std::static_pointer_cast<BusImpl<B>>(handle.impl_)) {
  }

  // publish is technically not constant but we give BusHandle const& to user code.
  template <ValidPublishTargetFor<B> E>
  [[nodiscard]] auto publish(std::shared_ptr<E> event) const {
    CHECK(*this);

    log_event(true, *node_, *event);
    return impl_->publish(std::move(event), *this);
  }

  template <ValidPublishTargetFor<B> E, typename... Args>
    requires std::constructible_from<E, Args...>
  [[nodiscard]] auto publish(Args&&... args) const {
    return publish<E>(std::make_shared<E>(std::forward<Args>(args)...));
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

  template <BusType BNew>
    requires std::derived_from<BNew, B>
  BusHandle<BNew> unsafe_static_downcast_to() const {
    CHECK(*this);

    return BusHandle<BNew>(node_, std::static_pointer_cast<BNew>(bus_), std::static_pointer_cast<BusImpl<BNew>>(impl_));
  }

  const B& operator*() const& {
    CHECK(*this);

    return *bus_;
  }

  const B* operator->() const& {
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

  template <td::OneOf<BusListeningActor, SpawnsWith<B>> T>
  const BusTreeNode& _node(td::Badge<T>) const {
    return *node_;
  }

 private:
  template <BusType Other>
  friend class BusHandle;

  BusHandle(std::shared_ptr<BusTreeNode> node, std::shared_ptr<B> bus, std::shared_ptr<BusImpl<B>> impl)
      : node_(std::move(node)), bus_(std::move(bus)), impl_(std::move(impl)) {
  }

  static void wire_bus(std::shared_ptr<BusTreeNode> node);

  std::shared_ptr<BusTreeNode> node_;
  std::shared_ptr<B> bus_;
  std::shared_ptr<BusImpl<B>> impl_;
};

class ListenerInstaller {
 public:
  virtual ~ListenerInstaller() = default;

  virtual void install_listeners_at(BusTypeId new_bus_type, const std::shared_ptr<BusTreeNode>& node,
                                    const td::actor::ActorId<BusListeningActor>& actor_id) = 0;
};

// Base class for all actors that handle bus events.
//
// Actor-type-erased `dispatch_event' method dispatches event to an actual handler. It is called
// from BusEventPublishImpl::publish.
class BusListeningActor : public td::actor::Actor {
 private:
  friend class Runtime;

  template <typename A, typename B, typename BOrigin, typename E>
  void dispatch_event(BusHandle<BOrigin> bus, std::shared_ptr<const E> event) {
    log_event(false, bus._node(td::Badge<BusListeningActor>{}), *event);
    if constexpr (!std::same_as<B, BOrigin>) {
      // When we install listeners, we guarantee that the actual bus type is at most B.
      static_cast<A*>(this)->template handle<B, E>(bus.template unsafe_static_downcast_to<B>(), std::move(event));
    } else {
      static_cast<A*>(this)->template handle<B, E>(std::move(bus), std::move(event));
    }
  }

  template <typename A, typename B, typename BOrigin, typename E>
  auto process_event(BusHandle<BOrigin> bus, std::shared_ptr<E> event) {
    log_event(false, bus._node(td::Badge<BusListeningActor>{}), *event);
    if constexpr (!std::same_as<B, BOrigin>) {
      return static_cast<A*>(this)->template process<B, E>(bus.template unsafe_static_downcast_to<B>(),
                                                           std::move(event));
    } else {
      return static_cast<A*>(this)->template process<B, E>(std::move(bus), std::move(event));
    }
  }

  std::shared_ptr<ListenerInstaller> installer_;
};

// Base class for all user-created bus listeners. Template argument specifies which bus the actor
// spawns with. Note that you have to register actor using Runtime::register_actor for an actor to
// actually be created with the bus.
template <BusType B>
class SpawnsWith : public BusListeningActor {
 public:
  using SpawnWithBus = B;

 protected:
  const BusHandle<B>& owning_bus() {
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
  using ConnectToBuses = td::TypeList<Bs...>;
};

template <typename T>
concept ActorType = std::derived_from<T, SpawnsWith<typename T::SpawnWithBus>> &&
                    td::IsSpecializationOf<typename T::ConnectToBuses, td::TypeList>;

template <typename A, typename B, typename E>
concept CanActorHandleEvent =
    requires(A& actor, BusHandle<B> bus, std::shared_ptr<const E> event) { actor.template handle<B, E>(bus, event); };

template <typename A, typename B, typename E>
concept CanActorProcessEvent =
    requires(A& actor, BusHandle<B> bus, std::shared_ptr<E> event) { actor.template process<B, E>(bus, event); };

class Runtime : public std::enable_shared_from_this<Runtime> {
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
  BusHandle<B> start(std::shared_ptr<B> bus, std::string_view name) {
    LOG_CHECK(!started_) << "Runtime::start must not be called twice";
    started_ = true;

    auto root_bus = std::make_shared<BusTreeNode>(shared_from_this(), name.empty() ? "" : std::string(name) + ".", bus);
    wire_bus(root_bus);
    auto bus_impl = std::static_pointer_cast<BusImpl<B>>(root_bus->bus_impl);

    return BusHandle<B>{{}, std::move(root_bus), std::move(bus), std::move(bus_impl)};
  }

  template <typename B>
  void _wire_bus(td::Badge<BusHandle<B>>, std::shared_ptr<BusTreeNode> node) {
    wire_bus(node);
  }

 private:
  bool started_ = false;

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
  class ListenerInstallerImpl final : public ListenerInstaller {
   public:
    struct Params {
      ListenerInstallerImpl& self;
      BusTypeId new_bus_type;
      const std::shared_ptr<BusTreeNode>& node;
      const td::actor::ActorId<BusListeningActor>& actor_id;
    };

    virtual void install_listeners_at(BusTypeId new_bus_type, const std::shared_ptr<BusTreeNode>& node,
                                      const td::actor::ActorId<BusListeningActor>& actor_id) override {
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
    struct Registrar<td::TypeList<Bs...>> {
      void operator()(Params params) {
        (EventRegistrar<Bs, Bs, typename Bs::Events>{}(params), ...);
      }
    };

    template <typename, typename, typename>
    struct EventRegistrar {};

    template <typename B, typename BOrigin, typename... Es>
    struct EventRegistrar<B, BOrigin, td::TypeList<Es...>> {
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
      if (get_bus_id<B>() == params.new_bus_type) {
        // node->bus_impl is BusImpl<BNew>. Since BNew <= B == new_bus_type, it is safe to
        // downcast node->bus_impl to BusImpl<B>.
        auto bus_impl =
            static_cast<BusEventPublishImpl<BOrigin, E>*>(static_cast<BusImpl<B>*>(params.node->bus_impl.get()));

        if constexpr (CanActorHandleEvent<A, B, E>) {
          // handle accepts (BusHandle<B>, std::shared_ptr<BOrigin::E>) while we treat newly installed
          // bus (of type BNew) as having type new_bus_type (one of types in its inheritance chain).
          bus_impl->add_handler({
              .actor = params.actor_id,
              .dispatcher_fn = &BusListeningActor::dispatch_event<A, B, BOrigin, E>,
          });
        }
        if constexpr (CanActorProcessEvent<A, B, E>) {
          CHECK(bus_impl->dispatcher_fn == nullptr);
          bus_impl->actor = params.actor_id;
          bus_impl->dispatcher_fn = &BusListeningActor::process_event<A, B, BOrigin, E>;
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
    auto installer = std::make_shared<ListenerInstallerImpl<A>>();
    auto instance = std::make_unique<A>();
    auto bus = std::static_pointer_cast<B>(node->bus);
    auto bus_impl = std::static_pointer_cast<BusImpl<B>>(node->bus_impl);
    instance->owning_bus_ = BusHandle<B>({}, std::move(node), bus, bus_impl);
    instance->installer_ = installer;
    return instance;
  }

  std::map<BusTypeId, std::vector<ActorSpawnInfo>> actors_to_spawn_for_;

  // ===== Event wiring =====
  void wire_bus(std::shared_ptr<BusTreeNode> node) {
    CHECK(node);

    // First, we create all actors that spawn on the added bus.
    std::vector<std::unique_ptr<BusListeningActor>> spawned_actors;
    std::vector<BusTypeId> bus_inheritance_chain;

    for (std::optional bus_type = node->type_id; bus_type; bus_type = bus_parents_[*bus_type]) {
      bus_inheritance_chain.push_back(*bus_type);

      for (const auto& [create_instance_fn, base_name] : actors_to_spawn_for_[*bus_type]) {
        auto instance = (this->*create_instance_fn)(node);
        auto installer = instance->installer_;
        std::string name = node->actor_name_prefix + base_name;
        auto actor_info =
            td::actor::detail::create_actor_info(td::actor::ActorOptions{}.with_name(name), std::move(instance));
        node->owned_actors.push_back({
            .id = td::actor::ActorId<BusListeningActor>::unsafe_create_from_info(actor_info),
            .installer = installer,
        });
      }
    }

    // Then, we wire events from the bus to all actors that are subscribed to it. Note that while
    // actors on the spawned bus cannot be stopped, parent buses can concurrently stop their actors.
    auto it = node;
    while (it) {
      for (const auto& [id, installer] : it->owned_actors) {
        for (auto new_bus_type : bus_inheritance_chain) {
          if (auto locked_installer = installer.lock()) {
            locked_installer->install_listeners_at(new_bus_type, node, id);
          }
        }
      }
      it = it->parent;
    }

    // And lastly, we start all newly created actors
    for (const auto& [id, _] : node->owned_actors) {
      td::actor::detail::register_actor_info_ptr(id.actor_info_ptr());
    }
  }
};

template <BusType B>
void BusHandle<B>::wire_bus(std::shared_ptr<BusTreeNode> node) {
  node->runtime->_wire_bus<B>({}, std::move(node));
}

}  // namespace detail

using detail::Bus;
using detail::BusHandle;
using detail::BusType;
using detail::ConnectsTo;
using detail::SpawnsWith;

class Runtime {
 public:
  template <detail::ActorType A>
  void register_actor(std::string_view name) {
    impl_->template register_actor<A>(name);
  }

  template <BusType B>
  BusHandle<B> start(std::shared_ptr<B> bus, std::string_view name = "") {
    return impl_->template start<B>(std::move(bus), name);
  }

 private:
  std::shared_ptr<detail::Runtime> impl_ = std::make_shared<detail::Runtime>();
};

#define TON_RUNTIME_DEFINE_EVENT_HANDLER()                                                            \
  template <::td::In<ConnectToBuses> B, ::ton::runtime::detail::ValidPublishTargetFor<B> E>           \
  constexpr void handle(::ton::runtime::BusHandle<B> bus, ::std::shared_ptr<E const> event) = delete; \
                                                                                                      \
  template <::td::In<ConnectToBuses> B, ::ton::runtime::detail::ValidRequestFor<B> E>                 \
  constexpr td::actor::Task<typename E::ReturnType> process(::ton::runtime::BusHandle<B> bus,         \
                                                            ::std::shared_ptr<E> request) = delete;

}  // namespace ton::runtime
