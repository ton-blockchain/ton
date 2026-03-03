# Coroutine Cancellation Design

## Goals
- O(1) `is_cancelled`, O(N) `cancel`.
- Parent waits for all children in `final_suspend`.
- `StartedTask` destructor cancels coroutine and its children.
- Child locals are destroyed before parent resumes (all paths: normal, error, cancel).
- Actor-bound suspended coroutines are cancelled automatically when the actor stops.
- Actor-affine coroutine teardown still runs with actor context even after the actor is closed.
- Explicit lifetime ownership via `ParentScopeLease` for scopes that may outlive creator frames.
- `ignore_cancellation()` guard to temporarily suppress cancellation propagation.

## Memory Layout: Single-Allocation Split Lifetime

### Problem
A cancelled coroutine must destroy its locals before the parent resumes. But external handles (`StartedTask`, `Ref<TaskControlBase>`) may still need to read the result after the frame is gone. The control block must outlive the frame.

### Solution
`TaskControl<T>` is placed directly before the coroutine frame in one allocation:

```
[allocation_base ... padding] [TaskControl<T>] [coroutine frame]
```

`promise_type<T>::operator new` allocates both regions. `TaskControl<T>::from_frame(h.address())` recovers the control via pointer arithmetic: `frame_ptr - sizeof(TaskControl<T>)`.

`Task<T>` and `StartedTask<T>` hold a `Ref<TaskControl<T>>` — ref-counted pointer to the control block. `promise_common` stores a raw `TaskControlBase*` back-pointer (set in `get_return_object()`) for non-template access.

The control block is ref-counted. The frame can be destroyed independently (via `try_destroy_frame()`). The allocation is freed only when the last ref drops (`on_zero_refs`).

### HALO Detection

Compilers may apply Heap Allocation eLision Optimization (HALO), placing coroutine frames on the stack and bypassing `operator new`. This breaks the single-allocation layout — the control block would not exist.

`TaskControlBase` writes a magic cookie (`0xC020C070`) at construction. `get_return_object()` checks it via `check_magic()`. If HALO elided the allocation, the cookie is absent and a fatal error fires immediately.

### Resume-Boundary Cancellation and Frame Destruction

C++ exceptions are not used for cancellation. Cancellation is observed at resume boundaries instead:

- awaiters check `should_finish_due_to_cancellation_tls()` in `await_ready()` and force the slow path when cancellation is pending;
- `route_resume_on_bound_executor()` is the single re-entry gate that publishes `cancelled_status()` when needed and enters `final_suspend()` if a terminal result is already available;
- frame destruction happens from the normal completion paths (`complete_inline()` / `on_zero_refs()`), not from a separate cancel-only frame-destroy helper.

Coroutine locals are still destroyed by the frame destructor, not by stack unwinding. The ordering guarantee (child locals destroyed before parent resumes) is maintained because `complete_inline()` destroys the frame before releasing the parent child-ref.

## Type Hierarchy

### TaskControl

```
CancelNode (ref-counted, intrusive list node)
  └── TaskControlBase (state machine, cancellation runtime, magic cookie)
        └── TaskControl<T> (result storage, allocation metadata)
```

`TaskControlBase` contains:
- `TaskStateManagerData` — executor, flags, continuation.
- `CancellationRuntime` — state, topology, parent link.
- `magic_` — HALO detection cookie.
- `frame_destroyed_` — single-destroy guard.
- `cancel()`, `should_finish_due_to_cancellation()`, `route_resume_from_external()`, `route_resume_on_bound_executor()`, `complete_inline()`, `complete_scheduled()` — core completion/cancellation logic.

`TaskControl<T>` adds:
- `result_` — `std::optional<td::Result<T>>`, persists after frame destroy.
- `allocation_base_`, `allocation_align_` — for deallocation.

### TaskHandle (CRTP)

