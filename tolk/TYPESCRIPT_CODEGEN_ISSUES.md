# TypeScript Codegen Issues

Issues identified during code review and testing of the TypeScript codegen for Tolk.

## Critical Issues

### 1. Missing `TypeDataInt` Handler (plain `int` type)
**Status:** ✅ FIXED  
**Severity:** High  
**File:** `ts-type-mapping.h`

The plain `int` type (257-bit TVM integer) was not handled in `get_ts_type_info()`.

**Fix applied:** Added handler for `TypeDataInt`:
```cpp
if (type->try_as<TypeDataInt>()) {
    info.ts_type = "bigint";
    info.load_expr = "slice.loadIntBig(257)";
    info.store_expr = "builder.storeInt(src." + field_name + ", 257)";
    return info;
}
```

### 2. BitsN Type Mismatch
**Status:** ✅ FIXED  
**Severity:** Medium  
**File:** `ts-type-mapping.h`

For non-byte-aligned `bitsN` types, the codegen was using `Buffer` as the TypeScript type, but `Slice.loadBits()` returns `BitString`, not `Buffer`.

**Fix applied:**
- Changed non-byte-aligned bitsN to use `BitString` type
- Added `needs_import_bitstring` flag to `TsTypeInfo`
- Updated `pipe-generate-ts-output.cpp` to include `BitString` import when needed

## Medium Issues

### 3. Internal/Stdlib Structs in Output
**Status:** OPEN  
**Severity:** Low  
**File:** `pipe-generate-ts-output.cpp`

Internal stdlib structs like `contract`, `blockchain`, `random`, `PackOptions`, `UnpackOptions` are included in the generated output. These should be filtered out or marked as internal.

**Current filtering:**
```cpp
if (struct_ref->name.find("__") == 0) continue;  // Only filters "__" prefix
```

**Suggested fix:** Also filter known stdlib structs, or add a flag to mark them.

### 4. Generic Store Expression Missing Type Assertion
**Status:** OPEN  
**Severity:** Low  
**File:** `ts-type-mapping.h`

For generic nullables with the 1-bit flag pattern, the store expression uses a comma operator which may confuse TypeScript:
```cpp
info.store_expr = "src." + field_name + " !== null ? (builder.storeBit(true), " + inner.store_expr + ") : builder.storeBit(false)";
```

This should probably be an IIFE or split into multiple statements.

## Code Quality Issues

### 5. Missing Import for BitString
**Status:** ✅ FIXED  
**File:** `ts-type-mapping.h`, `pipe-generate-ts-output.cpp`

Added `needs_import_bitstring` flag to `TsTypeInfo` and updated import generation.

### 6. Dictionary Load/Store Incomplete
**Status:** OPEN  
**File:** `ts-type-mapping.h`

The Dictionary handling generates incomplete code:
```cpp
info.load_expr = "Dictionary.load(/* key dict, value dict */)";
```

This needs proper key/value serializers to be generated.

### 7. Complex Union Load/Store Not Implemented
**Status:** OPEN  
**File:** `ts-type-mapping.h`

Full union types (not just nullable) have placeholder comments:
```cpp
info.load_expr = "/* union load - check opcodes */";
info.store_expr = "/* union store - switch on $$type */";
```

## Verification Checklist

- [x] Build compiles without errors
- [x] New CLI flags `-t` and `-T` work
- [x] Basic struct serialization generates correctly
- [x] Enum serialization generates correctly  
- [x] Opcode constants generate correctly
- [x] IntN types (int32, uint64, etc.) generate correctly
- [x] Plain `int` type handled (FIXED)
- [x] BitsN type mapping correct (FIXED)
- [x] Cell<T> generates correctly (outputs `Cell` — correct for @ton/core)
- [ ] Dictionary serialization complete
- [ ] Union serialization complete

## Fixes Applied in This Review

1. **TypeDataInt handler** - Added support for plain `int` type → `bigint` with 257-bit operations
2. **BitString type** - Non-byte-aligned bitsN now correctly uses `BitString` type instead of `Buffer`
3. **BitString import** - Added `needs_import_bitstring` flag and conditional import generation
4. **-T flag output** - Fixed TypeScript output to stdout when using `-T` flag without `-t`

## Testing Notes

Tested with:
- `pack-unpack-1.tolk` - Basic structs, Cell<T>
- `pack-unpack-8.tolk` - Enums with various backing types

All test files compile and produce TypeScript output, though some features generate placeholder code.
