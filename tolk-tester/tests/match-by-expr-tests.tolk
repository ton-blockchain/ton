const C1 = 1;
const C2 = C1 + 1;

const zero_addr = address("EQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM9c");

enum Role {
    User,
    Admin,
}

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

@noinline
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
    var r1 = match (x is int && x is int is bool) { false => 0, else => 10 };
    match (x) { }
    match (x * 0) { 1 => {} else => { r1 += 20; } }
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

global g12: int;

@noinline
fun helper12(x: int) {
    // via codegen, check that `return` is added implicitly to every case, and `IFJMP` is produced
    match (x) {
        1 => g12 = 1,
        2 => g12 = 2,
        3 => g12 = 3,
        else => g12 = x,
    }
}

@method_id(112)
fun test12(x: int) {
    g12 = 0;
    helper12(x);
    return g12
}

fun invalidAsmRole(): Role asm "99 PUSHINT"
fun invalidAsmBool(): bool asm "99 PUSHINT"

@noinline
fun mapRole(r: Role): int {
    return match (r) {
        Role.User => 100,
        Role.Admin => 200,
    }
    // no "missing return", match is exhaustive
}

@noinline
fun mapBool(b: bool): int {
    return match (b) {
        true => 100,
        false => 200,
    }
    // no "missing return", match is exhaustive
}

@method_id(113)
fun test13() {
    var ex1 = 0;
    var ex2 = 0;

    // will throw (the compiler generates "else" implicitly for enums)
    try { ex1 = mapRole(invalidAsmRole()); }        
    catch (ex) { ex1 = ex }

    // will not throw (the compiler compiles "match" for bool as true => else =>)
    try { ex2 = mapBool(invalidAsmBool()); }
    catch (ex) { ex2 = ex }
    
    return (ex1, ex2);
}

@method_id(114)
fun test14(case: int) {
    return match (match (case) {
        1 => address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf"),
        2 => address("Ef89xh-uy860-mCcvS8zcAUs8bApmxLGygDLEKjUk5RL-311"),
        else => zero_addr,
    }) {
        address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") => 10,
        address("Ef89xh-uy860-mCcvS8zcAUs8bApmxLGygDLEKjUk5RL-311") => 20,
        zero_addr => 30,
        else => throw 123,
    }
}

@method_id(115)
fun test15(useNone: bool) {
    var addr = (address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") as any_address).toCell().load();
    if (useNone) {
        addr = createAddressNone();
    }
    match (addr) {
        address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") =>
            return match (addr) {
                address("Ef9nzj6RBc4mQ6p3ng7mGJ7tp7MbzERhe7obkM9A0wnCCEcf") => 1,
                else => 2
            }
    }
    return createAddressNone() == addr ? null : 100;
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
@testcase | 0   |          | 2
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
@testcase | 112 | 2        | 2
@testcase | 112 | 20       | 20
@testcase | 113 |          | 5 200
@testcase | 114 | 2        | 20
@testcase | 114 | 8        | 30
@testcase | 115 | 0        | 1
@testcase | 115 | -1       | (null)

@fif_codegen
"""
  test1() PROC:<{ //  x
    DUP         //  x x
    1 EQINT     //  x '3
    IF:<{       //  x
      DROP      //
      100 PUSHINT       //  '1=100
    }>ELSE<{    //  x
      DUP       //  x x
      2 EQINT   //  x '6
      IF:<{     //  x
        DROP    //
        200 PUSHINT     //  '1=200
      }>ELSE<{  //  x
        3 EQINT //  '11
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
  test2() PROC:<{
    300 PUSHINT
  }>
"""

@fif_codegen
"""
  test3() PROC:<{       //  init
    DUP                 // init init
    isGt10() CALLDICT   // init '2
    -1 EQINT	        // init '5
    IF:<{	            // init
      10 PUSHINT	    // init '3=10
    }>ELSE<{	        // init
      20 PUSHINT	    // init '3=20
    }>	                // init r1
"""

@fif_codegen
"""
  test8() PROC:<{   // x
    DROP            //
    30 PUSHINT      // r1
  }>
"""

@fif_codegen
"""
  helper12() PROC:<{
    DUP
    1 EQINT
    IFJMP:<{
      DROP
      1 PUSHINT
      $g12 SETGLOB
    }>
    DUP
    2 EQINT
    IFJMP:<{
      DROP
      2 PUSHINT
      $g12 SETGLOB
    }>
    DUP
    3 EQINT
    IFJMP:<{
      DROP
      3 PUSHINT
      $g12 SETGLOB
    }>
    $g12 SETGLOB
  }>
"""

@fif_codegen
"""
      1 EQINT                   //  '6
      IFNOTJMP:<{               // 
        5 THROW
      }>                        // 
      200 PUSHINT               //  '1=200
"""

 */