`TaskHandle<Derived, T>` is the CRTP base for `Task<T>` and `StartedTask<T>`. Stores `Ref<TaskControl<T>> control_`. Provides:
- `ctrl()` — dereferences `control_`.
- `await_resume()`, `await_resume_peek()`.
- `wrap()`, `trace()`, `child()`, `unlinked()`, `then()`.

### CancelNode Hierarchy

```
CancelNode (ref-counted, intrusive list node)
  ├── TaskControlBase (coroutine scopes — child tasks)
  └── HeapCancelNode (disarm pattern for external resources)
        └── TimerNode, CancelPromiseNode, etc.
```

`HeapCancelNode` — adds `armed_` atomic. `on_cancel()`/`on_cleanup()` call `disarm()` first; only the winner executes `do_cancel()`/`do_cleanup()`. Destroyed via `delete this` on zero refs.

## CancellationState

```cpp
struct CancellationState {
  static constexpr uint32_t CANCELLED = 1u << 31;
  static constexpr uint32_t WAITING   = 1u << 30;
  static constexpr uint32_t IGNORED   = 1u << 29;
  static constexpr uint32_t COUNT_MASK = IGNORED - 1;

  std::atomic<uint32_t> state_;
};
```

- `CANCELLED` — scope has been cancelled.
- `WAITING` — parent reached `final_suspend` and must wait for children.
- `IGNORED` — `ignore_cancellation()` is active; prevents topology walk.
- Low 29 bits — child reference count.

Key queries:
- `is_cancelled()` — tests CANCELLED only.
- `is_effectively_cancelled()` — `CANCELLED && !IGNORED`. Used by `should_finish_due_to_cancellation()`.

## Topology

`CancelTopology<Tag>` — mutex-protected intrusive linked list of `CancelNode`s. Templated on tag for independent lists:

- `TopologyTag` — per-task cancellation topology for child scopes and heap cancel nodes.
- `ActorCancelTag` — per-actor topology for coroutines currently suspended on actor-bound external work.

Operations:
- `publish_and_maybe_cancel(node, is_cancelled_fn)` — add to list, cancel immediately if scope is already cancelled.
- `unpublish_and_cleanup(node)` — remove from list, call `on_cleanup()`.
- `cancel_snapshot()` — ref-hold all nodes, call `on_cancel()` on each, dec-ref.
- `drain_cleanup()` — remove all nodes, ref-hold, call `on_cleanup()` on each, dec-ref.

`cancel_snapshot` and `drain_cleanup` use a shared `batch()` template that handles the ref-hold→action→dec-ref loop.

`ActorInfo` owns `CancelTopology<ActorCancelTag> coro_cancel_topology_` plus `coro_cancelled_`. `wrap_coroutine()` publishes the suspended outer task control into this actor topology, and the wrapper's `final_suspend()` unpublishes it when the await completes.

## ParentLink

```cpp
struct ParentLink {
  enum class ReleaseReason : uint8_t { ChildCompleted, Teardown };

  void link_from_parent_scope_lease(CancelNode& self, ParentScopeLease parent_scope_ref);
  TaskControlBase* detach_for_child_completed(CancelNode& self);
  void release(CancelNode& self, ReleaseReason reason);

  std::atomic<TaskControlBase*> parent_{nullptr};
};
```

- `link_from_parent_scope_lease()` — transfers lease ownership, publishes child into parent topology.
- `detach_for_child_completed()` — unpublishes from parent topology (with self-ref guard against topology-ref drop), clears parent pointer. Returns parent for caller to release child-ref.
- `release(ChildCompleted)` — detaches + decrements parent child count. May complete a waiting parent.
- `release(Teardown)` — decrements parent child count but never completes (preserves the "no resume from non-scheduler threads" rule).

## ParentScopeLease

```cpp
class ParentScopeLease {
  std::unique_ptr<TaskControlBase, Deleter> ptr_;
};
```

Holds one child-count lease on a parent scope's control block. Destructor decrements the count and may complete a waiting parent. Created via `bridge::make_parent_scope_lease(TaskControlBase&)`.

## Completion Paths

### Normal / Error (final_suspend)

