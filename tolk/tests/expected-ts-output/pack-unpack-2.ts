/**
 * Expected TypeScript output for pack-unpack-2.tolk
 * 
 * This file represents what the TypeScript codegen SHOULD produce
 * for the structs defined in pack-unpack-2.tolk.
 * 
 * Key types tested:
 * - Simple structs (JustInt32, TwoInts32And64)
 * - Coins type (TwoInts32AndCoins)
 * - Cell<T> typed references (TwoInts32AndRef64)
 * - Nullable fields (JustMaybeInt32, TwoInts32AndMaybe64)
 * - Address types (JustAddress, TwoInts32And64SepByAddress)
 * - Union types (IntAndEitherInt8Or256, IntAndEither32OrRef64)
 * - bitsN/bytesN (SomeBytesFields)
 * - RemainingBitsAndRefs (IntAndRestInlineCell)
 * - Enums (EStoredAsInt8, EStoredAsUint1)
 * - Variadic integers (WithVariadicInts)
 * - Maps (WithMaps, WithNullableMaps)
 * - Nested structs (DifferentIntsWithMaybe, DifferentMix1/2/3)
 * 
 * STATUS: Many of these are already correctly generated.
 * KNOWN ISSUES (placeholders in actual output):
 * - Union load/store are not yet implemented (show "/* union load - check opcodes */")
 * - Complex nullable load/store are not yet implemented
 * - Tuple types (int32, int64) map to "unknown"
 * - builder type in struct fields is not loadable
 */

import { Builder, Slice, Cell, Address, ExternalAddress, Dictionary, BitString } from '@ton/core';

// === EStoredAsInt8 ===
export enum EStoredAsInt8 {
    M100 = -100,
    Z = 0,
    P100 = 100
}

// === EStoredAsUint1 ===
export enum EStoredAsUint1 {
    ZERO = 0,
    ONE = 1
}

// === MaybeNothing ===
export interface MaybeNothing {
}

export function loadMaybeNothing(slice: Slice): MaybeNothing {
    return {
    };
}

export function storeMaybeNothing(src: MaybeNothing): (builder: Builder) => void {
    return (builder: Builder) => {
    };
}

// === JustInt32 ===
export interface JustInt32 {
    value: number;
}

export function loadJustInt32(slice: Slice): JustInt32 {
    const value = slice.loadInt(32);
    return {
        value,
    };
}

export function storeJustInt32(src: JustInt32): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.value, 32);
    };
}

// === JustMaybeInt32 ===
export interface JustMaybeInt32 {
    value: number | null;
}

export function loadJustMaybeInt32(slice: Slice): JustMaybeInt32 {
    const value = slice.loadBit() ? slice.loadInt(32) : null;
    return {
        value,
    };
}

export function storeJustMaybeInt32(src: JustMaybeInt32): (builder: Builder) => void {
    return (builder: Builder) => {
        src.value !== null ? (builder.storeBit(true), builder.storeInt(src.value, 32)) : builder.storeBit(false);
    };
}

// === TwoInts32AndCoins ===
export interface TwoInts32AndCoins {
    op: number;
    amount: bigint;
}

export function loadTwoInts32AndCoins(slice: Slice): TwoInts32AndCoins {
    const op = slice.loadInt(32);
    const amount = slice.loadCoins();
    return {
        op,
        amount,
    };
}

export function storeTwoInts32AndCoins(src: TwoInts32AndCoins): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.op, 32);
        builder.storeCoins(src.amount);
    };
}

// === TwoInts32And64 ===
export interface TwoInts32And64 {
    op: number;
    query_id: bigint;
}

export function loadTwoInts32And64(slice: Slice): TwoInts32And64 {
    const op = slice.loadInt(32);
    const query_id = slice.loadUintBig(64);
    return {
        op,
        query_id,
    };
}

export function storeTwoInts32And64(src: TwoInts32And64): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.op, 32);
        builder.storeUint(src.query_id, 64);
    };
}

// === TwoInts32AndRef64 ===
export interface TwoInts32AndRef64 {
    op: number;
    query_id_ref: Cell;
}

export function loadTwoInts32AndRef64(slice: Slice): TwoInts32AndRef64 {
    const op = slice.loadInt(32);
    const query_id_ref = slice.loadRef();
    return {
        op,
        query_id_ref,
    };
}

export function storeTwoInts32AndRef64(src: TwoInts32AndRef64): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.op, 32);
        builder.storeRef(src.query_id_ref);
    };
}

// === TwoInts32AndMaybe64 ===
export interface TwoInts32AndMaybe64 {
    op: number;
    query_id: bigint | null;
    demo_bool_field: boolean;
}

export function loadTwoInts32AndMaybe64(slice: Slice): TwoInts32AndMaybe64 {
    const op = slice.loadInt(32);
    const query_id = slice.loadBit() ? slice.loadUintBig(64) : null;
    const demo_bool_field = slice.loadBoolean();
    return {
        op,
        query_id,
        demo_bool_field,
    };
}

