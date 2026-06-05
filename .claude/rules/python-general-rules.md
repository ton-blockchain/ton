---
paths:
  - "**/*.py"
---

# General Python Coding Rules

## Typing

Strict typing rules enforced by basedpyright (`uv run basedpyright`). You MUST maintain zero warnings and zero errors:

- **No `object` as type annotation** as a "weak `Any`". Use proper union types or `ABC` with `@abstractmethod` when there's a real constraint. `object` is correct — and sometimes the only correct answer — as the legitimate top type in **phantom / unconstrained type-parameter positions**, e.g. `Coroutine[object, object, T]` for a dispatcher that only cares about the return type, or `list[Awaitable[object]]` passed to `asyncio.gather(*, return_exceptions=True)`. Prefer a covariant stdlib alias when one exists (often `Awaitable[T]` instead of `Coroutine[object, object, T]`).
- **No `# type: ignore`, `# pyright: ignore`, `cast()`, or `typing.Any`** unless absolutely necessary. The only exception is working around wrong/incomplete type stubs in third-party libraries we cannot change. All other typing suppressions MUST be explicitly approved by the user. You are unlikely to get an approval though. If you are stuck trying to placate basedpyright, look around the codebase for a similar pattern that is already typed.
  - A module-level suppression like `# pyright: reportPrivateUsage=false` at the top of a **test file** is acceptable when the test deliberately exercises private state; treat it like the already-approved `# pyright: reportAny=false` in existing tests.
- **`match` statements** for exhaustive isinstance dispatches (3+ branches). Two-branch if/else is fine.
- **`@override` on every method that overrides a parent.** basedpyright enforces `reportImplicitOverride`. This includes Protocol implementations.
- **`@final` on concrete classes** that won't be subclassed. Pick ONE: either `@final` *or* explicit `self.x: Type = ...` in `__init__`. With `@final` basedpyright infers attribute types from the RHS, so the annotation is usually redundant, only keep an explicit annotation if basedpyright's heuristics do not widen/constrain the type correctly.
- **`@abstractmethod` on ABC methods**, not `raise NotImplementedError`.
- **`Protocol` method bodies must be `...`** — not a docstring alone. basedpyright requires an actual return-path, and a pure docstring body infers `-> None`, which then fails `reportReturnType` at callsites.
- **`frozen=True` on immutable dataclasses.** Use `@functools.cached_property` for computed properties on frozen dataclasses.
- **Mixins must be `Protocol`s**, not plain classes. basedpyright requires structural typing for mixin patterns.
- **`eq=False` on mutable dataclasses** that should compare by identity.
- **No implicit string concatenation without explicit parentheses.** Use `(f"..." f"...")` not `f"..." + f"..."`. basedpyright requires the inner parens.
- **`except (A, B):` tuple-except parens are optional in Python 3.14+.** Ruff format strips them to `except A, B:`. Both parse identically as tuple excepts. Do not "correct" ruff's output.
- **Assign `_ =` to unused return values** (e.g., `_ = builder.store_uint(...)`). This includes `_ = await asyncio.Event.wait()` and `_ = task.cancel()`.
- **No `from __future__ import annotations`** -- not needed in Python 3.14+. PEP 695 `type X = Y | Z` for type aliases.
- **Don't wrap annotations in string literals.** Python 3.14 lazily evaluates annotations (PEP 649), so `x: "Foo"` and `x: Foo` are equivalent at runtime — no startup cost, no forward-reference benefit. Use string literals ONLY when basedpyright actually requires them (rare in 3.14). The canonical surviving case is CRTP: `class Foo(Base[Foo]):` — the name inside the class's own bases needs quotes because `Foo` isn't bound yet. Forward references across class bodies or within the same module do not need quotes under PEP 649.
- **`Callable[[Args], Ret]`** from `collections.abc`.
- **`@contextmanager` / `@asynccontextmanager` annotate `-> Generator[T]` / `-> AsyncGenerator[T]`**, not `Iterator[T]` / `AsyncIterator[T]`. basedpyright reports the `Iterator` form as deprecated — the decorator calls `.send()` on the generator, so the return type must preserve that capability. The yield-only single-parameter form is what PEP 696 / 3.13+ default the other type args to.
- **Prefix unused parameters with `_`** (e.g., `_idx`, `_ti`) to avoid `reportUnusedParameter`.
- **Name decorator-registered callbacks `_`.** FastAPI routes, pytest fixtures, WebSocket handlers, etc. register their target function via side-effect on the decorator — the name is never referenced by the programmer. Use literal `_` to avoid `reportUnusedFunction` without needing a suppression. Python allows multiple `_` definitions in the same scope (each rebinds the earlier one). When pytest needs the fixture name for dependency injection, use `@pytest_asyncio.fixture(name="explicit_name")` alongside `async def _()`.
- **Function return types are rarely needed.** basedpyright can infer them correctly in almost all cases. Only include them if it is necessary to upcast a specific type to a protocol or a base class for a public-facing API or if they bring clarity.