1. Coroutine body returns or throws → result published to `TaskControl<T>`.
2. `final_suspend` calls `try_wait_for_children()`.
   - If children remain → suspend (WAITING set). Last child completes parent via `complete_scheduled()`.
   - If no children → `complete_inline()`: notify parent, resume continuation.

### Cancellation / Resume Boundary

1. `cancel()` sets `CANCELLED`. If `IGNORED` is not active, it immediately walks the task topology via `cancel_snapshot()`.
2. Awaiters notice cancellation through `should_finish_due_to_cancellation_tls()` and route back through `route_resume_from_external()` instead of consuming a ready result inline.
3. `route_resume_on_bound_executor()` is the canonical completion gate:
   - If the scope is cancelled, it stores `cancelled_status()` unless another terminal result is already present.
   - If any terminal result is present (cancelled, error, externally supplied), it enters `final_suspend()`.
   - Otherwise it continues the coroutine body on the bound executor with TLS restored.
4. `final_suspend()` either waits for children or calls `complete_inline()`.
5. `complete_inline()` marks the task ready, detaches the parent link for child completion, destroys the frame, releases refs, then returns the continuation.

The normal completion path therefore also enforces the cancellation ordering guarantee: **child locals are destroyed before parent resumes**.

### Actor Stop / Closed Actor Path

1. When an actor-bound coroutine suspends on external work, `wrap_coroutine()` publishes its `TaskControlBase` into the owning actor's `coro_cancel_topology_`.
2. `ActorExecutor::flush_context_flags()` calls `cancel_coro_cancel_nodes()` before setting the actor's `Closed` flag and before `tear_down()`.
3. Resumes back to the actor use `ActorMessageCoroutineSafe`, which marks the continuation message as `survives_close()`.
4. If the actor closes before that continuation runs, closed-mailbox draining destroys the safe message under actor context. Its destructor calls `fail_actor_destroyed()`, which synchronously re-enters `route_resume_on_bound_executor()` and may drive `final_suspend()` / frame destruction immediately.
5. This is why suspended actor coroutines are cancelled automatically on actor stop, and why actor-affine coroutine locals/destructors can still run with actor context even after close.

### on_zero_refs (last ref drop)

1. `on_last_ref_teardown()` — release parent link (Teardown), drain topology.
2. `try_destroy_frame()` — if frame not already destroyed, destroy it.
3. `free_allocation()` — destruct control, free backing memory.

## Operations

### is_cancelled (O(1))
```
is_cancelled() = state & CANCELLED
is_effectively_cancelled() = (state & CANCELLED) && !(state & IGNORED)
```

### cancel (O(N))
1. `fetch_or(CANCELLED)`. If already set, return.
2. If IGNORED, return (deferred to `leave_ignore()`).
3. `cancel_topology_with_self_ref()`: increment `cancel_traversal_depth_`, take self-ref, call `cancel_snapshot()`, drop self-ref, decrement depth.

### final_suspend waiting
```
if (try_wait_for_children()) return noop;   // children still running
return complete_inline();                    // done
```

### child completion
On `release_child_ref(self, policy)`:
- `prev = state_.fetch_sub(1)`
- If `prev.count == 1` and `prev.WAITING` and `policy == MayComplete` → `complete_scheduled()`.

## ignore_cancellation

```cpp
auto guard = co_await ignore_cancellation();
// cancellation suppressed
// guard destructor re-enables
```

**Enter** (`try_enter_ignore()`):
1. If `ignored_depth_ > 0`, increment and return true (nested).
2. CAS IGNORED bit. If CANCELLED already set without IGNORED, return false — cancellation wins.

**Leave** (`leave_ignore()`):
1. Decrement `ignored_depth_`. If still > 0, return.
2. Clear IGNORED via `fetch_and(~IGNORED)`.
3. If CANCELLED was set, perform deferred topology walk.

## CancellationRuntime

