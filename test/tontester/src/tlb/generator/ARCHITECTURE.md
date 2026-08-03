# TL-B Code Generator Architecture

## What is TL-B?

TL-B (Type Language - Binary) is the schema language for TON blockchain's binary
serialization format. It describes how data structures are serialized into cells
(bit strings with references to other cells). See `grammar.md` for the formal syntax.

Key concepts:
- **Constructors** define how to serialize one variant of a type (tag + fields)
- **Tags** are bit prefixes that identify which constructor was used
- **Implicit params** (`{n:#}`, `{X:Type}`) are not serialized — inferred from context
- **Output params** (`~n`) are computed during deserialization, not known beforehand
- **Cell references** (`^Type`) store a value in a separate cell
- **Special cells** (`!` prefix) are exotic TON cell types (merkle proofs, pruned branches)
- **Inline records** (`[field1:Type1 field2:Type2]`) are anonymous types

## Pipeline

```
TL-B text → Lexer → Parser → AST → Sema → Resolved IR → Codegen → Python
```

### Lexer (`lexer.py`)
Tokenizes TL-B text. Single `IDENT` token type (no LC/UC distinction — case
conventions checked in parser/sema). Handles `#hex_` and `$bin_` tag literals,
`##`, `#<`, `#<=` compound tokens.

### Parser (`parser.py`)
Recursive descent, LL(2), no backtracking. Field types parsed at `conditional`
precedence level (expr95) — application/arithmetic need parentheses in field
position. Produces `ast_nodes.py` types.

### Sema (`sema/`)

Nine phases in `sema/__init__.py`:

1. **Register types** — collect arities, param kinds (Nat/Type), output positions,
   check consistency across constructors (arity, `~` positions, `!` special)
2. **Resolve constructors** — expression resolution (nat vs type disambiguation),
   scope-based name resolution, constraint resolution
3. **Check arities** — validate type application arities and param kind mismatches
   (e.g., `#< T` where T is a Type param)
4. **Insert implicit constraints** — add `n > 0` constraints for `#< n` with
   non-constant n (recurses into nested generics like `Maybe (#< n)`)
5. **Compute tags** — CRC32 auto-tags from canonical text representation
6. **Build match trees** — constructor dispatch (bit prefix + constraint-based),
   follows typedef chains, detects ambiguity
7. **Classify inference** — which Type params support output param propagation
8. **Compute deser plans** — ordered steps (entry bindings from type args,
   ReadField, BindOutputParam, SolveConstraint, CheckConstraint)
9. **Classify types** — is_enum, is_typedef, is_special; topological sort

Key resolved IR types (`sema/types.py`):

**Expression nodes** (all `frozen=True`):
- `ResolvedNatExpr` — nat expressions (NatLiteral, NatParamRef, NatFieldValue,
  NatAdd, NatSub, NatMul, NatGetBit, NatTypeArg). Each has computed properties:
  `references_params` (bool), `references_type_arg` (bool). Binary ops share these
  via `_NatBinOpMixin(Protocol)` with `@functools.cached_property`.
- `ResolvedTypeExpr` — type expressions (TypeApply, TupleType, CellRefType,
  TypeParamRef, AnonymousRecordType). Each has `references_type_params` (bool)
  computed via `@functools.cached_property`.
- `is_nat()` — type guard discriminating `ResolvedExpr` into nat/type.

**Param types** (all `frozen=True`):
- `TypeParamDef(name, type_level_param)` — implicit Type param, links to its TLP
- `NatParamDef(name)` — implicit nat param
- `type ParamDef = TypeParamDef | NatParamDef`
- `TypeLevelParam(position, kind, is_output)` — type-level param position sentinel

**Deser steps** (all `frozen=True`):
- `ReadField`, `BindParam`, `BindOutputParam`, `SolveConstraint`, `CheckConstraint`
- `OutputExtraction.result_param_position` — indexed by TLP position (not a separate
  output-only index)

**Match tree** (all `frozen=True`):
- `MatchBit`, `MatchTag`, `MatchConstraint`, `MatchConstructor`, `MatchFail`

**Mutable container types:**
- `ResolvedConstructor` — `deser_steps`, `source_order`, `nat_param_values` set across phases
- `ResolvedType` — `arity`, `type_level_params`, `match_tree`, `inference` built across phases
- `InferenceInfo` — `constructor_field` dict populated during classification
- `ResolvedField` — `eq=False` (identity comparison)

