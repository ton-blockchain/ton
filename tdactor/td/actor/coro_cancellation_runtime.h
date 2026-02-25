#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "td/actor/coro_ref.h"
#include "td/utils/List.h"
#include "td/utils/Mutex.h"
#include "td/utils/common.h"

namespace td {
template <class T>
class Promise;
}  // namespace td

namespace td::actor {

struct CancellationRuntime;
struct CancelNode;

namespace detail {
struct TaskControlBase;
}  // namespace detail

// Bridge: operations CancellationRuntime needs on TaskControlBase.
// Breaks the circular dependency (TaskControlBase contains CancellationRuntime).
// All functions are defined in coro_task.h after TaskControlBase is complete.
namespace bridge {
CancellationRuntime& runtime(detail::TaskControlBase& self);
const CancellationRuntime& runtime(const detail::TaskControlBase& self);
void complete_scheduled(detail::TaskControlBase& self);
bool should_finish_due_to_cancellation(const detail::TaskControlBase& self);
}  // namespace bridge

class ParentScopeLease;

namespace bridge {
ParentScopeLease make_parent_scope_lease(detail::TaskControlBase& ctrl);
}  // namespace bridge

class ParentScopeLease {
 public:
  ParentScopeLease() = default;

  explicit operator bool() const {
    return bool(ptr_);
  }
  bool is_cancelled() const;

  void publish_heap_cancel_node(CancelNode& node);
  void publish_cancel_promise(td::Promise<td::Unit> p);
  ParentScopeLease copy() const;

 private:
  friend struct CancellationRuntime;
  friend struct ParentLink;
  friend ParentScopeLease bridge::make_parent_scope_lease(detail::TaskControlBase&);

  struct Deleter {
    void operator()(detail::TaskControlBase* p) const;
  };
  std::unique_ptr<detail::TaskControlBase, Deleter> ptr_;

  explicit ParentScopeLease(detail::TaskControlBase* p) : ptr_(p) {
  }
  detail::TaskControlBase* release() {
    return ptr_.release();
  }
  detail::TaskControlBase* get() const {
    return ptr_.get();
  }
};

// Pure state machine: cancel flag + child count + waiting flag.
struct CancellationState {
  static constexpr uint32_t CANCELLED = 1u << 31;
  static constexpr uint32_t WAITING = 1u << 30;
  static constexpr uint32_t IGNORED = 1u << 29;
  static constexpr uint32_t COUNT_MASK = IGNORED - 1;

  bool is_cancelled() const {
    return state_.load(std::memory_order_acquire) & CANCELLED;
  }

  bool is_effectively_cancelled() const {
    auto s = state_.load(std::memory_order_acquire);
    return (s & CANCELLED) && !(s & IGNORED);
  }

  bool is_effectively_cancelled_seq_cst() const {
    auto s = state_.load(std::memory_order_seq_cst);
    return (s & CANCELLED) && !(s & IGNORED);
  }

  // Returns previous bits — caller checks for CANCELLED/IGNORED.
  uint32_t set_cancelled() {
    return state_.fetch_or(CANCELLED, std::memory_order_seq_cst);
  }

  // Atomically set IGNORED bit, unless CANCELLED is set without IGNORED.
  // Returns true if IGNORED was set, false if CANCELLED won the race.
  bool try_set_ignored() {
    uint32_t old = state_.load(std::memory_order_seq_cst);
    while (true) {
      if ((old & CANCELLED) && !(old & IGNORED)) {
        return false;
      }
      if (state_.compare_exchange_weak(old, old | IGNORED, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
        return true;
      }
    }
  }

  uint32_t clear_ignored() {
    return state_.fetch_and(~IGNORED, std::memory_order_seq_cst);
  }

  void add_child_ref() {
    state_.fetch_add(1, std::memory_order_relaxed);
  }

  bool release_child_ref() {
    auto prev = state_.fetch_sub(1, std::memory_order_acq_rel);
    return (prev & (WAITING | COUNT_MASK)) == (WAITING | 1u);
  }

  bool try_wait_for_children() {
    auto prev = state_.fetch_or(WAITING, std::memory_order_acq_rel);
    return (prev & COUNT_MASK) != 0;
  }

  uint32_t child_count_relaxed_for_test() const {
    return state_.load(std::memory_order_relaxed) & COUNT_MASK;
  }

 private:
  std::atomic<uint32_t> state_{0};
};

struct ParentLink {
 public:
  ~ParentLink() {
    DCHECK(parent_.load(std::memory_order_relaxed) == nullptr);
  }

