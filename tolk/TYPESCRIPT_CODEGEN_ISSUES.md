# TypeScript Codegen — Issues & Status

## Build Status

✅ **Compiles successfully** with CMake on Ubuntu 24.04 (g++ 13.3, cmake 3.28)
✅ **All 8 pack-unpack test files** produce TypeScript output without crashes
✅ **stdlib types are now filtered** — only user-defined structs/enums are exported

### Build instructions
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DTON_ONLY_TONLIB=ON
make tolk -j$(nproc)
```

### Usage
```bash
# Output TypeScript to stdout (after Fift code)
./tolk -T contract.tolk

# Write TypeScript to a file
./tolk -t bindings.ts contract.tolk
```

---

## What Works (Correctly Generated)

### Simple Struct Types
- `int8`..`int32` → `number` with `loadInt`/`storeInt`
- `uint8`..`uint32` → `number` with `loadUint`/`storeUint`
- `int33`..`int257` → `bigint` with `loadIntBig`/`storeInt`
- `uint33`..`uint256` → `bigint` with `loadUintBig`/`storeUint`
- `bool` → `boolean` with `loadBoolean`/`storeBit`
- `coins` → `bigint` with `loadCoins`/`storeCoins`
- `varint16/32`, `varuint16/32` → `bigint` with `loadVarIntBig`/`storeVarInt`

### Address Types
- `address` → `Address` with `loadAddress`/`storeAddress`
- `any_address` → `Address | ExternalAddress | null` with `loadAddressAny`/`storeAddress`

### Cell/Ref Types
- `cell` → `Cell` with `loadRef`/`storeRef`
- `Cell<T>` (typed cell refs) → `Cell` with `loadRef`/`storeRef`
- `slice` → `Slice`
- `builder` → `Builder`

### Bytes/Bits Types
- `bytesN` → `Buffer` with `loadBuffer`/`storeBuffer`
- `bitsN` (byte-aligned) → `Buffer` with `loadBuffer`/`storeBuffer`
- `bitsN` (non-byte-aligned) → `BitString` with `loadBits`/`storeBits`

### RemainingBitsAndRefs
- `RemainingBitsAndRefs` → `Slice` with `slice.clone()`

### Enums
- Simple enums with explicit values → TypeScript `enum` with numeric values
- Enum fields in structs → proper load/store with backing type cast
- `enum E: int8 { ... }` → loads/stores with the specified backing type

### Opcodes
- `struct(0xABCD) Name { ... }` → opcode constant, `$$type` discriminator, load/store with opcode check

### Nested Struct References
- Struct fields referencing other structs → calls `loadOtherStruct(slice)` / `storeOtherStruct(src.field)(builder)`

### Nullable Primitive Types
- `int32?` → `number | null` with bit flag
- `address?` → `Address | null` with `loadMaybeAddress`/`storeAddress`
- `cell?` → `Cell | null` with `loadMaybeRef`/`storeMaybeRef`

### Import Generation
- Only imports what's needed (`Address`, `Cell`, `Dictionary`, `BitString`)
- Always imports `Builder` and `Slice`

### Stdlib Filtering
- ✅ Fixed: stdlib types (`contract`, `blockchain`, `PackOptions`, `UnpackOptions`, `ContractState`, etc.) are now excluded
- ✅ Fixed: Generic stdlib instantiations (`Cell<T>`, `MapLookupResult<T>`) are excluded

---

## Known Limitations (Placeholders in Output)

### 1. Union Type Load/Store — Priority: HIGH
**Status:** Placeholder comments (`/* union load - check opcodes */`)

Union types like `int8 | int256`, `Cell<TwoInts32AndCoins> | Cell<JustInt32>`, `coins | uint64` generate correct TypeScript type annotations but lack load/store implementations.

**What's needed:**
- For opcode-discriminated unions: generate `switch` on opcode prefix
- For auto-prefixed unions (2-bit, 3-bit tags): generate bit-prefix matching
- For `Either<L, R>` pattern: generate 1-bit tag check

**Affected structs across test files:** `IntAndEitherInt8Or256`, `MsgSinglePrefix48`, `IntAndEither32OrRef64`, `IntAndEither8OrMaybe256`, `IntAndEitherMaybe8Or256`, `IntAndMaybeMaybe8`, `SomeBytesFields.f4`, `IntAndRestEitherCellOrRefCell.rest`, `DifferentMix3.bod`, `WithNullableMaps.m4`, `WithEnumsUnion.u`

### 2. Complex Nullable Load/Store — Priority: MEDIUM
**Status:** Placeholder comments (`/* complex nullable: T | null */`)

Nullable types where the inner type is a struct (not a primitive) don't have load/store implementations.

**What's needed:**
- For `StructType?`: generate 1-bit flag + struct load/store
- For `(int32, int64)?`: handle tuple nullables (requires tuple support first)

**Affected:** `DifferentIntsWithMaybe.jmiMaybe`, `DifferentMix1.ja2m`, `DifferentMix3.tim`, `DifferentMix3.pairm`

### 3. Tuple Types — Priority: MEDIUM
**Status:** Maps to `unknown /* (int32, int64) */`

Tensor/tuple types `(T1, T2, ...)` are not yet mapped to TypeScript types.

**Possible approaches:**
- Map to TypeScript tuples: `[number, bigint]`
- Generate inline load/store for each element
- Handle nested tuples recursively

**Affected:** `DifferentMix3.pairm`, `WithMaps.m2` (key type `JustInt32`, value type `(int8, int8)`)

### 4. Custom Pack/Unpack (Type Aliases with Methods) — Priority: LOW
**Status:** Falls through to underlying type, ignoring custom pack/unpack methods

Types like `TelegramString`, `Custom8`, `MyBorderedInt`, `UnsafeColor` that define `.packToBuilder()` and `.unpackFromSlice()` are treated as their underlying type.

**What's needed:** Detect when a type alias has custom pack/unpack methods and either:
- Generate a note that custom serialization is used
- Skip these fields with a comment
- Or implement some form of custom function references

### 5. Map Serialization — Priority: LOW
**Status:** Dict type annotation works, but load/store are placeholder-quality

`map<K, V>` generates `Dictionary<TsK, TsV>` but the load/store use generic placeholders that don't specify key/value serializers.

**What's needed:** Generate proper `Dictionary.Keys.*` and `Dictionary.Values.*` specifications.

### 6. Void/Unit Type Fields — Priority: LOW  
**Status:** Maps to `unknown /* () */`

Fields with type `()` (void/unit) like `MyCustomNothing`, `MagicGlobalModifier` are shown as unknown. These are used for side-effect-only custom serialization.

---

## Files Modified

### New Files
- `tolk/pipe-generate-ts-output.cpp` — Main codegen pipeline stage
- `tolk/ts-type-mapping.h` — Type mapping and load/store expression generation

### Modified Files
- `tolk/compiler-state.h` — Added `emit_typescript`, `typescript_output_filename`, `generated_typescript_code` to `CompilerSettings`
- `tolk/pipeline.h` — Added `pipeline_generate_ts_output()` declaration
- `tolk/tolk.cpp` — Called `pipeline_generate_ts_output()` after Fift output
- `tolk/tolk-main.cpp` — Added `-t<file>` and `-T` CLI flags
- `tolk/tolk-wasm.cpp` — Added `emitTypescript` option for WASM builds
- `tolk/CMakeLists.txt` — Added `pipe-generate-ts-output.cpp` to sources

### Test/Reference Files
- `tolk/tests/expected-ts-output/pack-unpack-{1..8}.ts` — Expected TypeScript output for each test fixture
- `tolk/tests/actual-ts-output/pack-unpack-{1..8}.ts` — Current actual output (for comparison)

---

## Architecture Notes

### Pipeline Position
`pipeline_generate_ts_output()` runs **after** all type checking and Fift generation are complete. It reads the fully resolved type system and generates TypeScript as a separate output stream.

### Type Mapping Strategy
The type mapping in `ts-type-mapping.h` uses `try_as<>()` pattern matching on `TypeData*` subclasses. Each type maps to:
- A TypeScript type string
- A load expression (reading from Slice)
- A store expression (writing to Builder)
- Import flags (which @ton/core types are needed)

### Stdlib Filtering
Uses `SrcFile::is_stdlib_file` to detect stdlib types. Both direct stdlib types and generic instantiations of stdlib types (e.g., `Cell<T>`, `MapLookupResult<T>`) are filtered.

### Design Alignment with TON Ecosystem
- Output follows `@ton/core` conventions
- Load/store function naming matches `tlb-codegen` patterns
- Opcode handling uses `$$type` discriminator (Tact convention)
- `loadX`/`storeX` function pair pattern

---

## Next Steps (Priority Order)

1. **Union load/store** — The biggest gap. Need to examine how Tolk resolves union type tags at the AST level and generate corresponding TypeScript switch/if chains.

2. **Complex nullable load/store** — Once unions work, complex nullables (which are internally `T | null` unions) should follow naturally.

3. **Tuple type support** — Map `(T1, T2)` to TypeScript tuples `[ts_T1, ts_T2]` with sequential load/store.

4. **Map key/value serializers** — Use `Dictionary.Keys.Int(bits)` / `Dictionary.Values.BigInt(bits)` etc.

5. **Type alias custom serialization** — Detect and handle custom pack/unpack methods.

6. **Version string** — Use actual compiler version instead of hardcoded "1.2".
