struct (0x01) PrefixedSome { x: int32 }
type PrefixedOrVoid = PrefixedSome | void

struct PlainSome { x: int32 }
type PlainOrVoid = PlainSome | void

type Int32OrVoid = int32 | void

struct (0b00) ThreeP1 { v: int8 }
struct (0b01) ThreeP2 { v: int16 }
type ThreeWayWithVoid = ThreeP1 | ThreeP2 | void

struct InsideStruct {
    a: int8
    tail: PrefixedOrVoid
}

type Int32OrNullOrVoid = int32 | null | void        // empty / 0 / 1+int32


@noinline
fun run<T>(input: T, ans: slice) {
    repeat (2) {
        var s = input.toCell().beginParse();
        assert (s.bitsEqual(ans)) throw 401;
        assert (s.remainingRefsCount() == ans.remainingRefsCount()) throw 401;
        input = T.fromSlice(s);
    }
    input.toCell().beginParse().skipAny<T>().assertEnd();
}

@inline
fun makeVoid<T>(): T {
    return T.fromSlice(createEmptySlice());
}


@method_id(101)
fun test_PrefixedOrVoid() {
    run<PrefixedOrVoid>(PrefixedSome { x: 0x12345678 } as PrefixedOrVoid, "0112345678".hexToSlice());
    run<PrefixedOrVoid>(makeVoid<PrefixedOrVoid>(), "".hexToSlice());
    return true;
}

@method_id(102)
fun test_PlainOrVoid() {
    run<PlainOrVoid>(PlainSome { x: 0x12345678 } as PlainOrVoid, "12345678".hexToSlice());
    run<PlainOrVoid>(makeVoid<PlainOrVoid>(), "".hexToSlice());
    return true;
}

@method_id(103)
fun test_Int32OrVoid() {
    run<Int32OrVoid>(0x7FFFFFFF, "7FFFFFFF".hexToSlice());
    run<Int32OrVoid>(makeVoid<Int32OrVoid>(), "".hexToSlice());
    return true;
}

@method_id(104)
fun test_ThreeWayWithVoid() {
    run<ThreeWayWithVoid>(ThreeP1 { v: 1 } as ThreeWayWithVoid, "006_".hexToSlice());
    run<ThreeWayWithVoid>(ThreeP2 { v: 0x0102 } as ThreeWayWithVoid, "4040A_".hexToSlice());
    run<ThreeWayWithVoid>(makeVoid<ThreeWayWithVoid>(), "".hexToSlice());

    val v1 = ThreeWayWithVoid.fromSlice((ThreeP1 { v: 5 } as ThreeWayWithVoid).toCell().beginParse());
    val v2 = ThreeWayWithVoid.fromSlice((ThreeP2 { v: 200 } as ThreeWayWithVoid).toCell().beginParse());
    val v3 = ThreeWayWithVoid.fromSlice("".hexToSlice());
    val r1: int = match (v1) { ThreeP1 => v1.v, ThreeP2 => v1.v, void => -1 };
    val r2: int = match (v2) { ThreeP1 => v2.v, ThreeP2 => v2.v, void => -1 };
    val r3: int = match (v3) { ThreeP1 => v3.v, ThreeP2 => v3.v, void => -1 };
    return (r1, r2, r3);
}

@method_id(105)
fun test_InsideStruct() {
    run<InsideStruct>({ a: 7, tail: PrefixedSome { x: 100 } as PrefixedOrVoid }, "070100000064".hexToSlice());
    run<InsideStruct>({ a: 9, tail: makeVoid<PrefixedOrVoid>() }, "09".hexToSlice());
    return true;
}

@method_id(106)
fun test_Int32OrNullOrVoid() {
    // int32 → '1' + int32; null → '0'; void → empty
    run<Int32OrNullOrVoid>(42 as Int32OrNullOrVoid, "800000154_".hexToSlice());
    run<Int32OrNullOrVoid>(null, "4_".hexToSlice());
    run<Int32OrNullOrVoid>(makeVoid<Int32OrNullOrVoid>(), "".hexToSlice());

    val v_void = Int32OrNullOrVoid.fromSlice("".hexToSlice());
    val v_null = Int32OrNullOrVoid.fromSlice(beginCell().storeUint(0, 1).endCell().beginParse());
    val v_int = Int32OrNullOrVoid.fromSlice(beginCell().storeUint(1, 1).storeUint(42, 32).endCell().beginParse());
    val r_void: int = match (v_void) { int32 => v_void as int32, null => -2, void => -1 };
    val r_null: int = match (v_null) { int32 => v_null as int32, null => -2, void => -1 };
    val r_int : int = match (v_int)  { int32 => v_int  as int32, null => -2, void => -1 };
    return (r_void, r_null, r_int);
}

get fun exposeToAbi(p: (Int32OrVoid, ThreeWayWithVoid, Int32OrNullOrVoid)) {}

fun main() {
}

/**
@testcase | 101 | | -1
@testcase | 102 | | -1
@testcase | 103 | | -1
@testcase | 104 | | 5 200 -1
@testcase | 105 | | -1
@testcase | 106 | | -1 -2 42

@abi_json
"""
    {"kind":"union","variants":[{"variant_ty_idx":8,"prefix_num":0,"prefix_len":0,"is_prefix_implicit":true,"stack_type_id":46,"stack_width":1},{"variant_ty_idx":0,"prefix_num":0,"prefix_len":0,"is_prefix_implicit":true,"stack_type_id":10,"stack_width":0}],"stack_width":2},
"""

@abi_json
"""
    {"kind":"union","variants":[{"variant_ty_idx":15,"prefix_num":0,"prefix_len":2,"stack_type_id":131,"stack_width":1},{"variant_ty_idx":17,"prefix_num":1,"prefix_len":2,"stack_type_id":132,"stack_width":1},{"variant_ty_idx":0,"prefix_num":0,"prefix_len":0,"stack_type_id":10,"stack_width":0}],"stack_width":2},
"""

@abi_json
"""
    {"kind":"union","variants":[{"variant_ty_idx":8,"prefix_num":1,"prefix_len":1,"is_prefix_implicit":true,"stack_type_id":46,"stack_width":1},{"variant_ty_idx":21,"prefix_num":0,"prefix_len":1,"is_prefix_implicit":true,"stack_type_id":0,"stack_width":1},{"variant_ty_idx":0,"prefix_num":0,"prefix_len":0,"is_prefix_implicit":true,"stack_type_id":10,"stack_width":0}],"stack_width":2},
"""
 */
