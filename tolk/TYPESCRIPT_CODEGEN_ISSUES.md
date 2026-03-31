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
- Enum fields in structs → proper load/store with calculated bit width via `calculate_intN_to_serialize_enum`
- `enum E: int8 { ... }` → loads/stores with the specified backing type

### Opcodes
- `struct(0xABCD) Name { ... }` → opcode constant, `$$type` discriminator, load/store with opcode check

### Nested Struct References
- Struct fields referencing other structs → calls `loadOtherStruct(slice)` / `storeOtherStruct(src.field)(builder)`

### Nullable Primitive Types
- `int32?` → `number | null` with bit flag
- `address?` → `Address | null` with `loadMaybeAddress`/`storeAddress`
- `cell?` → `Cell | null` with `loadMaybeRef`/`storeMaybeRef`

### ✅ Union Type Load/Store — FIXED
Union types generate proper load/store functions:

- **Case A: Manual opcodes** — All variants have `struct(0xABCD)`. Uses `preloadUint(prefix_len)` to check opcodes, dispatches to variant load functions. Store uses `$$type` discriminator.
- **Case B: Auto-generated prefix tree** — No manual opcodes. Uses `auto_generate_opcodes_for_union()` to calculate bit prefixes. For `A|B|C`: `00+A | 01+B | 10+C`. For `A|B|null`: `0` for null, `1+prefix+data` for others.
- Store uses TypeScript type guards (`$$type`, `typeof`, `instanceof`, `Buffer.isBuffer`) to dispatch.

### ✅ Complex Nullable Load/Store — FIXED
Nullable struct types (`StructType?` where struct width > 1) now generate:
- Load: `slice.loadBit() ? loadStructType(slice) : null`
- Store: `src.field !== null ? (builder.storeBit(true), storeStructType(src.field)(builder)) : builder.storeBit(false)`

### ✅ Tuple/Tensor Types — FIXED
Tensor types `(T1, T2, ...)` map to TypeScript tuples:
- Type: `[number, bigint]` for `(int32, int64)`
- Load: `[slice.loadInt(32), slice.loadUintBig(64)]`
- Store: sequential stores from tuple elements `t[0]`, `t[1]`, etc.
- Handles nested nullable elements inside tensors

### ✅ Map Serialization — FIXED
`map<K, V>` now generates proper Dictionary serialization:
- Key mapping: `intN`/`uintN` → `Dictionary.Keys.Int(N)`/`Dictionary.Keys.Uint(N)`, `address` → `Dictionary.Keys.Address()`
- Value mapping: for struct values, generates inline `serialize`/`parse` using load/store functions
- For primitive values, generates inline serializers

### ✅ Custom Pack/Unpack Detection — FIXED
Type aliases with custom `.packToBuilder()`/`.unpackFromSlice()` methods are detected before alias unwrapping:
- Generates a warning comment: `/* Note: uses custom serialization (TypeName.unpackFromSlice) */`
- Falls through to underlying type for the actual load/store expression
- Detection uses `get_custom_pack_unpack_function()` from pack-unpack-serializers

### ✅ Void/Unit Type Fields — FIXED
- `void` type → `void` in TypeScript
- Empty tensor `()` → `[]` (empty tuple)
- Void-typed fields are **skipped** in interfaces and load/store functions (they carry no data)
- Custom-serialized void aliases still appear with their custom serialization comment

### Import Generation
- Only imports what's needed (`Address`, `Cell`, `Dictionary`, `BitString`)
- Always imports `Builder` and `Slice`

### Stdlib Filtering
- ✅ stdlib types (`contract`, `blockchain`, `PackOptions`, `UnpackOptions`, `ContractState`, etc.) are excluded
- ✅ Generic stdlib instantiations (`Cell<T>`, `MapLookupResult<T>`) are excluded

---

## Remaining Limitations

### 1. Builder Type — Cannot Load
`builder` fields in structs generate `/* cannot load builder */` — this is inherent to TVM; builders cannot be loaded from slices.

### 2. Struct-Typed Dictionary Keys
`map<StructType, V>` generates `/* struct key type: Name — use custom Dictionary.Keys */`. Struct-typed keys require custom key serialization which can't be auto-generated trivially.

### 3. Ambiguous Enum Union Type Guards
When a union contains multiple enum types (e.g., `EFits8Bits | EStartFromM2`), both type guards resolve to `typeof x === 'number'`. The first branch always wins, which matches the encoding order. This is correct behavior but means TypeScript can't distinguish at runtime.

### 4. Version String
Uses hardcoded "1.2" instead of actual compiler version.

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

### Union Implementation
Uses `auto_generate_opcodes_for_union()` from pack-unpack-serializers to get the same opcode/prefix scheme the compiler uses internally. Two codepaths:
- **Manual opcodes**: Uses `preloadUint` + opcode comparison, stores with `$$type` guard
- **Auto-prefix tree**: Uses calculated prefix bits (`ceil(log2(n))`) with null handling

### Stdlib Filtering
Uses `SrcFile::is_stdlib_file` to detect stdlib types. Both direct stdlib types and generic instantiations of stdlib types are filtered.

### Design Alignment with TON Ecosystem
- Output follows `@ton/core` conventions
- Load/store function naming matches `tlb-codegen` patterns
- Opcode handling uses `$$type` discriminator (Tact convention)
- `loadX`/`storeX` function pair pattern