**`nat_param_values`** on `ResolvedConstructor`: `list[ResolvedNatExpr | None]` indexed by
TLP position. `None` for TYPE TLPs. For NAT TLPs: the expression computing the value
(output params from `~` exprs, non-output from result_param_exprs). Powers `get_output()`.

Conditionals (`flag?Type`) are a field modifier, not a type expression — stored
as `ResolvedField.condition: ResolvedNatExpr | None`.

`Cell` is an alias for `Any` — `^Cell` is just `^Any`.

Inline records (`[field1:Type1 ...]`) create anonymous `ResolvedType` objects
registered in the type registry. Type params (`{X:Type}`) become external
(contribute to arity), nat params (`{n:#}`) stay internal (resolved by deser plan).
Outer scope does not leak into inline records.

Match tree algorithm (`sema/match.py`):
1. Try constraint split on nat type args (partial — reduces groups)
2. Expand typedef chains (follow first inline field's type constructors)
3. Consume common bit prefix → MatchTag
4. Split on diverging bit → MatchBit, recurse
5. Error if any constructor has no more bits (genuinely ambiguous)

### Python Codegen (`py/`)

Generates Python dataclasses with `serialize_to`/`load_from` methods.

Architecture:
- **`PyContext`** (`py/context.py`) — shared state: NameScope, import tracker,
  temp var counter, ref wrapper registry
- **`NameScope`** (`py/name_scope.py`) — collision-free name binding with typed
  binding classes: `BindableForName` (types, constructors, TLPs),
  `BindableForField` (fields, params), `BindableForGeneric` (type variable names
  on TLPs). Uses `IdentityKey` for dict keys.
- **`NatExpr`** (`py/nat_expr.py`) — wraps `ResolvedNatExpr` + scope, renders with
  `.local` (for load_from) or `.self_` (for serialize_to). Also contains
  `render_constraint()` for rendering `CheckConstraint` as Python expressions.
- **`TypeStrategy`** (ABC in `py/strategy/_base.py`) — knows how to emit store/load
  code for a type expression. Provides `type_info_expr()` (local context) and
  `type_info_expr_self()` (self-prefixed for serialize assertions).
  Subclasses in `py/strategy/`: `UintStrategy`, `BoundedUintStrategy`,
  `IntStrategy`, `BitsStrategy`, `UserTypeStrategy`, `CellRefStrategy`,
  `GenericCellRefStrategy`, `TupleStrategy`, `TypeParamStrategy`,
  `SliceTypeStrategy`.
- **`StrategyBuilder`** (`py/strategy/builder.py`) — builds `TypeStrategy` for a
  `ResolvedTypeExpr`. Tracks `used_type_params` for determining which type params
  become class generics.
- **`TypeGenerator`** (`py/type_generator.py`) — generates type alias, TypeInfo class
  with `__eq__`/`__hash__`, `load_from` with nat param assertions, match tree dispatch
- **`ConstructorGenerator`** (`py/constructor_generator.py`) — generates dataclass
  with serialize assertions (nat params ≥ 0, field constraints, sub-type consistency
  via `get_output()` and `check_type()`), `load_from` with deser plan execution,
  `get_output()` for all nat TLP values, `check_type()` for type param verification
- **`MatchTreeGenerator`** (`py/match_tree_generator.py`) — generates dispatch code
  on a `probe` copy of the slice

Scope hierarchy:
- **File scope** (`ctx.scope`) — type names, constructor names
- **Type scope** (child of file scope) — `TypeLevelParam` sentinels: `bind()` for
  TypeInfo param names (`_type_arg_{pos}`, `_t{Name}`), `bind_generic()` for type
  variable names (`X`, `T`). Used by TypeInfo and MatchTree.
- **Constructor scope** (child of type scope) — constructor params (`bind_field()`),
  fields (`bind_field()`).

CellRef handling:
- **Concrete `^Type`** (no type params inside) → lazy wrapper class (`Ref_uint32`,
  `Ref_TickTock`). Deduped by `(descriptor, is_special)`. The `.ref` property
  lazily deserializes and checks for special cells.
- **Parameterized `^Type`** (type or nat params inside) → runtime `Ref[X]` from
  `object.py` with `GenericCellRefStrategy`. Uses `RefType[X].instantiate(inner_ti)`.

Serialization assertions in `serialize_to`:
- `assert self.n >= 0` for all nat params
- Field constraints re-checked (skipping NatTypeArg-referencing constraints
  since those are entry-time checks)
- Sub-type nat params: `assert self.field.get_output(pos) == expected`
- Sub-type type params: `self.field.check_type(pos, expected_ti)`

Deserialization assertions in `load_from`:
- `assert param >= 0` for all nat type-level args at function entry
  (both on TypeInfo and constructor `load_from`)

### Runtime (`tlb/object.py`)

Support library imported by generated code:
- `TLBRecord` — ABC for serializable records. `get_output(idx)` returns nat TLP
  values by position. `check_type(idx, ti)` verifies type TLP values (no-op base,
  overridden in generic constructors).
- `TypeInfo[T, *Args]` — protocol for type (de)serialization. All implementations
  have `__eq__`/`__hash__` for serialize-time type param verification.
- `InstantiableTypeInfo` — TypeInfo with `.instantiate()` for nested generics
- `Ref[X]` — generic lazy cell reference (used by parameterized ^Type codegen path)
- `RefType[X]` — TypeInfo for Ref[X]
- `UintTypeConstructor(n)` — validates `n >= 0`, `value >= 0`, handles `n=0`
- `IntTypeConstructor(n)` — validates `n >= 0`, handles `n=0`
- `BoundedUintTypeConstructor(bound, inclusive)` — `#<=` and `#<` with range validation
  on both load and store
- `BitsTypeConstructor(n)` — validates `n >= 0`, `len(value) == n`
- `TupleTypeConstructor[X](count, element_ti)` — validates `count >= 0`, `len(value) == count`
- `AnyType` — TypeInfo for `Any`/`Cell` (consumes entire slice)
- `TlbModelError` — deserialization/constraint errors

### Type Simplifications (`SimplifyConfig`)

Well-known TL-B types can be simplified to native Python types when
`SimplifyConfig` is passed to `generate_python()`. Controlled per-type via
`WellKnownType` enum. Sema detects patterns in `sema/well_known.py` and sets
`ResolvedType.well_known`. The strategy builder checks `ctx.simplify.is_enabled()`
and produces simplified strategies instead of `UserTypeStrategy`.

**Simplified types:**

| TL-B type | Well-known | Python type | Strategy |
|-----------|-----------|-------------|----------|
| `Maybe X` | `MAYBE` | `X \| None` | `MaybeStrategy` (inline tag + inner) |
| `Unit` | `UNIT` | `None` | `EnumLiteralStrategy` |
| `Bool` | `BOOL` | `bool` | `EnumLiteralStrategy` |
| `True`/`BoolTrue` | `BOOL_TRUE` | `Literal[True]` | `EnumLiteralStrategy` |
| `BoolFalse` | `BOOL_FALSE` | `Literal[False]` | `EnumLiteralStrategy` |
| `Bit` | `BIT` | `bool` (+ `n * Bit` → `bitarray`) | `EnumLiteralStrategy`/`BitsStrategy` |
| `Unary ~n` | `UNARY` | `int` | `UnaryStrategy` (output param = value itself) |
| `HashmapE n X` | `HASHMAP_E` | `HashmapDict[X]` | `HashmapStrategy(allow_empty=True)` |
| `Hashmap n X` | `HASHMAP` | `HashmapDict[X]` | `HashmapStrategy(allow_empty=False)` |
| `VarUInteger n` | `VAR_UINTEGER` | `int` | `VarIntStrategy(signed=False)` |
| `VarInteger n` | `VAR_INTEGER` | `int` | `VarIntStrategy(signed=True)` |
| `^Cell` | (builtin) | `Cell` | `CellRefBuiltinStrategy` |

**Key design decisions:**
- Classes for well-known types are still generated (needed when used unsimplified
  or when nested Maybe falls back). Simplification only affects field-level strategies.
- `MaybeStrategy` checks `inner.is_nullable` — nested `Maybe (Maybe X)` falls back
  to full `UserTypeStrategy` since `X | None | None` would collapse.
- `UnaryStrategy.emit_get_output()` returns the value directly (the int IS the output),
  used by inference chains. `InferenceStep.concrete_arg` stores the concrete type
  expression for the chain endpoint.
- `HashmapDict[V]` is a lazy dict wrapper in `tlb/hashmap.py` backed by generated
  helpers in `tlb/hashmap_auto.py`. Tree traversal on demand, mutations via sorted
  overlay (`SortedDict`), proper trie serialization on write.
- `HashmapStrategy.emit_serialize_assertions` verifies `key_bits` and `value_ti` match.

**Runtime support for simplifications** (`tlb/object.py`):
- `MaybeTypeInfo[X](inner_ti)` — for simplified Maybe as generic arg
- `UnitTypeInfo`, `BoolTypeInfo` — singletons for Unit/Bool
- `UnaryTypeInfo` — unary encoding ser/deser
- `VarUIntTypeConstructor(n)`, `VarIntTypeConstructor(n)` — variable-length integers
- `CellRefType` — opaque `^Cell` (store_ref/load_ref)
- `HashmapDictTypeInfo[V]` — InstantiableTypeInfo for HashmapDict in generic contexts

### Writing a New Backend

The sema layer produces a backend-agnostic resolved IR. To write a new code
generator (e.g., C++, Rust, TypeScript):

1. **Consume `list[ResolvedType]`** from `analyze()`. Each type has constructors,
   fields, type-level params, match trees, deser plans, and `well_known` classification.

2. **Implement a name scope** for your target language's naming rules (keywords,
   reserved names, collision avoidance).

