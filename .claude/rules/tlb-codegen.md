---
paths:
  - "test/tontester/src/tlb/**"
  - "test/tontester/tests/tlb/**"
---

# TL-B Codegen Rules

## Commands

All commands run from the repo root.

**Check source:**
```
uv run basedpyright test/tontester/src/tlb
```

**Regenerate + check generated + check tests + run tests:**
```
uv run python test/tontester/generate_tl.py && uv run basedpyright test/tontester/tests/tlb && uv run pytest test/tontester/tests/tlb/ -q --tb=short
```

## Architecture

Pipeline: `TL-B text -> Lexer -> Parser -> AST -> Sema -> Resolved IR -> Codegen -> Python`

- See `test/tontester/src/tlb/generator/ARCHITECTURE.md` for full details
- See `test/tontester/src/tlb/generator/grammar.md` for the TL-B grammar

## Codegen-specific rules

- **All variable names through NameScope.** Never hardcode generated variable names -- always go through `scope.bind()` or `scope.reserve()`. Use `bind_field()` for dataclass fields.
- **Generated code must also pass basedpyright** with zero warnings.
- **Data errors -> TlbModelError**, not assert. Assert is for internal invariants.
- **No placeholder code.** If codegen can't handle a case, assert False with a clear TODO message.
- **Reserve names that generated code references** (like `_T` in generic method signatures) in NameScope's `_RESERVED` set, so they can't be shadowed by generated field/param names.
- **Watch for field name shadowing** in dataclasses: a field named `field` shadows `dataclasses.field()`.
- **Every bug fix must include a targeted unit test** that would have caught the bug.
