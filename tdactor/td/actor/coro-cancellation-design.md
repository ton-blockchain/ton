# Coroutine Cancellation (Minimal Design)

## Goals
- O(1) `is_cancelled`, O(N) `cancel`.
- Parent waits for all children in `final_suspend`.
- `StartedTask` destructor cancels coroutine and its children.
- Small state, simple rules, no cross-thread resume from handle destruction.
- Explicit lifetime ownership via parent scope leases (`PromiseChildLease`) for scopes that may outlive creator frames.
- `ignore_cancellation()` guard to temporarily suppress cancellation propagation.

## Core State

### CancellationState

Pure state machine: cancel flag + child count + waiting flag + ignored flag.

```cpp
struct CancellationState {
  static constexpr uint32_t CANCELLED = 1u << 31;
  static constexpr uint32_t WAITING   = 1u << 30;
  static constexpr uint32_t IGNORED   = 1u << 29;
  static constexpr uint32_t COUNT_MASK = IGNORED - 1;

  std::atomic<uint32_t> state_;  // flags + child_count
};
```

- `CANCELLED` — scope has been cancelled.
- `WAITING` — parent reached `final_suspend` and must wait for children.
- `IGNORED` — `ignore_cancellation()` is active; prevents topology walk.
- Low 29 bits — child reference count.

Key queries:
- `is_cancelled()` — tests CANCELLED only.
- `is_effectively_cancelled()` — `CANCELLED && !IGNORED`. Used by `should_finish_due_to_cancellation()` to decide whether a coroutine should short-circuit.

### CancelNode hierarchy

`CancelNode` is the base class for anything that appears in a scope's cancellation topology. It is ref-counted and forms an intrusive singly-linked list.

```
CancelNode (ref-counted, intrusive list node)
  ├── promise_common  (coroutine scopes — child tasks)
  └── HeapCancelNode  (disarm pattern for external resources)
        └── TimerNode  (sleep timers)
```

**CancelNode** — virtual `on_cancel()`, `on_cleanup()`, `on_publish()`. Single-shot `try_publish_once()` guard. `on_zero_refs()` is pure virtual.

**HeapCancelNode** — adds `armed_` atomic bool and the disarm pattern: `on_cancel()` and `on_cleanup()` call `disarm()` first; only the winner executes `do_cancel()` or `do_cleanup()`. `on_publish()` takes a topology ref; `on_cleanup()` drops it. Destroyed via `delete this` on zero refs.

**promise_common** — is itself a `CancelNode`. `on_cancel()` delegates to `CancellationRuntime::cancel()`. `on_cleanup()` calls `dec_ref()`. `on_publish()` calls `add_ref()`.

### Topology

Lock-free intrusive singly-linked list inside `CancellationRuntime`. Holds all `CancelNode`s published to a scope.

```cpp
struct Topology {
  PublishResult publish(CancelNode& node);         // CAS into head
  void for_each_seq_cst(Fn&& fn) const;            // cancel walk
  void for_each_acquire(Fn&& fn) const;             // cleanup walk

  std::atomic<CancelNode*> head_{nullptr};
};
```

- `publish()` calls `try_publish_once()`, then `on_publish()` (adds ref), then CAS-pushes onto the list.
- After publish, if the scope is effectively cancelled, `on_cancel()` is called immediately.

### ParentLink

Links a child scope to its parent. Supports two linking modes:

```cpp
struct ParentLink {
  enum class ReleaseReason : uint8_t { ChildCompleted, Teardown };

  void link(promise_common& self, promise_common* parent);
  void link_from_promise_child_lease(promise_common& self, PromiseChildLease parent_scope_ref);
  void release(ReleaseReason reason);
};
```

- `link()` — increments parent's child count, publishes self into parent's topology.
- `link_from_promise_child_lease()` — transfers ownership of an existing child-count lease (no extra increment).
- `release(ChildCompleted)` — decrements parent child count and may complete a waiting parent.
- `release(Teardown)` — decrements parent child count but never completes (preserves the "no resume from non-scheduler threads" rule).

### PromiseChildLease

```cpp
using PromiseChildLease = std::unique_ptr<promise_common, PromiseChildLeaseDeleter>;
```

Holds one child-count lease on a parent scope. Destructor decrements the count and may complete a waiting parent. Created via `make_promise_child_lease()`. Used by `start_with_parent_scope()` and friends to pass scope ownership across non-coroutine boundaries.

### bridge namespace

Breaks the circular dependency between `promise_common` (which contains `CancellationRuntime`) and `CancellationRuntime` (which needs to call back into `promise_common`). Provides:
- `runtime()` — access `CancellationRuntime` from a `promise_common`.
- `complete_scheduled()` — trigger scheduled completion.
- `cancel_node()` — upcast `promise_common&` to `CancelNode&`.
- `should_finish_due_to_cancellation()` — delegation.

## Invariants
- Child count counts **all child scopes**, including `PromiseChildLease` refs.
- Detaching a `StartedTask` handle does **not** detach it from the parent scope.
- `PromiseChildLease` destructor decrements the count and may complete a waiting parent, but completion is always **scheduled** (never inline resume).
- `ParentLink::release(Teardown)` is used from `on_last_ref_teardown()` to preserve the "no resume from non-scheduler threads" rule.
- When IGNORED is set, `cancel()` sets the CANCELLED flag but skips the topology walk. The deferred walk happens in `leave_ignore()`.

## TLS at Cancellation Resume Points

All coroutine resume paths use centralized helpers in `coro_types.h`, never raw `.resume()`. This matters for cancellation because cancel callbacks and timer expiry are the two main non-`co_await` resume sites.

