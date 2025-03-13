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
    if (random()) { return doNothing(); }
    // here we test that no "missing return" error
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

@fif_codegen
"""
  test3 PROC:<{
    //  x
    DUP	//  x x
    20 NEQINT	//  x '2
    IFNOTJMP:<{	//  x
      DROP	//
      20 PUSHINT	//  '3=20
    }>	//  x
    DUP	//  x x
    50 EQINT	//  x '5
    IFNOTJMP:<{	//  x
"""
*/