  enum class ReleaseReason : uint8_t { ChildCompleted, Teardown };

  void link_from_parent_scope_lease(CancelNode& self, ParentScopeLease parent_scope_ref);
  detail::TaskControlBase* detach_for_child_completed(CancelNode& self);
  void release(CancelNode& self, ReleaseReason reason);
  bool has_parent() const {
    return parent_.load(std::memory_order_acquire) != nullptr;
  }
  bool is_parent(const detail::TaskControlBase* parent) const {
    return parent_.load(std::memory_order_acquire) == parent;
  }

 private:
  std::atomic<detail::TaskControlBase*> parent_{nullptr};
};

struct TopologyTag {};
struct ActorCancelTag {};

// Lifecycle:
//   Creator holds initial ref (refs_=1).
//   on_publish() adds topology ref.
//   on_cleanup() drops topology ref.
//   on_zero_refs() destroys the node.
struct CancelNode : td::TaggedListNode<TopologyTag>, td::TaggedListNode<ActorCancelTag> {
  friend struct CancellationRuntime;
  friend struct ParentLink;
  template <class T>
  friend struct Ref;
  template <class Tag>
  friend struct CancelTopology;

  virtual ~CancelNode() {
    DCHECK(static_cast<td::TaggedListNode<TopologyTag>*>(this)->empty());
    DCHECK(static_cast<td::TaggedListNode<ActorCancelTag>*>(this)->empty());
  }
  virtual void on_cancel() {
  }
  virtual void on_cleanup() {
  }
  virtual void on_publish() {
  }

 protected:
  explicit CancelNode(uint32_t initial_refs = 1) : refs_(initial_refs) {
    CHECK(initial_refs > 0);
  }
  void add_ref() {
    refs_.fetch_add(1, std::memory_order_relaxed);
  }
  void dec_ref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      on_zero_refs();
    }
  }
  virtual void on_zero_refs() = 0;

 private:
  std::atomic<uint32_t> refs_;
};

struct HeapCancelNode : CancelNode {
  bool disarm() {
    return armed_.exchange(false, std::memory_order_acq_rel);
  }

  void on_cancel() final {
    if (disarm()) {
      do_cancel();
    }
  }
  void on_cleanup() final {
    if (disarm()) {
      do_cleanup();
    }
    dec_ref();
  }
  void on_publish() final {
    add_ref();
  }

  virtual void do_cancel() = 0;
  virtual void do_cleanup() {
  }

 protected:
  explicit HeapCancelNode(uint32_t initial_refs = 1) : CancelNode(initial_refs) {
  }
  void on_zero_refs() override {
    delete this;
  }

 private:
  std::atomic<bool> armed_{true};
};

template <class Tag>
struct CancelTopology {
  enum class PublishResult : uint8_t { Published, AlreadyInList };

  PublishResult publish_raw(CancelNode& node) {
    auto& hook = static_cast<td::TaggedListNode<Tag>&>(node);
    std::lock_guard<td::TinyMutex> lock(mutex_);
    if (!hook.empty())
      return PublishResult::AlreadyInList;
    node.on_publish();
    head_.put(&hook);
    return PublishResult::Published;
  }

  bool unpublish_raw(CancelNode& node) {
    auto& hook = static_cast<td::TaggedListNode<Tag>&>(node);
    std::lock_guard<td::TinyMutex> lock(mutex_);
    if (hook.empty())
      return false;
    hook.remove();
    return true;
  }

  template <class HoldFn>
  void snapshot(std::vector<CancelNode*>& out, HoldFn&& hold_fn) {
    std::lock_guard<td::TinyMutex> lock(mutex_);
    for (auto* it = head_.begin(); it != head_.end(); it = it->get_next()) {
      auto* node = static_cast<CancelNode*>(it);
      hold_fn(*node);
      out.push_back(node);
    }
  }

