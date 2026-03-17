# Tolk → TypeScript Wrapper Codegen — Scope & Design

## Problem

Tolk contracts define structs with rich type information (opcodes, field types, serialization layout). 
But developers currently hand-write TypeScript wrappers for off-chain interaction — duplicating 
knowledge the compiler already has. This is error-prone, tedious, and blocks Alexander's stated vision 
of eliminating the JS-centric workflow.

The docs explicitly note: "Auto-generated TypeScript wrappers will be available in the future."

## Approach: Compiler-Native `--emit-typescript`

Add a new pipeline stage that emits TypeScript after type inference + serialization checks complete.
This runs as a pure read-only pass over `G.all_structs` and `G.all_enums` — no mutation, 
no interference with existing Fift codegen.

### Why compiler-native (not external tool)?

1. **Full type information** — the compiler has resolved all types, generics instantiations, 
   union variants, opcode assignments. An external parser would need to replicate all of this.
2. **Consistent with roadmap** — Alexander explicitly said the compiler will generate TypeScript.
   Building it as a compiler pass means it ships with the compiler.
3. **Single source of truth** — no version drift between .tolk definitions and generated TS.
4. **Already precedented** — the compiler already has `pipe-generate-fif-output.cpp` as a codegen stage.

## Architecture

### New files

```
tolk/pipe-generate-ts-output.cpp    — main codegen pass
tolk/ts-type-mapping.h              — Tolk type → TypeScript type mapping
```

### Pipeline integration (tolk.cpp)

```cpp
// After all checks pass, optionally emit TypeScript
if (G.settings.emit_typescript) {
    pipeline_generate_ts_output();
}
pipeline_generate_fif_output_to_std_cout();
```

### Compiler flag

```
--emit-typescript [output_path]     — generate .ts wrappers alongside Fift output
```

For WASM (tolk-js), add to JSON config:
```json
{ "emitTypescript": true }
```

And include in result JSON:
```json
{ "typescriptCode": "..." }
```

## Type Mapping (Tolk → TypeScript)

| Tolk Type | TypeScript Type | @ton/core Load | @ton/core Store |
|-----------|----------------|----------------|-----------------|
| `int8..256` | `number` (≤32) / `bigint` (>32) | `loadInt(N)` / `loadIntBig(N)` | `storeInt(v, N)` / `storeIntBig(v, N)` |
| `uint8..256` | `number` (≤32) / `bigint` (>32) | `loadUint(N)` / `loadUintBig(N)` | `storeUint(v, N)` / `storeUintBig(v, N)` |
| `coins` | `bigint` | `loadCoins()` | `storeCoins(v)` |
| `varint16/32` | `bigint` | `loadVarIntBig(N)` | `storeVarInt(v, N)` |
| `varuint16/32` | `bigint` | `loadVarUintBig(N)` | `storeVarUint(v, N)` |
| `bool` | `boolean` | `loadBoolean()` | `storeBit(v)` |
| `address` | `Address` | `loadAddress()` | `storeAddress(v)` |
| `address?` | `Address \| null` | `loadMaybeAddress()` | `storeAddress(v)` |
| `any_address` | `Address \| ExternalAddress \| null` | `loadAddressAny()` | `storeAddress(v)` |
| `cell` | `Cell` | `loadRef()` | `storeRef(v)` |
| `cell?` | `Cell \| null` | `loadMaybeRef()` | `storeMaybeRef(v)` |
| `Cell<T>` | `Cell` (with typed load helper) | `loadRef()` | `storeRef(v)` |
| `Cell<T>?` | `Cell \| null` | `loadMaybeRef()` | `storeMaybeRef(v)` |
| `bitsN` | `Buffer` | `loadBuffer(N/8)` or `loadBits(N)` | `storeBuffer(v)` or `storeBits(v)` |
| `bytesN` | `Buffer` | `loadBuffer(N)` | `storeBuffer(v)` |
| `slice` | `Slice` | `loadSlice(...)` | `storeSlice(v)` |
| `builder` | `Builder` | N/A | `storeBuilder(v)` |
| `map<K,V>` | `Dictionary<K,V>` | `Dictionary.load(...)` | `.store(v)` |
| `T?` (nullable) | `T \| null` | conditional on 1 bit | conditional on 1 bit |
| `T1 \| T2` (union) | `T1 \| T2` (discriminated) | switch on opcode | switch on kind |
| `enum E : uintN` | `enum` (numeric) | `loadUint(N)` | `storeUint(v, N)` |
| `RemainingBitsAndRefs` | `Slice` | `s.clone()` (remaining) | `storeSlice(v)` |

