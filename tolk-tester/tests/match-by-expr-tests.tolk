const C1 = 1;
const C2 = C1 + 1;

@method_id(101)
fun test1(x: int) {
    return match (x) {
        1 => 100,
        C2 => 200,
        2 + 1 => 300,
        else => 400
    }
}

@method_id(102)
fun test2() {
    var x = 3;
    return match (x) {
        1 => 100,
        C2 => 200,
        2 + 1 => 300,
        else => 400
    }
}

fun isGt10(x: int) { return x > 10; }

@method_id(103)
fun test3(init: int) {
    var r1 = match (isGt10(init)) { true => 10, false => 20 };
    var r2 = match (isGt10(init)) { !!true => 10, !!false => 20, else => 8 };
    var r3 = match (isGt10(init)) { 10 > 3 => 10, 10 < 3 => 20, 100 > 1 => 30, else => 8 };
    return (r1, r2, r3);
}

@method_id(104)
fun test4(x: int?) {
    return match(match(x) { null => 0, int => x * 2 }) { 0 => 0, 30 => 30, else => throw 123 }
}

@method_id(105)
fun test5(x: int) {
    try {
        return match (val r = x * 2) {
            10/C1 => 10,
            C2*10 => throw 500,
            else => 50
        }
    } catch (excCode) {
        return excCode
    }
}

@method_id(106)
fun test6(x: coins) {
    var result1 = 0;
    match (x) {
        ton("0.05") => { result1 = 5; }
        ton("0.1") => { result1 = 10; }
        else => { result1 = 20; }
    }
    var result2 = 0;
    match (x) {
        ton("0.05") => { result2 = 5 }
        ton("0.1") => { result2 = 10 }
    }
    return (result1, result2);
}

@method_id(107)
fun test7(x: int8) {
    match (x) { 10 => {} }
    return match (x) {
        10 => 10,
        20 as int8 => 20,
        30 as int16 => 30,
        40 as uint255 => 40,
        else => 50,
    };
}

@method_id(108)
fun test8(x: int) {
    var r1 = match (x) { else => 10 };
    match (x) { }
    match (x) { else => { r1 += 20; } }
    return r1;
}

@method_id(109)
fun test9(x: int | slice) {
    if (x is slice) {
        match (x.loadInt(32)) { -1 => {}, 0 => {} }
    }
    return match (x) {
        int => match (x) { 10 => 5, else => -1 },
        slice => match (x.loadInt(32)) { -1 => 0, 0 => 0, else => throw 20 }
    }
}

@method_id(110)
fun test10(x: int) {
    var result1 = match (x) {
        10 => throw 10,
        20 => return 20,
        30 => { x += 10; return x; }
        else => x
    };
    return result1 * 2;
}

@method_id(111)
fun test11(x: bool) {
    return match (x) {
        true => false,
        else => !x,
    }
}


type asdf = int;

fun main() {
    match (10) {
        asdf => 1,      // it's match by type
    };

    var asdf = 5;
    return match (10) {
        asdf => 2,      // also match by type, regardless of local var
    };
}

/**
@testcase | 101 | 1        | 100
@testcase | 101 | 2        | 200
@testcase | 101 | 3        | 300
@testcase | 101 | 4        | 400
@testcase | 102 |          | 300
@testcase | 103 | 5        | 20 20 20
@testcase | 103 | 15       | 10 10 10
@testcase | 104 | 15       | 30
@testcase | 104 | null     | 0
@testcase | 105 | 10       | 500
@testcase | 105 | 11       | 50
@testcase | 106 | 50000000 | 5 5
@testcase | 106 | 300      | 20 0
@testcase | 107 | 30       | 30
@testcase | 107 | 40       | 40
@testcase | 107 | 41       | 50
@testcase | 108 | 5        | 30
@testcase | 109 | 1 1      | -1
@testcase | 110 | 20       | 20
@testcase | 110 | 30       | 40
@testcase | 110 | 50       | 100
@testcase | 111 | 0        | -1
@testcase | 111 | -1       | 0

@fif_codegen
"""
  test1 PROC:<{ //  x
    DUP         //  x x
    1 EQINT     //  x '3
    IF:<{       //  x
      DROP      //
      100 PUSHINT       //  '1=100
    }>ELSE<{    //  x
      DUP       //  x x
      2 EQINT   //  x '8
      IF:<{     //  x
        DROP    //
        200 PUSHINT     //  '1=200
      }>ELSE<{  //  x
        3 EQINT //  '13
        IF:<{   //
          300 PUSHINT   //  '1=300
        }>ELSE<{        //
          400 PUSHINT   //  '1=400
        }>
      }>
    }>
  }>
"""

@fif_codegen
"""
  test2 PROC:<{
    300 PUSHINT
  }>
"""

@fif_codegen
"""
  test3 PROC:<{         //  init
    DUP                 // init init
    isGt10 CALLDICT     // init '2
    -1 EQINT	        // init '5
    IF:<{	            // init
      10 PUSHINT	    // init '3=10
    }>ELSE<{	        // init
      20 PUSHINT	    // init '3=20
    }>	                // init r1
"""

@fif_codegen
"""
  test8 PROC:<{     // x
    DROP            //
    30 PUSHINT      // r1
  }>
"""

 */