  template <class HoldFn>
  void drain(std::vector<CancelNode*>& out, HoldFn&& hold_fn) {
    std::lock_guard<td::TinyMutex> lock(mutex_);
    for (auto* it = head_.begin(); it != head_.end();) {
      auto* next = it->get_next();
      auto* node = static_cast<CancelNode*>(it);
      hold_fn(*node);
      it->remove();
      out.push_back(node);
      it = next;
    }
  }

  template <class IsCancelledFn>
  bool publish_and_maybe_cancel(CancelNode& node, IsCancelledFn&& is_cancelled) {
    if (publish_raw(node) != PublishResult::Published) {
      return false;
    }
    if (is_cancelled()) {
      node.on_cancel();
    }
    return true;
  }

  bool unpublish_and_cleanup(CancelNode& node) {
    if (!unpublish_raw(node)) {
      return false;
    }
    node.on_cleanup();
    return true;
  }

  template <class CollectFn, class ActionFn>
  void batch(CollectFn&& collect_fn, ActionFn&& action_fn) {
    std::vector<CancelNode*> nodes;
    collect_fn(nodes, [](CancelNode& node) { node.add_ref(); });
    for (auto* node : nodes) {
      action_fn(*node);
      node->dec_ref();
    }
  }

  void cancel_snapshot() {
    batch([&](auto& out, auto fn) { snapshot(out, fn); }, [](CancelNode& n) { n.on_cancel(); });
  }

  void drain_cleanup() {
    batch([&](auto& out, auto fn) { drain(out, fn); }, [](CancelNode& n) { n.on_cleanup(); });
  }

  bool empty() {
    std::lock_guard<td::TinyMutex> lock(mutex_);
    return head_.begin() == head_.end();
  }

 private:
  td::TaggedListNode<Tag> head_;
  td::TinyMutex mutex_;
};

struct CancellationRuntime {
  enum class ChildReleasePolicy : uint8_t { MayComplete, NoComplete };

  bool is_cancelled() const {
    return state_.is_cancelled();
  }

  bool should_finish_due_to_cancellation() const {
    return state_.is_effectively_cancelled();
  }

  void cancel(CancelNode& self) {
    auto prev = state_.set_cancelled();
    if (prev & CancellationState::CANCELLED)
      return;
    if (prev & CancellationState::IGNORED)
      return;
    cancel_topology_with_self_ref(self);
  }

  bool try_wait_for_children() {
    return state_.try_wait_for_children();
  }

  void add_child_ref() {
    state_.add_child_ref();
  }

  void release_child_ref(detail::TaskControlBase& self, ChildReleasePolicy policy) {
    if (state_.release_child_ref() && policy == ChildReleasePolicy::MayComplete) {
      bridge::complete_scheduled(self);
    }
  }

  uint32_t child_count_relaxed_for_test() const {
    return state_.child_count_relaxed_for_test();
  }

  bool has_parent_scope() const {
    return parent_link_.has_parent();
  }

  bool is_parent(const detail::TaskControlBase* parent) const {
    return parent_link_.is_parent(parent);
  }

  void notify_parent_child_completed(CancelNode& self) {
    parent_link_.release(self, ParentLink::ReleaseReason::ChildCompleted);
  }

  detail::TaskControlBase* take_parent_for_child_completed(CancelNode& self) {
    return parent_link_.detach_for_child_completed(self);
  }

  void set_parent_lease(CancelNode& self, ParentScopeLease parent_scope_ref) {
    parent_link_.link_from_parent_scope_lease(self, std::move(parent_scope_ref));
  }

  void unpublish_cancel_node(CancelNode& node) {
    topology_.unpublish_and_cleanup(node);
  }

  void publish_cancel_node(CancelNode& node) {
    topology_.publish_and_maybe_cancel(node, [&] { return state_.is_effectively_cancelled_seq_cst(); });
  }