## Generated Output Shape

For each non-generic struct:

```typescript
import { Address, Builder, Cell, Slice, Dictionary } from '@ton/core';

// === Point ===
export interface Point {
    x: number;  // int32
    y: number;  // int32
}

export function loadPoint(slice: Slice): Point {
    const x = slice.loadInt(32);
    const y = slice.loadInt(32);
    return { x, y };
}

export function storePoint(src: Point): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.x, 32);
        builder.storeInt(src.y, 32);
    };
}
```

For structs with opcodes:

```typescript
// === TransferNotification (opcode: 0x7362d09c) ===
export const TransferNotificationOpcode = 0x7362d09c;

export interface TransferNotification {
    $$type: 'TransferNotification';
    queryId: bigint;
    amount: bigint;
    sender: Address;
}

export function loadTransferNotification(slice: Slice): TransferNotification {
    if (slice.loadUint(32) !== TransferNotificationOpcode) {
        throw new Error('Invalid opcode');
    }
    const queryId = slice.loadUintBig(64);
    const amount = slice.loadCoins();
    const sender = slice.loadAddress();
    return { $$type: 'TransferNotification', queryId, amount, sender };
}

export function storeTransferNotification(src: TransferNotification): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeUint(TransferNotificationOpcode, 32);
        builder.storeUint(src.queryId, 64);
        builder.storeCoins(src.amount);
        builder.storeAddress(src.sender);
    };
}
```

For union types:

```typescript
export type Asset = AssetSimple | AssetBooking;

export function loadAsset(slice: Slice): Asset {
    // peek first bits to determine variant
    const prefix = slice.preloadUint(4);  // max prefix length
    if ((prefix >> 1) === 0b001) return loadAssetSimple(slice);
    if (prefix === 0b1000) return loadAssetBooking(slice);
    throw new Error('Unknown Asset variant');
}
```

For enums:

```typescript
export enum Color {
    Red = 1,
    Green = 2,
    Blue = 3,
}
```

For Cell<T> fields:

```typescript
export interface NftStorage {
    ownerAddress: Address;
    nextItemIndex: bigint;
    content: Cell;                              // untyped ref
    royalty: Cell;                              // Cell<RoyaltyParams>
}

// Helper to load typed ref
export function loadNftStorageRoyalty(cell: Cell): RoyaltyParams {
    return loadRoyaltyParams(cell.beginParse());
}
```

## Implementation Plan

### Phase 1: Core struct codegen (this PR)
- [ ] `ts-type-mapping.h` — type resolution 
- [ ] `pipe-generate-ts-output.cpp` — iterate `G.all_structs`, emit interfaces + load/store
- [ ] `--emit-typescript` flag in `CompilerSettings`
- [ ] Handle: intN, uintN, coins, bool, address, cell, Cell<T>, bitsN, bytesN
- [ ] Handle: struct opcodes (prefix serialization)
- [ ] Handle: nullable fields (T?)
- [ ] Handle: RemainingBitsAndRefs
- [ ] Tests against pack-unpack test fixtures

### Phase 2: Unions, enums, maps
- [ ] Union type codegen with opcode-based dispatch
- [ ] Enum codegen
- [ ] map<K,V> → Dictionary wrapper
- [ ] Generic struct instantiation output

### Phase 3: WASM integration
- [ ] Add `emitTypescript` to tolk-wasm.cpp JSON config
- [ ] Include `typescriptCode` in output JSON
- [ ] Blueprint plugin that auto-generates wrappers on build

### Phase 4: Advanced
- [ ] Contract-level wrapper (deploy, send message helpers)
- [ ] Get method wrappers
- [ ] Custom serializer support (type aliases with packToBuilder/unpackFromSlice)

## What We're NOT Doing

- Not replacing hand-written wrappers for complex protocols
- Not generating full SDK clients (that's a higher-level tool)
- Not modifying any existing compiler passes
- Not changing serialization semantics

## Key Files to Modify

1. `tolk/compiler-state.h` — add `emit_typescript` to `CompilerSettings`
2. `tolk/pipeline.h` — declare `pipeline_generate_ts_output()`
3. `tolk/tolk.cpp` — call the new pipeline stage
4. `tolk/CMakeLists.txt` — add new .cpp files
5. `tolk/tolk-wasm.cpp` — add JSON config option + output field
6. `tolk/tolk-main.cpp` — add CLI flag (if it exists)

## Compatibility

- Generated TS uses `@ton/core` (the standard TON TypeScript library)
- Output is compatible with Blueprint project structure
- Load/store function signatures match the `tlb-codegen` convention
- No runtime dependencies beyond `@ton/core`