3. **Implement type strategies** — the core abstraction. For each `ResolvedTypeExpr`,
   decide how to emit load/store code. At minimum:
   - Builtin nat types (`##`, `#<`, `#<=`, `uint`, `int`, `bits`)
   - User-defined types (`TypeApply` → delegate to TypeInfo-like construct)
   - Cell references (`^Type`)
   - Tuples (`n * Type`)
   - Type params (generics)
   - Conditionals (`flag?Type`)

4. **Implement constructor/type generators** that walk `deser_steps` and emit
   load/store methods. The deser plan is ordered: entry bindings → field reads →
   constraint checks → output extraction. Follow this order exactly.

5. **Implement a runtime library** for your target language:
   - TypeInfo protocol (serialize/deserialize dispatch)
   - TLBRecord base (serialize_to, get_output, check_type)
   - Primitive type constructors (uint, int, bits, bounded uint, etc.)
   - Ref wrapper (lazy cell reference)
   - HashMap support (if simplifications are desired)

6. **Handle well-known types** optionally — check `ResolvedType.well_known` and
   emit simplified code for Maybe, Bool, Unary, HashmapE, VarUInteger, etc.
   The sema detection is backend-agnostic.

7. **Test against block.tlb** — the 979-line schema with ~380 constructors covers
   every TL-B feature. Deserialize a real block BOC and verify round-trip.

