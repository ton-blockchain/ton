import "imports/use-dicts.tolk"

fun simpleAllConst() {
    return (!0, !!0 & !false, !!!0, !1, !!1, !-1, !!-1, (!5 as int == 0) == !0, !0 == true);
}

fun compileTimeEval1(x: int) {
    // todo now compiler doesn't understand that bool can't be equal to number other than 0/-1
    // (but understands that it can't be positive)
    // that's why for now, the last condition is evaluated at runtime
    return (!x, !x as int > 10, (!x as int) < 10, !!x as int == 5, !x as int == -10);
}

@method_id(101)
fun withIfNot(x: int, y: int) {
    if (!x) { return 10 }
    else if (!y) { { return 20 } }
    return x+y
}

@method_id(102)
fun withAndOr(x: int, y: int, z: int) {
    var return_at_end = -1;
    if (!x & !y) {
        if (!z & !y) { return 10; }
        else if ((z != 0) | !!y) { return_at_end = 20; }
    } else if (!!x & !!y & !z) {
        if (!z & (x > 10)) { return_at_end = 30; }
        if ((x != 11) & !z) { return 40; }
        return_at_end = 50;
    } else {
        return_at_end = !x ? !y as int : (!z as int) | 1;
    }
    return return_at_end;
}

@method_id(103)
fun someSum(upto: int) {
    var x = 0;
    var should_break = false;
    while (!x & !should_break) {
        if (upto < 10) { x = upto; should_break = true; }
        else { upto = upto - 1; }
    }
    return x;
}

@method_id(104)
fun testDict(last: int) {
    // prepare dict: [3 => 30, 4 => 40, 5 => x]
    var dict = prepareDict_3_30_4_40_5_x(!last ? 100 : last);
    return (lookupIdxByValue(dict, 30), lookupIdxByValue(dict, last), lookupIdxByValue(dict, 100));
}

@method_id(105)
fun testNotNull(x: int?) {
    return [x == null, null == x, !(x == null), null == null, (null != null) as int];
}

@method_id(106)
fun testAndConstCodegen() {
    return (
        [1 && 0, 0 && 1, 0 && 0, 1 && 1],
        [4 && 3 && 0, 5 && 0 && 7 && 8, (7 && 0) && -19],
        [4 && 3 && -1, 5 && -100 && 7 && 8, (7 && (1 + 2)) && -19],
        [true && false, true && true]
    );
}

@method_id(107)
fun testOrConstCodegen() {
    return (
        [1 || 0, 0 || 1, 0 || 0, 1 || 1],
        [0 || 0 || 0, 0 || (0 || 0), ((0 || 0) || 0) || 0],
        [4 || 3 || -1, 0 || -100 || 0 || 0, (0 || (1 + -1)) || -19],
        [true || false, false || false]
    );
}

global eqCallsCnt: int;

fun eq(x: int) { return x; }
fun eqCnt(x: int) { eqCallsCnt += 1; return x; }
fun isGt0(x: int) { return x > 0; }

fun alwaysThrows(): int { throw 444 ; return 444; }

@method_id(108)
fun testAndSimpleCodegen(a: int, b: int) {
    return a && b;
}

@method_id(109)
fun testOrSimpleCodegen(a: int, b: int) {
    return a > 0 || b > 0;
}

@method_id(110)
fun testLogicalOps1(x: int) {
    eqCallsCnt = 0;
    return (
        isGt0(x) || !isGt0(x) || alwaysThrows(),
        x && eqCnt(x) && eqCnt(x - 1) && eqCnt(x - 2),
        (400 == eq(x)) && alwaysThrows(),
        (500 == eq(x)) || eqCnt(x) || false,
        (500 == eq(x)) || eqCnt(x) || true,
        eqCallsCnt
    );
}

@method_id(111)
fun testLogicalOps2(first: int) {
    var s = beginCell().storeInt(1, 32).storeInt(2, 32).storeInt(3, 32).storeInt(4, 32).storeInt(5, 32).endCell().beginParse();
    var sum = 0;
    if (first && s.loadUint(32)) {
        (2 == s.loadUint(32)) && (sum += s.loadUint(32));
        (3 == s.loadUint(32)) && (sum += s.loadUint(32));
        (5 == s.preloadUint(32)) && (sum += s.loadUint(32));
    } else {
        (10 == s.loadUint(32)) || (20 == s.loadUint(32)) || (3 == s.loadUint(32)) || (4 == s.loadUint(32));
        sum += s.loadUint(32);
    }
    return (s.remainingBitsCount(), sum);
}

@method_id(112)
fun mixLogicalIntsAndBools(first: int, cond: bool) {
    return (
        (first && cond) || (!first && cond),
        ((first & -1) & cond as int) == ((first && true) && cond) as int,
        7 && cond,
        first || cond || !cond || alwaysThrows(),
        cond || first || !first || alwaysThrows()
    );
}

@method_id(113)
fun testConvertIfToIfnot(x: bool) {
    assert(!!(x == false), 100);
    assert(!x, 100);
    if (x == !!false) {
        return 1;
    }
    if (!!(x != !false)) {
        return 1;
    }
    assert(!!x, 100);
    return -4;
}

@pure
fun get123(): int
    asm "123 PUSHINT";

@method_id(114)
fun test114() {
    val fals = (get123() < 0) as int;
    return (fals ? -1 : fals) != 0;
}

@method_id(115)
fun test115() {
    val tru = get123() >= 0;
    val fals = get123() < 0;

    if ((true || false) && (false || false)) {
        throw 123;
    }
    if ((tru || fals) && (fals || fals)) {
        throw 456;
    }
    return (tru, fals);
}


