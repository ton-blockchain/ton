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
    __expect_type([[null]], "array<array<null>>");
    __expect_type(null as tuple?, "tuple?");
    __expect_type(null as [int]?, "[int]?");
    __expect_type(((null)) as (int, int)?, "(int, int)?");
}

struct WrappedUnknown {
    u: unknown
}

@method_id(109)
fun test9() {
    var w = (
        { u: 123 } as WrappedUnknown?,
        { u: null } as WrappedUnknown?,
        null as WrappedUnknown?,
    );
    __expect_type(w, "(WrappedUnknown?, WrappedUnknown?, WrappedUnknown?)");
    return (w.0 == null, w.1 == null, w.2 == null, w.0!.u == null, w.1!.u == null, null as WrappedUnknown? as unknown)
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
@testcase | 102 | 5    | (null)
@testcase | 102 | null | -1
@testcase | 103 | 5    | 5
@testcase | 103 | 15   | -1
@testcase | 104 |      | (null)
@testcase | 107 |      | -11
@testcase | 109 |      | 0 0 -1 0 -1 [ (null) 0 ]

@fif_codegen
"""
  main() PROC:<{
    1 PUSHINT   // '3=1
  }>
"""

@fif_codegen
"""
  test7() PROC:<{
    ...
    PLDOPTREF	//  b c
    ISNULL	    //  b '10
    10 MULCONST	//  b '12
    SWAP	    //  '12 b
    ISNULL	    //  '12 '13
    NOT         //  '12 '13
    ADD	        //  '14
  }>
"""
*/
