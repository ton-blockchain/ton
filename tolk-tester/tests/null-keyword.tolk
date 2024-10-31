import "../../crypto/smartcont/stdlib.tolk"
@method_id(101)
fun test1() {
    var numbers: tuple = null;
    numbers = cons(1, numbers);
    numbers = cons(2, numbers);
    numbers = cons(3, numbers);
    numbers = cons(4, numbers);
    var (h, numbers redef) = uncons(numbers);
    h += car(numbers);

    var t = empty_tuple();
    do {
        var num = numbers~list_next();
        t~tpush(num);
    } while (numbers != null);

    return (h, numbers == null, t);
}

@method_id(102)
fun test2(x: int) {
    if (null != x) {
        var y: int = null;
        if (y != null) { return 10; }
        return y;
    }
    try {
        return x + 10;  // will throw, since not a number
    } catch {
        return -1;
    }
    return 100;
}

fun myIsNull(x: int): int {
    return x == null ? -1 : x;
}

@method_id(103)
fun test3(x: int) {
    return myIsNull(x > 10 ? null : x);
}

fun getUntypedNull() {
    var untyped = null;
    if (true) {
        return untyped;
    }
    return untyped;
}

@method_id(104)
fun test4() {
    var (_, (_, untyped)) = (3, (empty_tuple, null));
    if (true) {
        return untyped;
    }
    return untyped;
}

@method_id(105)
fun test5() {
    var n = getUntypedNull();
    return !(null == n) ? n~load_int(32) : 100;
}

@method_id(106)
fun test6(x: int) {
    return x > null;  // this compiles (for now), but fails at runtime
}

@method_id(107)
fun test7() {
    var b = begin_cell().store_maybe_ref(null);
    var s = b.end_cell().begin_parse();
    var c = s~load_maybe_ref();
    return (null == c) * 10 + (b != null);
}

fun main() {
    // now, the compiler doesn't optimize this at compile-time, fif codegen contains ifs
    var i: int = null;
    if (i == null) {
        return 1;
    }
    return 10;
}

/**
@testcase | 101 |      | 7 -1 [ 3 2 1 ]
@testcase | 102 | 5    | (null)
@testcase | 102 | null | -1
@testcase | 103 | 5    | 5
@testcase | 103 | 15   | -1
@testcase | 104 |      | (null)
@testcase | 105 |      | 100
@testcase | 107 |      | -11
@fif_codegen
"""
  test1 PROC:<{
    //
    PUSHNULL	//  numbers
    1 PUSHINT	//  numbers _2=1
    SWAP	//  _2=1 numbers
    CONS	//  numbers
    2 PUSHINT	//  numbers _4=2
    SWAP	//  _4=2 numbers
    CONS	//  numbers
    3 PUSHINT	//  numbers _6=3
    SWAP	//  _6=3 numbers
    CONS	//  numbers
    4 PUSHINT	//  numbers _8=4
    SWAP	//  _8=4 numbers
    CONS	//  numbers
    UNCONS	//  h numbers
    DUP	//  h numbers numbers
    CAR	//  h numbers _12
"""

@fif_codegen
"""
  main PROC:<{
    //
    PUSHNULL	//  i
    ISNULL	//  _2
    IFJMP:<{	//
      1 PUSHINT	//  _3=1
    }>	//
    10 PUSHINT	//  _4=10
  }>
"""

@fif_codegen
"""
  test6 PROC:<{
    //  x
    PUSHNULL	//  x _1
    GREATER	//  _2
  }>
"""

@fif_codegen
"""
  test7 PROC:<{
    ...
    LDOPTREF	//  b _17 _16
    DROP	    //  b c
    ISNULL	    //  b _10
    10 MULCONST	//  b _12
    SWAP	    //  _12 b
    ISNULL	    //  _12 _13
    0 EQINT     // _12 _14
    ADD	        //  _15
  }>
"""
*/
