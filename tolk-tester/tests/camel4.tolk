// Here we test that a just-return function is not a valid wrapper, it will not be inlined.
// (doesn't use all arguments, has different pureness, has method_id, etc.)

fun myStoreUint(b: builder, x: int, unused: int): builder { return storeUint(b, x, x); }
fun throwIf(excNo: int, cond: int) { assert(!cond) throw excNo; }

fun initial1(x: auto) { return x; }
fun initial2(x: auto) { return initial1(x); }

@pure
fun asm_func_4(a: int, b: (int, (int, int)), c: int): tuple
asm (b a c -> 0) "5 TUPLE";
fun asmFunc4(a: int, b: (int, (int, int)), c: int): tuple { return asm_func_4(a, b, c); }

fun postpone_elections(): int {
    return false;
}

fun setAndGetData(ret: int): int {
    var c: cell = beginCell().storeUint(ret, 8).endCell();
    setContractData(c);
    var s: slice = getContractData().beginParse();
    throwIf(101, 0);
    return s~loadUint(8);
}

fun setAndGetDataWrapper(ret: int): int {
    return setAndGetData(ret);
}

@method_id(101)
fun test1(): int {
    var c: cell = beginCell().myStoreUint(32, 10000000).endCell();
    var s: slice = c.beginParse();
    return s~loadUint(32);
}

get fun test2(ret: int): int {
    return setAndGetDataWrapper(ret);
}

@method_id(103)
fun test3(): int {
    return initial2(10);
}

global t: tuple;

fun foo(x: int): int {
    t~tuplePush(x);
    return x * 10;
}

@method_id(104)
fun test4(): (tuple, tuple) {
    t = createEmptyTuple();
    var t2: tuple = asmFunc4(foo(11), (foo(22), (foo(33), foo(44))), foo(55));
    return (t, t2);
}

@method_id(105)
fun test5(): int {
    if (1) {
        return postpone_elections();
    }
    return 123;
}

@method_id(106)
fun test6(): int {
    return add2(1, 2);  // doesn't inline since declared below
}

fun main(ret: int): int {
    return setAndGetDataWrapper(ret);
}

fun onExternalMessage(ret: int): int {
    return setAndGetData(ret);
}

// currently, functions implemented after usage, can't be inlined, since inlining is legacy, not AST
fun add2(x: int, y: int): int { return x + y; }

/**
    method_id  | in  | out
@testcase | 101 |     | 32
@testcase | 103 |     | 10
@testcase | 104 |     | [ 11 22 33 44 55 ] [ 220 330 440 110 550 ]
@testcase | 105 |     | 0
@testcase | 106 |     | 3
@testcase | 74435 | 99 | 99
@testcase | 0   | 98 | 98
@testcase | -1  | 97 | 97

@fif_codegen DECLPROC myStoreUint
@fif_codegen DECLPROC throwIf
@fif_codegen DECLPROC postpone_elections
@fif_codegen DECLPROC add2
@fif_codegen 74435 DECLMETHOD test2

@fif_codegen
"""
  test3 PROC:<{
    //
    10 PUSHINT  //  _0=10
    initial2 CALLDICT   //  _1
  }>
"""

@fif_codegen
"""
  test2 PROC:<{
    //  ret
    setAndGetData CALLDICT       //  _1
  }>
"""

@fif_codegen
"""
    11 PUSHINT
    foo CALLDICT
    22 PUSHINT
    foo CALLDICT
    33 PUSHINT
    foo CALLDICT
    44 PUSHINT
    foo CALLDICT
    55 PUSHINT
    foo CALLDICT
    asmFunc4 CALLDICT   //  t2
"""

@fif_codegen
"""
  test6 PROC:<{
    //
    1 PUSHINT   //  _0=1
    2 PUSHINT   //  _0=1 _1=2
    add2 CALLDICT       //  _2
  }>
"""

@fif_codegen_avoid setAndGetDataWrapper
*/