- **Timer cancel** (`TimerNode::do_cancel`): calls `try_claim(CANCELLED)` and, if it wins, `resume_on_current_tls(cont)`. The cancel runs within an existing coroutine's execution (the parent called `cancel()`), so TLS is already valid.
- **Timer expiry** (`TimerNode::process_expired`): calls `try_claim(FIRED)` and, if it wins, `resume_root(cont)`. The scheduler fires expired timers with no coroutine context, so TLS is set to `nullptr`.
- **Symmetric transfer at `final_suspend`**: the continuation returned by `complete_inline()` flows through `Executor::resume_or_schedule`, which calls `continue_with_tls(cont)` — a direct `set_current_promise(cont)` before returning the handle. No `.resume()` call; the compiler drives symmetric transfer.
- **Scheduled completion** (`complete_scheduled`): when `final_suspend` waits for children and the last child completes later, the continuation is enqueued via `SchedulerExecutor::schedule`. `CpuWorker::run` then decodes the token and calls either `resume_with_tls(h, promise)` (promise-encoded) or `resume_root(h)` (handle-encoded).

## Cancellation Contract
- Structured cancellation guarantees apply to scoped starts:
  - `start(scope)`
  - `start_immediate(scope)`
  - `start_in_current_scope()`
  - `start_immediate_in_current_scope()`
  - `start_*_with_parent_scope(...)`
- Legacy unscoped starts (`start_deprecated()`, `start_immediate_deprecated()`) are compatibility APIs and should not be relied on for parent-await cancellation propagation.
- `HeapCancelNode` is the extension point for **custom external resources** (timers, IO, etc.) that need to participate in cancellation. They are published into the scope's topology and receive `on_cancel()`/`on_cleanup()` callbacks.
- Cancellation is **scheduler/actor-context-affine**: `promise_common::cancel()` expects an active `SchedulerContext`, and timer cancel callbacks rely on this context for message routing.

## Operations

### is_cancelled (O(1))
```cpp
bool is_cancelled() { return state & CANCELLED; }
bool is_effectively_cancelled() { return (state & CANCELLED) && !(state & IGNORED); }
```

`is_effectively_cancelled()` is what `should_finish_due_to_cancellation()` uses. A coroutine inside an `ignore_cancellation()` guard will not short-circuit even if CANCELLED is set.

### cancel (O(N))
- Atomically set `CANCELLED` via `fetch_or`. If already set, return.
- If `IGNORED` is set, return (deferred to `leave_ignore()`).
- Call `cancel_topology_with_self_ref()`: take a self-ref, walk topology calling `on_cancel()` on each node, drop self-ref.

### final_suspend waiting
```cpp
if (try_wait_for_children()) return noop; // WAITING set
complete(); // notify parent + resume continuation
```

`try_wait_for_children()`:
- If count is 0 -> return false (no wait).
- Else set `WAITING` bit via atomic RMW and return true.

### child completion
On `release_child_ref()`:
- `prev = fetch_sub(1)`
- If `prev.count == 1` and `prev.WAITING` and `policy == ChildReleasePolicy::MayComplete` -> `complete_scheduled()` (notify parent + schedule continuation).
- Otherwise do nothing.

## ignore_cancellation

```cpp
auto guard = co_await ignore_cancellation();
// ... cancellation suppressed ...
// guard destructor re-enables cancellation
```

Returns a `CancellationGuard` RAII object. While active, `is_effectively_cancelled()` returns false even if CANCELLED is set.

### State machine

**Enter** (`try_enter_ignore()`):
1. If `ignored_depth_` > 0, increment and return true (nested).
2. CAS `IGNORED` bit into state. If CANCELLED is already set (without IGNORED), return false — cancellation wins the race, and the awaiter suspends into `finish_if_cancelled()`.
3. On success, increment `ignored_depth_`.

**Leave** (`leave_ignore()`):
1. Decrement `ignored_depth_`. If still > 0, return (nested).
2. Clear IGNORED bit via `fetch_and(~IGNORED)`.
3. If CANCELLED was set, perform the deferred topology walk (`for_each_seq_cst` calling `on_cancel()`).

### Race semantics
- **Cancel-before-ignore**: `try_enter_ignore()` fails, coroutine finishes with cancelled status.
- **Ignore-before-cancel**: `cancel()` sets CANCELLED but skips the walk. `leave_ignore()` performs the walk.
- **Nested ignore**: `ignored_depth_` counter; only the outermost leave clears the bit.

## CancellationRuntime

The `CancellationRuntime` struct is embedded in every `promise_common`. It owns:

- `CancellationState state_` — flags + child count.
- `Topology topology_` — intrusive list of `CancelNode`s.
- `ParentLink parent_link_` — link to parent scope.
- `uint32_t ignored_depth_` — nesting counter for `ignore_cancellation()`.

Key methods:
- `cancel()` — set CANCELLED, walk topology (unless IGNORED).
- `publish_cancel_node()` — add a node to topology; cancel immediately if scope is effectively cancelled.
- `try_enter_ignore()` / `leave_ignore()` — ignore cancellation guard support.
- `try_wait_for_children()` — final_suspend child-wait.
- `add_child_ref()` / `release_child_ref(..., ChildReleasePolicy)` — child count management.
- `on_last_ref_teardown()` — release parent link (Teardown) and cleanup topology.

## Why this is minimal
- 3 atomics per scope: `state_`, `topology_.head_`, `parent_link_`.
- `HeapCancelNode` is opt-in: only allocated for external resources (timers, IO).
- `CancelNode` hierarchy reuses ref-counting and intrusive list for both child tasks and external resources — no separate mechanism.
- No awaiter cancel slot — cancellation propagates through the topology, not through awaiter internals.