Embedded in `TaskControlBase`. Owns:
- `CancellationState state_` — flags + child count.
- `CancelTopology<TopologyTag> topology_` — intrusive list of cancel nodes.
- `ParentLink parent_link_` — link to parent scope.
- `uint32_t ignored_depth_` — nesting counter.
- `std::atomic<uint32_t> cancel_traversal_depth_` — reentrancy guard for topology walks.

## bridge namespace

Breaks circular dependency between `TaskControlBase` (which contains `CancellationRuntime`) and `CancellationRuntime` (which needs to call back into `TaskControlBase`). Forward-declared in `coro_cancellation_runtime.h`, defined in `coro_task.h` after `TaskControlBase` is complete:
- `runtime(TaskControlBase&)` — access `CancellationRuntime`.
- `complete_scheduled(TaskControlBase&)` — trigger scheduled completion.
- `should_finish_due_to_cancellation(const TaskControlBase&)` — delegation.
- `should_finish_due_to_cancellation_tls()` — check TLS control block.
- `make_parent_scope_lease(TaskControlBase&)` — create a lease.

## Cancellation Contract

- Structured cancellation applies to scoped starts, including internal external-scoped starts.
- Unscoped starts (`start_without_scope()`, etc.) do not participate in parent cancellation.
- Actor-bound external suspension participates in actor-local cancellation even without a parent scope.
- `HeapCancelNode` is the extension point for custom external resources (timers, IO).
- Cancellation is scheduler/actor-context-affine: `TaskControlBase::cancel()` requires an active `SchedulerContext`.

## TLS at Resume Points

Thread-local `current_ctrl_` (`TaskControlBase*`) tracks the currently executing coroutine's control block. Used for `current_scope_lease()` and cancellation checks.

All coroutine resume paths use centralized helpers in `coro_types.h`, never raw `.resume()`:
- `resume_with_tls(h, ctrl)` — set TLS to `ctrl`, resume.
- `resume_with_own_promise(h)` — set TLS from `h.promise().control_`, resume.
- `resume_on_current_tls(h)` — resume with current TLS.
- `resume_root(h)` — resume with TLS=nullptr.

Token encoding for scheduler queue uses bottom 2 bits:
- `...00` — actor message.
- `...01` — handle-encoded continuation (no TLS info).
- `...11` — ctrl-encoded (`TaskControlBase*`, CpuWorker sets TLS before resume).

## Invariants

- Child count counts all child scopes, including `ParentScopeLease` refs.
- Detaching a `StartedTask` handle does not detach from parent scope.
- `ParentScopeLease` destructor may complete a waiting parent, but completion is routed via `complete_scheduled()` and therefore resumes on the parent's bound executor (inline if already on that executor, otherwise scheduled).
- `ParentLink::release(Teardown)` never completes parent (no resume from non-scheduler threads).
- When IGNORED is set, `cancel()` sets CANCELLED but skips topology walk. Deferred to `leave_ignore()`.
- Child locals are destroyed before parent resumes in all completion paths (normal, error, cancellation).
- `TaskControl<T>` magic cookie detects HALO at coroutine creation time.

## File Map

- `coro_task.h` — `TaskControlBase`, `TaskControl<T>`, `TaskHandle` CRTP, `promise_common`, `promise_type<T>`, `Task<T>`, `StartedTask<T>`, `TaskGroup`.
- `coro_cancellation_runtime.h` — `CancellationState`, `CancelNode`, `HeapCancelNode`, `CancelTopology`, `CancellationRuntime`, `ParentLink`, `ParentScopeLease`.
- `coro_awaitables.h` — `TaskAwaiter`, `ResultUnwrapAwaiter`, wrapped coroutine helpers.
- `coro_executor.h` — `Executor`, `ActorExecutor`, `SchedulerExecutor`, `AnyExecutor`.
- `coro_types.h` — TLS, token encoding, `AwaiterOptions`, `FireAndForget`.
- `coro_ref.h` — `Ref<T>` intrusive ref-counted pointer.
- `coro_utils.h` — `with_timeout`, `any`, `all`, `all_fail_fast`, `CoroMutex`, `TaskActor`.
