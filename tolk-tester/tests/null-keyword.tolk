import "@stdlib/lisp-lists"

@method_id(101)
fun test1() {
    var numbers: tuple = createEmptyList();
    numbers = listPrepend(1, numbers);
    numbers = listPrepend(2, numbers);
    numbers = listPrepend(3, numbers);
    numbers = listPrepend(4, numbers);
    var (h: int, numbers redef) = listSplit(numbers);
    h += listGetHead(numbers);

    _ = null;
    (_, _) = (null, null);
    var t = createEmptyTuple();
    do {
        var num: int = numbers.listNext();
        t.tuplePush(num);
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
    var untyped: null = null;
    if (true) {
        return untyped;
    }
    return untyped;
}

@method_id(104)
fun test4(): null {
    var (_, (_, untyped: null)) = (3, (createEmptyTuple, null));
    if (true) {
        return untyped;
    }
    return untyped;
}

@method_id(105)
fun test5() {
    var n: slice = getUntypedNull();
    return !(null == n) ? n.loadInt(32) : 100;
}

@method_id(107)
fun test7() {
    var b = beginCell().storeMaybeRef(null);
    var s = b.endCell().beginParse();
    var c = s.loadMaybeRef();
    return (null == c) as int * 10 + (b != null) as int;
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
  test7 PROC:<{
    ...
    LDOPTREF	//  b _18 _17
    DROP	    //  b c
    ISNULL	    //  b _11
    10 MULCONST	//  b _13
    SWAP	    //  _13 b
    ISNULL	    //  _13 _14
    NOT         // _13 _15
    ADD	        //  _16
  }>
"""
*/