### Tests

- Schema files: `tests/tlb/schemas/*.tlb` (one per feature area)
- Generated code: `tests/tlb/generated/*.py` (gitignored, regenerated by `generate_tl.py`)
- Test files: one per schema (`test_codegen_basic.py`, `test_codegen_generics.py`, etc.)
  plus `test_sema.py`, `test_match_tree.py`, `test_lexer.py`, `test_parser.py`
- Validation tests: `test_codegen_validation.py` — comprehensive serialize/deserialize
  assertion testing (negative nat params, constraint violations, sub-type mismatches,
  TypeInfo equality, mutated-after-deser detection)
- End-to-end: `test_block_e2e.py` deserializes a real testnet block (seq 49375158)
- All test and generated code passes basedpyright with zero warnings

## What's implemented

- [x] Lexer, parser (full TL-B grammar including extensions)
- [x] Sema (type resolution, expression disambiguation, tags, match trees,
      inference, deser plans, constraint solving, topological sort)
- [x] Python codegen: all field types (uint/int/bits/#/##/#</#<=, Cell/Any,
      conditionals, tuples, cell refs)
- [x] Generics: type params, nat params, nested generics, concrete instantiation
- [x] Output params (~n): Unary encoding, inference chains, NatFieldValue,
      compound SolveConstraint
- [x] Inline records with scope isolation and internal nat params
- [x] Special cells (! constructors) with deserialize override
- [x] Bounded uint validation (#< and #<= with range checks)
- [x] Serialize assertions (nat params, constraints, sub-type consistency)
- [x] Deserialize assertions (nat param non-negativity at entry)
- [x] TypeInfo equality for runtime type param verification
- [x] Type simplifications: Maybe→`X|None`, Bool→`bool`, Unary→`int`,
      Bit→`bool`/`bitarray`, HashmapE→`HashmapDict`, VarUInteger/VarInteger→`int`,
      `^Cell`→opaque `Cell`
- [x] Lazy `HashmapDict` with sorted overlay mutations, trie serialization
- [x] CRC32 auto-tags matching C++ `tlbc` reference implementation
- [x] End-to-end block.tlb (979 lines, ~380 constructors, zero basedpyright warnings)
- [x] Real block deserialization verified against C++ reference
- [ ] General enum simplification (non-well-known enums → Python IntEnum)
- [ ] HashmapAugE simplification
- [ ] Other backends (C++, etc.) — same sema output, different codegen