  bool try_enter_ignore() {
    DCHECK(ignored_depth_ < UINT32_MAX);
    if (ignored_depth_++ > 0)
      return true;
    if (state_.try_set_ignored())
      return true;
    --ignored_depth_;
    return false;
  }

  void leave_ignore(CancelNode& self) {
    DCHECK(ignored_depth_ > 0);
    if (--ignored_depth_ > 0)
      return;
    auto old = state_.clear_ignored();
    if (old & CancellationState::CANCELLED) {
      cancel_topology_with_self_ref(self);
    }
  }

  void drain_topology() {
    topology_.drain_cleanup();
  }

  void on_last_ref_teardown(CancelNode& self) {
    parent_link_.release(self, ParentLink::ReleaseReason::Teardown);
    drain_topology();
  }

  bool is_cancel_topology_traversal_active() const {
    return cancel_traversal_depth_.load(std::memory_order_acquire) != 0;
  }

  bool has_published_cancel_nodes() {
    return !topology_.empty();
  }

 private:
  void cancel_topology_with_self_ref(CancelNode& self) {
    cancel_traversal_depth_.fetch_add(1, std::memory_order_acq_rel);
    self.add_ref();
    topology_.cancel_snapshot();
    self.dec_ref();
    cancel_traversal_depth_.fetch_sub(1, std::memory_order_acq_rel);
  }

  uint32_t ignored_depth_{0};
  std::atomic<uint32_t> cancel_traversal_depth_{0};
  CancellationState state_{};
  CancelTopology<TopologyTag> topology_{};
  ParentLink parent_link_{};
};

inline void ParentLink::link_from_parent_scope_lease(CancelNode& self, ParentScopeLease parent_scope_ref) {
  if (!parent_scope_ref) {
    return;
  }
  parent_scope_ref.publish_heap_cancel_node(self);
  auto* transferred_parent = parent_scope_ref.release();
  auto* expected = static_cast<detail::TaskControlBase*>(nullptr);
  CHECK(parent_.compare_exchange_strong(expected, transferred_parent, std::memory_order_release,
                                        std::memory_order_relaxed));
}

inline detail::TaskControlBase* ParentLink::detach_for_child_completed(CancelNode& self) {
  auto* parent = parent_.load(std::memory_order_acquire);
  if (!parent) {
    return nullptr;
  }
  // Unpublish can drop topology ref; keep the node alive through unpublish.
  self.add_ref();
  bridge::runtime(*parent).unpublish_cancel_node(self);
  auto* result = parent_.exchange(nullptr, std::memory_order_acq_rel);
  self.dec_ref();
  return result;
}

inline void ParentLink::release(CancelNode& self, ReleaseReason reason) {
  detail::TaskControlBase* parent;
  if (reason == ReleaseReason::ChildCompleted) {
    parent = detach_for_child_completed(self);
  } else {
    parent = parent_.exchange(nullptr, std::memory_order_acq_rel);
  }
  if (!parent) {
    return;
  }
  auto policy = reason == ReleaseReason::ChildCompleted ? CancellationRuntime::ChildReleasePolicy::MayComplete
                                                        : CancellationRuntime::ChildReleasePolicy::NoComplete;
  bridge::runtime(*parent).release_child_ref(*parent, policy);
}

inline void ParentScopeLease::Deleter::operator()(detail::TaskControlBase* p) const {
  bridge::runtime(*p).release_child_ref(*p, CancellationRuntime::ChildReleasePolicy::MayComplete);
}

inline bool ParentScopeLease::is_cancelled() const {
  return ptr_ && bridge::should_finish_due_to_cancellation(*ptr_);
}

inline ParentScopeLease bridge::make_parent_scope_lease(detail::TaskControlBase& ctrl) {
  bridge::runtime(ctrl).add_child_ref();
  return ParentScopeLease(&ctrl);
}

inline void ParentScopeLease::publish_heap_cancel_node(CancelNode& node) {
  CHECK(ptr_);
  bridge::runtime(*ptr_).publish_cancel_node(node);
}

inline ParentScopeLease ParentScopeLease::copy() const {
  if (!ptr_) {
    return {};
  }
  return bridge::make_parent_scope_lease(*ptr_);
}

}  // namespace td::actor
