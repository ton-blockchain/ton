import "@stdlib/lisp-lists"

@method_id(101)
fun test1() {
    var numbers: tuple? = createEmptyList();
    numbers = listPrepend(1, numbers);
    numbers = listPrepend(2, numbers);
    numbers = listPrepend(3, numbers);
    numbers = listPrepend(4, numbers);
    var (h: int, numbers redef) = listSplit(numbers!);
    h += listGetHead(numbers!);

    _ = null;
    (_, _) = (null, null);
    var t = createEmptyTuple();
    do {
        var (num: int, numbers redef) = listSplit(numbers!);
        t.push(num);
    } while (numbers != null);

    return (h, numbers == null, t);
}

@method_id(102)
fun test2(x: int?) {
    if (null != x) {
        var y: int? = null;
        if (y != null) { return 10; }
        if (10 < 20) {   // always true at runtime (not at compile-time)
            return y;
        }
    }
    try {
        return x! + 10;  // will throw, since not a number
    } catch {
        return -1;
    }
    return 100;
}

fun myIsNull(x: int?): int {
    return x == null ? -1 : x!;
}

@method_id(103)
fun test3(x: int) {
    return myIsNull(x > 10 ? null : x);
}

@method_id(104)
fun test4(): null {
    var (_, (_, untyped: null)) = (3, (createEmptyTuple, null));
    if (true) {
        return untyped;
    }
    return untyped;
}

@method_id(107)
fun test7() {
    var b = beginCell().storeMaybeRef(null) as builder?;
    var s = b!.endCell().beginParse();
    var c = s.loadMaybeRef();
    return (null == c) as int * 10 + (b != null) as int;
}

fun test8() {
    __expect_type(null, "null");
    __expect_type([[null]], "[[null]]");
    __expect_type(null as tuple?, "tuple?");
    __expect_type(null as [int]?, "[int]?");
    __expect_type(((null)) as (int, int)?, "(int, int)?");
}

fun main() {
    // the compiler optimizes this at compile-time
    var i: int? = null;
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
@testcase | 107 |      | -11

@fif_codegen
"""
  test1 PROC:<{
    PUSHNULL	//  numbers
    1 PUSHINT	//  numbers '2=1
    SWAP	//  '2=1 numbers
    CONS	//  numbers
    2 PUSHINT	//  numbers '4=2
    SWAP	//  '4=2 numbers
    CONS	//  numbers
    3 PUSHINT	//  numbers '6=3
    SWAP	//  '6=3 numbers
    CONS	//  numbers
    4 PUSHINT	//  numbers '8=4
    SWAP	//  '8=4 numbers
    CONS	//  numbers
    UNCONS	//  h numbers
    DUP	//  h numbers numbers
    CAR	//  h numbers '13
"""

@fif_codegen
"""
  main PROC:<{
    1 PUSHINT   // '3=1
  }>
"""

@fif_codegen
"""
  test7 PROC:<{
    ...
    LDOPTREF	//  b '9 '8
    DROP	    //  b c
    ISNULL	    //  b '11
    10 MULCONST	//  b '13
    SWAP	    //  '13 b
    ISNULL	    //  '13 '14
    NOT         // '13 '14
    ADD	        //  '15
  }>
"""
*/
