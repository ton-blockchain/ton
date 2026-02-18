#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <utility>

#include "td/actor/coro_ref.h"
#include "td/utils/common.h"

namespace td {
template <class T>
class Promise;
}  // namespace td

namespace td::actor {

struct promise_common;
struct CancellationRuntime;
struct CancelNode;

// Bridge: operations CancellationRuntime needs on promise_common.
// Breaks the circular dependency (promise_common contains CancellationRuntime).
// All functions are defined in coro_task.h after promise_common is complete.
namespace bridge {
CancellationRuntime& runtime(promise_common& self);
const CancellationRuntime& runtime(const promise_common& self);
std::coroutine_handle<> complete_scheduled(promise_common& self);
CancelNode& cancel_node(promise_common& self);
bool should_finish_due_to_cancellation(const promise_common& self);
}  // namespace bridge

class ParentScopeLease;

namespace bridge {
ParentScopeLease make_parent_scope_lease(promise_common& p);
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
  friend ParentScopeLease bridge::make_parent_scope_lease(promise_common&);

  struct Deleter {
    void operator()(promise_common* p) const;
  };
  std::unique_ptr<promise_common, Deleter> ptr_;

  explicit ParentScopeLease(promise_common* p) : ptr_(p) {
  }
  promise_common* release() {
    return ptr_.release();
  }
  promise_common* get() const {
    return ptr_.get();
  }
};

// Pure state machine: cancel flag + child count + waiting flag.
// No dependencies on promise_common — can be unit-tested in isolation.
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
  enum class ReleaseReason : uint8_t { ChildCompleted, Teardown };

  void link_from_parent_scope_lease(promise_common& self, ParentScopeLease parent_scope_ref);
  void release(ReleaseReason reason);
  bool has_parent() const {
    return parent_.load(std::memory_order_acquire) != nullptr;
  }

 private:
  std::atomic<promise_common*> parent_{nullptr};
};

// Lifecycle:
//   Creator holds initial ref (refs_=1).
//   on_publish() adds topology ref.
//   on_cleanup() drops topology ref.
//   on_zero_refs() destroys the node.
struct CancelNode {
  friend struct CancellationRuntime;
  template <class T>
  friend struct Ref;

  CancelNode* next{nullptr};

  bool try_publish_once() {
    bool expected = false;
    return published_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  virtual ~CancelNode() = default;
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
  std::atomic<bool> published_{false};
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

  void release_child_ref(promise_common& self, ChildReleasePolicy policy) {
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

  void notify_parent_child_completed() {
    parent_link_.release(ParentLink::ReleaseReason::ChildCompleted);
  }

  void set_parent_lease(promise_common& self, ParentScopeLease parent_scope_ref) {
    parent_link_.link_from_parent_scope_lease(self, std::move(parent_scope_ref));
  }

  void publish_cancel_node(CancelNode& node) {
    if (topology_.publish(node) != Topology::PublishResult::Published) {
      return;
    }
    if (state_.is_effectively_cancelled_seq_cst()) {
      node.on_cancel();
    }
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

  void on_last_ref_teardown() {
    parent_link_.release(ParentLink::ReleaseReason::Teardown);
    topology_.for_each_acquire([](CancelNode& n) { n.on_cleanup(); });
  }

 private:
  void cancel_topology_with_self_ref(CancelNode& self) {
    self.add_ref();
    topology_.for_each_seq_cst([](CancelNode& n) { n.on_cancel(); });
    self.dec_ref();
  }

  struct Topology {
    enum class PublishResult : uint8_t { Published, AlreadyPublished };

    PublishResult publish(CancelNode& node) {
      if (!node.try_publish_once()) {
        return PublishResult::AlreadyPublished;
      }
      DCHECK(node.next == nullptr);
      node.on_publish();
      auto* head = head_.load(std::memory_order_seq_cst);
      do {
        node.next = head;
      } while (!head_.compare_exchange_weak(head, &node, std::memory_order_seq_cst, std::memory_order_seq_cst));
      return PublishResult::Published;
    }

    template <class Fn>
    void for_each_seq_cst(Fn&& fn) const {
      for (auto* node = head_.load(std::memory_order_seq_cst); node;) {
        auto* next = node->next;
        fn(*node);
        node = next;
      }
    }

    template <class Fn>
    void for_each_acquire(Fn&& fn) const {
      for (auto* node = head_.load(std::memory_order_acquire); node;) {
        auto* next = node->next;
        fn(*node);
        node = next;
      }
    }

    std::atomic<CancelNode*> head_{nullptr};
  };

  uint32_t ignored_depth_{0};
  CancellationState state_{};
  Topology topology_{};
  ParentLink parent_link_{};
};

inline void ParentLink::link_from_parent_scope_lease(promise_common& self, ParentScopeLease parent_scope_ref) {
  if (!parent_scope_ref) {
    return;
  }
  parent_scope_ref.publish_heap_cancel_node(bridge::cancel_node(self));
  auto* transferred_parent = parent_scope_ref.release();
  auto* expected = static_cast<promise_common*>(nullptr);
  CHECK(parent_.compare_exchange_strong(expected, transferred_parent, std::memory_order_release,
                                        std::memory_order_relaxed));
}

inline void ParentLink::release(ReleaseReason reason) {
  auto* parent = parent_.exchange(nullptr, std::memory_order_acq_rel);
  if (!parent) {
    return;
  }
  auto policy = reason == ReleaseReason::ChildCompleted ? CancellationRuntime::ChildReleasePolicy::MayComplete
                                                        : CancellationRuntime::ChildReleasePolicy::NoComplete;
  bridge::runtime(*parent).release_child_ref(*parent, policy);
}

inline void ParentScopeLease::Deleter::operator()(promise_common* p) const {
  bridge::runtime(*p).release_child_ref(*p, CancellationRuntime::ChildReleasePolicy::MayComplete);
}

inline bool ParentScopeLease::is_cancelled() const {
  return ptr_ && bridge::should_finish_due_to_cancellation(*ptr_);
}

inline ParentScopeLease bridge::make_parent_scope_lease(promise_common& p) {
  bridge::runtime(p).add_child_ref();
  return ParentScopeLease(&p);
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