## Variance

- **When basedpyright complains about invariance, reach for a read-only covariant Protocol.** Generic containers that can *both* be read and written are invariant by design: if `Container[T]` lets you `write(t: T)`, you can't upcast `Container[Cat]` to `Container[Animal]` without breaking type-safety. The fix is to express your use case as a narrower view that only *reads* `T`, then parameterize it with a covariant `TypeVar("_T_co", covariant=True)`. Call sites typed against that view accept any `Container[Subtype]`.
- **Before writing your own Protocol, check whether a covariant stdlib alternative already exists:**
  - `Awaitable[_T_co]` instead of `Future[_T]` / `Task[_T]` (both invariant).
  - `Iterable[_T_co]` / `Sequence[_T_co]` / `Mapping[_K, _V_co]` instead of `list[_T]` / `dict[_K, _V]` (invariant).
  - `Coroutine[..., ..., _T_co]` is covariant in its return type.
  - For instance, a heterogeneous list passed to `asyncio.gather(*, return_exceptions=True)` should be typed `list[Awaitable[object]]`, not `list[Future[object]]`.
- **When you must own a Protocol, write it explicitly read-only.** Only include methods that *return* `_T_co`; keep anything that consumes `T` in a separate invariant interface, or omit it entirely.

## Constructor signatures

- **Identity positional, everything else keyword-only after `*`.** Required values that identify the instance (`name`, `socket_path`, the thing the class is *about*) stay positional. Optional collaborators for DI (`runner: ProcessRunner | None = None`) and tunable constants (`heartbeat_timeout_seconds: float = 30.0`) go after `*`.
- **Don't name a knob "for tests".** A knob is a knob regardless of who turns it. `heartbeat_timeout_seconds` is better than `test_heartbeat_timeout_seconds` even when tests are the only current caller overriding it — the latter misleads future maintainers into thinking it's vestigial.

## Architecture

- **Inject collaborators as Protocols** when a class has external dependencies (subprocesses, HTTP, etc.) that tests need to substitute. Keep production defaults in the constructor signature (e.g. `runner: ProcessRunner | None = None` with a `_RealProcessRunner()` fallback). Protocols beat ABCs here because they don't force implementers to inherit.
- **Use an ABC over a Protocol** only when at least two implementations *compose* behavior (e.g. a delegating wrapper that overrides selected methods) — otherwise a Protocol is lighter.
- **Test harness belongs inside the production package as `<pkg>.testing`.** Co-locating test doubles with the module under test avoids every flavor of path-shadowing / relative-import / sys.path workaround under basedpyright strict. Follow the pattern `fastapi.testclient`, `httpx.testing`, `pydantic.dataclasses` use.
- **Discriminated unions via pydantic:** `Annotated[A | B | C, Field(discriminator="kind")]` + `TypeAdapter(MyUnion)`. Parses in one call; surfaces the discriminator error cleanly.

## Testing

- **`@pytest_asyncio.fixture`** for async fixtures under strict mode, not `@pytest.fixture` — the latter is silently not awaited and pytest ≥9 errors out.
- **`aiotools.VirtualClock().patch_loop()`** for time-sensitive async tests. Wire up as an `autouse=True` fixture in `conftest.py`; `asyncio.sleep()` and `asyncio.wait_for()` become instant, tests that accidentally wait on a never-firing event fail fast instead of hanging.

## Workflow

- **Run `uv run basedpyright` continuously** -- after every substantial change, check the affected files. Typing requires constant attention, not just a final check at the end.
- **Before finishing** (i. e. after fixing a bug or implementing a requested feature), make sure `uv run ruff format` and `uv run ruff check` are clean.
