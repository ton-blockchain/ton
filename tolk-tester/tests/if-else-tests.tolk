global t: tuple;

@method_id(101)
fun test1(x: int): int {
    if (x > 200) {
        return 200;
    } else if (x > 100) {
        return 100;
    } else if (!(x <= 50)) {
        if (!(x > 90)) {
            return x;
        } else {
            return 90;
        }
    } else {
        return 0;
    }
}

@method_id(102)
fun test2(x: int) {
    if (x == 20) { return 20; }
    if (x != 50) { return 50; }
    if (x == 0) { return 0; }
    return -1;
}

@method_id(103)
fun test3(x: int) {
    if (!(x != 20)) { return 20; }
    if (!(x == 50)) { return 50; }
    if (!x) { return 0; }
    return -1;
}

fun elseif(cond: int) {
    if (cond > 0) {
        throw(cond);
    }
}

@inline
@method_id(104)
fun test4(x: int): int {
    if (x==1) {
        return 111;
    } else {
        x *= 2;
    }
    return x + 1;
}

@method_id(105)
fun test5(x: int): (int, int) {
    return (test4(x), 222);
}

@method_id(106)
fun test6(x: int) {
    try {
        if (x < 0) { return -1; }
        elseif (x);
    } catch(excNo) {
        return excNo * 1000;
    }
    return 0;
}

fun doNothing(): void {}

@method_id(107)
fun test7() {
    if (random.uint256()) { return doNothing() }
    // here we test that no "missing return" error
}

fun withNoElse(op: int): int {
    if (op == 123) { return 100 }
    if (op == 234) { return 200 }
    if (op == 345) { return 300 }
    throw 0xFF
}

fun withElse(op: int): int {
    if (op == 123) { return 100 }
    else if (op == 234) { return 200 }
    else if (op == 345) { return 300 }
    return 0xFF
}

fun withMatch(op: int): int {
    match (op) {
        123 => return 100,
        234 => return 200,
        345 => return 300,
    }
    throw 0xFF
}

@method_id(108)
fun test8(op: int) {
    return (withNoElse(op), withElse(op), withMatch(op));
}

@noinline
fun demo9_1(x: int) {
    if (x > 0) { t.push(123); }
    else { t.push(456) }
}

@noinline
fun demo9_2(x: int) {
    if (x > 0) { t.push(123); }
}

@noinline
fun demo9_3(x: int) {
    if (x > 0) { t.push(123); }
    return;
}

@noinline
fun demo9_4(x: int) {
    if (x != -100) {
        if (x > 0) { t.push(123); }
        return;
    }
}

@method_id(109)
fun test9(x: int) {
    t = createEmptyTuple();
    demo9_1(x); demo9_2(x); demo9_3(x); demo9_4(x);
    return t;
}

@noinline
fun demo10_1(x: int) {
    match(x) {
        0 => t.push(123),
        1 => t.push(456),
    }
}

@noinline
fun demo10_2(x: int) {
    if (x != -100) {
        match (x) {
            0 => t.push(123),
            else => t.push(456)
        }
        return;
    }
    t.push(789)
}

@method_id(110)
fun test10(x: int) {
    t = createEmptyTuple();
    demo10_1(x); demo10_2(x);
    return t;
}

@noinline
fun demo_neg_11_1(mutate x: int) {
    match (x) {
        -1 => { x = 0; }
        else => { x = 1; }
    }
}

fun justVoidPush() {
    t.push(100);
}

fun demo_neg_11_2(x: int) {
    match (x) {
        -1 => t.push(123),
        else => t.push(456)
    }
    return justVoidPush();
}

@method_id(111)
fun test11(x: int) {
    t = createEmptyTuple();
    demo_neg_11_1(mutate x);
    demo_neg_11_2(x);
    return (x, t);
}

fun demo12_1(x: int) {
    __expect_inline(true);
    if (x > 0) { t.push(123); }
    else { t.push(456) }
}

fun demo12_2(x: int) {
    __expect_inline(true);
    if (x > 0) { t.push(123); }
}

fun demo12_3(x: int) {
    __expect_inline(true);
    if (x > 0) { t.push(123); }
    return;
}

fun demo12_4(x: int) {
    __expect_inline(false);
    if (x != -100) {
        if (x > 0) { t.push(123); }
        return;
    }
}

@method_id(112)
fun test12(x: int) {
    t = createEmptyTuple();
    demo12_1(x); demo12_2(x); demo12_3(x); demo12_4(x);
    return t;
}