fun main() {

}

/**
@testcase | 101 | 0 0    | 10
@testcase | 101 | 5 0    | 20
@testcase | 101 | 5 8    | 13
@testcase | 102 | 0 0 0  | 10
@testcase | 102 | 0 0 5  | 20
@testcase | 102 | 1 2 0  | 40
@testcase | 102 | 11 2 0 | 50
@testcase | 102 | 1 0 0  | -1
@testcase | 102 | 0 1 0  | 0
@testcase | 102 | 1 0 1  | 1
@testcase | 103 | 15     | 9
@testcase | 103 | 6      | 6
@testcase | 103 | -1     | -1
@testcase | 104 | 50     | 3 5 -1
@testcase | 104 | 100    | 3 5 5
@testcase | 104 | 0      | 3 -1 5
@testcase | 105 | 0      | [ 0 0 -1 -1 0 ]
@testcase | 105 | null   | [ -1 -1 0 -1 0 ]
@testcase | 106 |        | [ 0 0 0 -1 ] [ 0 0 0 ] [ -1 -1 -1 ] [ 0 -1 ]
@testcase | 107 |        | [ -1 -1 0 -1 ] [ 0 0 0 ] [ -1 -1 -1 ] [ -1 0 ]
@testcase | 108 | 1 2    | -1
@testcase | 108 | 1 0    | 0
@testcase | 109 | -5 -4  | 0
@testcase | 109 | -5 4   | -1
@testcase | 109 | 1 99   | -1
@testcase | 110 | 0      | -1 0 0 0 -1 2
@testcase | 110 | 1      | -1 0 0 -1 -1 4
@testcase | 110 | 2      | -1 0 0 -1 -1 5
@testcase | 110 | 500    | -1 -1 0 -1 -1 3
@testcase | 111 | 0      | 32 4
@testcase | 111 | -1     | 0 8
@testcase | 112 | 5 0    | 0 -1 0 -1 -1
@testcase | 112 | 0 -1   | -1 -1 -1 -1 -1
@testcase | 113 | 0      | 1
@testcase | 114 |        | 0
@testcase | 115 |        | -1 0

@fif_codegen
"""
  simpleAllConst PROC:<{
    TRUE
    0 PUSHINT
    TRUE
    FALSE
    TRUE
    FALSE
    TRUE
    TRUE
    TRUE
  }>
"""

@fif_codegen
"""
  compileTimeEval1 PROC:<{  //  x
    DUP	//  x x
    0 EQINT	//  x '1
    FALSE	//  x '1 '4
    TRUE	//  x '1 '4 '7
    FALSE	//  x '1 '4 '7 '11
    s0 s4 XCHG	//  '11 '1 '4 '7 x
    0 EQINT	//  '11 '1 '4 '7 '12
    -10 EQINT	//  '11 '1 '4 '7 '14
    s3 s4 XCHG
    s1 s3 s0 XCHG3	//  '1 '4 '7 '11 '14
  }>
"""

@fif_codegen
"""
  withIfNot PROC:<{  //  x y
    c2 SAVE
    SAMEALTSAVE
    OVER	//  x y x
    IFNOTJMP:<{	//  x y
      2DROP	//
      10 PUSHINT	//  '2=10
    }>	//  x y
    DUP	//  x y y
    IFNOTJMP:<{	//  x y
      2DROP	//
      20 PUSHINT	//  '3=20
      RETALT
    }>	//  x y
    ADD	//  '4
  }>
"""

@fif_codegen
"""
  testAndConstCodegen PROC:<{
    FALSE
    0 PUSHINT
    DUP
    TRUE
    4 TUPLE
    FALSE
    0 PUSHINT
    DUP
    TRIPLE
    TRUE
    TRUE
    TRUE
    TRIPLE
    FALSE
    TRUE
    PAIR
  }>
"""

@fif_codegen
"""
  testOrConstCodegen PROC:<{
    -1 PUSHINT
    TRUE
    FALSE
    s2 PUSH
    4 TUPLE
    FALSE
    FALSE
    FALSE
    TRIPLE
    -1 PUSHINT
    DUP
    TRUE
    TRIPLE
    -1 PUSHINT
    FALSE
    PAIR
  }>
"""

Currently, && operator is implemented via ?: and is not optimal in primitive cases.
For example, `a && b` can be expressed without IFs.
These are moments of future optimizations. For now, it's more than enough.
@fif_codegen
"""
  testAndSimpleCodegen PROC:<{  //  a b
    SWAP	//  b a
    IF:<{	//  b
      0 NEQINT	//  '2
    }>ELSE<{	//  b
      DROP	//
      0 PUSHINT	//  '2=0
    }>
  }>
"""

@fif_codegen
"""
  testOrSimpleCodegen PROC:<{   //  a b
    SWAP	//  b a
    0 GTINT	//  b '3
    IF:<{	//  b
      DROP	//
      -1 PUSHINT	//  '4=-1
    }>ELSE<{	//  b
      0 GTINT	//  '7
      0 NEQINT	//  '4
    }>
  }>
"""

@fif_codegen
"""
  testConvertIfToIfnot PROC:<{  //  x
    DUP	//  x x
    100 THROWIF
    DUP	//  x x
    100 THROWIF
    DUP	//  x x
    IFNOTJMP:<{	//  x
      DROP	//
      1 PUSHINT	//  '5=1
    }>	//  x
    DUP	//  x x
    IFNOTJMP:<{	//  x
      DROP	//
      1 PUSHINT	//  '6=1
    }>	//  x
    100 THROWIFNOT
    -4 PUSHINT	//  '9=-4
  }>
"""

 */
