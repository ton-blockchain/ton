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

@fif_codegen
"""
  test3 PROC:<{
    //  x
    DUP	//  x x
    20 NEQINT	//  x _2
    IFNOTJMP:<{	//  x
      DROP	//
      20 PUSHINT	//  _3=20
    }>	//  x
    DUP	//  x x
    50 EQINT	//  x _5
    IFNOTJMP:<{	//  x
"""
*/