fun main() {

}

/**
@testcase | 101 | 0 | 0
@testcase | 101 | 1000 | 200
@testcase | 101 | 150 | 100
@testcase | 101 | -1 | 0
@testcase | 101 | 87 | 87
@testcase | 101 | 94 | 90
@testcase | 102 | 20 | 20
@testcase | 102 | 40 | 50
@testcase | 102 | 50 | -1
@testcase | 103 | 20 | 20
@testcase | 103 | 40 | 50
@testcase | 103 | 50 | -1
@testcase | 104 | 1  | 111
@testcase | 104 | 3  | 7
@testcase | 105 | 1  | 111 222
@testcase | 105 | 3  | 7 222
@testcase | 106 | -5 | -1
@testcase | 106 | 5  | 5000
@testcase | 108 | 123 | 100 100 100
@testcase | 108 | 345 | 300 300 300
@testcase | 109 | 10  | [ 123 123 123 123 ]
@testcase | 109 | -10 | [ 456 ]
@testcase | 110 | 0   | [ 123 123 ]
@testcase | 110 | 5   | [ 456 ]
@testcase | 111 | 5   | 1 [ 456 100 ]
@testcase | 112 | 10  | [ 123 123 123 123 ]
@testcase | 112 | -10 | [ 456 ]

@fif_codegen
"""
  test3() PROC:<{ //  x
    DUP	        //  x x
    20 NEQINT	//  x '2
    IFNOTJMP:<{	//  x
      DROP	    //
      20 PUSHINT	//  '3=20
    }>	        //  x
    DUP	        //  x x
    50 EQINT	//  x '5
    IFNOTJMP:<{	//  x
"""

@fif_codegen
"""
  withNoElse() PROC:<{
    DUP
    123 EQINT
    IFJMP:<{
      DROP
      100 PUSHINT
    }>
    DUP
    234 PUSHINT
    EQUAL
    IFJMP:<{
      DROP
      200 PUSHINT
    }>
    345 PUSHINT
    EQUAL
    IFJMP:<{
      300 PUSHINT
    }>
    255 THROW
  }>
"""

@fif_codegen
"""
  withElse() PROC:<{
    DUP
    123 EQINT
    IFJMP:<{
      DROP
      100 PUSHINT
    }>
    DUP
    234 PUSHINT
    EQUAL
    IFJMP:<{
      DROP
      200 PUSHINT
    }>
    345 PUSHINT
    EQUAL
    IFJMP:<{
      300 PUSHINT
    }>
    8 PUSHPOW2DEC
  }>
"""

@fif_codegen
"""
  withMatch() PROC:<{
    DUP
    123 EQINT
    IFJMP:<{
      DROP
      100 PUSHINT
    }>
    DUP
    234 PUSHINT
    EQUAL
    IFJMP:<{
      DROP
      200 PUSHINT
    }>
    345 PUSHINT
    EQUAL
    IFJMP:<{
      300 PUSHINT
    }>
    255 THROW
  }>
"""

@fif_codegen
"""
  demo9_1() PROC:<{
    0 GTINT
    IFJMP:<{
      $t GETGLOB
      123 PUSHINT
      TPUSH
      $t SETGLOB
    }>
    $t GETGLOB
    456 PUSHINT
    TPUSH
    $t SETGLOB
  }>
"""

@fif_codegen
"""
  demo9_2() PROC:<{
    0 GTINT
    IFJMP:<{
      $t GETGLOB
      123 PUSHINT
      TPUSH
      $t SETGLOB
    }>
  }>
"""

@fif_codegen
"""
  demo9_3() PROC:<{
    0 GTINT
    IFJMP:<{
      $t GETGLOB
      123 PUSHINT
      TPUSH
      $t SETGLOB
    }>
  }>
"""

@fif_codegen
"""
  demo9_4() PROC:<{
    DUP
    -100 NEQINT
    IFJMP:<{
      0 GTINT
      IFJMP:<{
        $t GETGLOB
        123 PUSHINT
        TPUSH
        $t SETGLOB
      }>
    }>
    DROP
  }>
"""

@fif_codegen
"""
  demo10_1() PROC:<{
    DUP
    0 EQINT
    IFJMP:<{
      DROP
      $t GETGLOB
      123 PUSHINT
      TPUSH
      $t SETGLOB
    }>
    1 EQINT
    IFJMP:<{
      $t GETGLOB
      456 PUSHINT
      TPUSH
      $t SETGLOB
    }>
  }>
"""

*/