export function storeTwoInts32AndMaybe64(src: TwoInts32AndMaybe64): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.op, 32);
        src.query_id !== null ? (builder.storeBit(true), builder.storeUint(src.query_id, 64)) : builder.storeBit(false);
        builder.storeBit(src.demo_bool_field);
    };
}

// === JustAddress ===
export interface JustAddress {
    addr: Address;
}

export function loadJustAddress(slice: Slice): JustAddress {
    const addr = slice.loadAddress();
    return {
        addr,
    };
}

export function storeJustAddress(src: JustAddress): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeAddress(src.addr);
    };
}

// === TwoInts32And64SepByAddress ===
export interface TwoInts32And64SepByAddress {
    op: number;
    addr_e: Address | ExternalAddress | null;
    query_id: bigint;
}

export function loadTwoInts32And64SepByAddress(slice: Slice): TwoInts32And64SepByAddress {
    const op = slice.loadInt(32);
    const addr_e = slice.loadAddressAny();
    const query_id = slice.loadUintBig(64);
    return {
        op,
        addr_e,
        query_id,
    };
}

export function storeTwoInts32And64SepByAddress(src: TwoInts32And64SepByAddress): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.op, 32);
        builder.storeAddress(src.addr_e);
        builder.storeUint(src.query_id, 64);
    };
}

// === Inner1 ===
export interface Inner1 {
    query_id_ref: bigint;
}

export function loadInner1(slice: Slice): Inner1 {
    const query_id_ref = slice.loadUintBig(64);
    return {
        query_id_ref,
    };
}

export function storeInner1(src: Inner1): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeUint(src.query_id_ref, 64);
    };
}

// === Inner2 ===
export interface Inner2 {
    i64_in_ref: bigint;
}

export function loadInner2(slice: Slice): Inner2 {
    const i64_in_ref = slice.loadIntBig(64);
    return {
        i64_in_ref,
    };
}

export function storeInner2(src: Inner2): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.i64_in_ref, 64);
    };
}

// === WithVariadicInts ===
export interface WithVariadicInts {
    ui16: bigint;
    i16: bigint;
    ui32: bigint;
    i32: bigint;
}

export function loadWithVariadicInts(slice: Slice): WithVariadicInts {
    const ui16 = slice.loadVarUintBig(16);
    const i16 = slice.loadVarIntBig(16);
    const ui32 = slice.loadVarUintBig(32);
    const i32 = slice.loadVarIntBig(32);
    return {
        ui16,
        i16,
        ui32,
        i32,
    };
}

export function storeWithVariadicInts(src: WithVariadicInts): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeVarUint(src.ui16, 16);
        builder.storeVarInt(src.i16, 16);
        builder.storeVarUint(src.ui32, 32);
        builder.storeVarInt(src.i32, 32);
    };
}

// === WithEnums ===
export interface WithEnums {
    e1: EStoredAsInt8;
    e2: EStoredAsUint1;
    rem: number;
}

export function loadWithEnums(slice: Slice): WithEnums {
    const e1 = slice.loadInt(8) as EStoredAsInt8;
    const e2 = slice.loadUint(1) as EStoredAsUint1;
    const rem = slice.loadUint(7);
    return {
        e1,
        e2,
        rem,
    };
}

export function storeWithEnums(src: WithEnums): (builder: Builder) => void {
    return (builder: Builder) => {
        builder.storeInt(src.e1, 8);
        builder.storeUint(src.e2, 1);
        builder.storeUint(src.rem, 7);
    };
}

// NOTE: The following struct types have KNOWN LIMITATIONS in the current codegen:
//
// IntAndEitherInt8Or256 - union load/store not implemented (placeholder comments)
// IntAndEither32OrRef64 - union load/store not implemented
// IntAndEither8OrMaybe256 - union load/store not implemented  
// IntAndEitherMaybe8Or256 - union load/store not implemented
// IntAndMaybeMaybe8 - union load/store not implemented
// SomeBytesFields - union for f4 (bits100 | bits200) not implemented
// IntAndRestInlineCell - correctly generated
// IntAndRestRefCell - correctly generated
// IntAndRestEitherCellOrRefCell - union load/store not implemented
// DifferentMaybeRefs - correctly generated (nullable cell uses loadMaybeRef)
// DifferentIntsWithMaybe - complex nullable for struct types not implemented
// DifferentMix1/2/3 - various union/complex nullable issues
// WriteWithBuilder - builder type cannot be loaded
// WithMaps - Dict key/value serializers need custom DictionaryValue impls
// WithNullableMaps - complex nullable for Dict types not implemented
// DifferentMix3.pairm - tuple type (int32, int64) maps to "unknown"
