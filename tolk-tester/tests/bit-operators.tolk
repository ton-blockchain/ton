fun lshift(): bool {
    return (1 << 0) == 1;
}

fun rshift(): bool {
    return (1 >> 0) == 1;
}

fun lshift_var(i: int): bool {
    return (1 << i) == 1;
}

fun rshift_var(i: int): bool {
    return (1 >> i) == 1;
}

fun main(x: int): bool {
    if (x == 0) {
        return lshift();
    } else if (x == 1) {
        return rshift();
    } else if (x == 2) {
        return lshift_var(0);
    } else if (x == 3) {
        return rshift_var(0);
    } else if (x == 4) {
        return lshift_var(1);
    } else {
        return rshift_var(1);
    }
}

@method_id(11)
fun is_claimed(index: int): bool {
    var claim_bit_index: int = index % 256;
    var mask: int = 1 << claim_bit_index;
    return (255 & mask) == mask;
}

@method_id(12)
fun bit_not(i: int, b: bool): (int, bool, bool, bool, int, bool) {
    var i2 = ~i;
    var b2 = !b;
    var (i3: int, b3: bool) = (i2, b2);
    return (i3, b3, !i, !b, ~~~i, !!!b);
}

@method_id(13)
fun boolWithBitwiseConst() {
    var found = true;
    return (found & false, found | true, found ^ true, found & found);
}

global g14: int;
fun getBool() { return (g14 += 1) > 2; }

@method_id(14)
fun boolWithBitwise(b: bool) {
    g14 = 0;
    return (b & getBool(), !b & getBool(), b | getBool(), !b | getBool(), b ^ getBool(), !b & getBool(), g14);
}

@method_id(15)
fun boolWithBitwiseSet(b1: bool, b2: bool) {
    b1 &= b2;
    b2 |= true;
    b1 |= b1 == false;
    b2 ^= (b1 ^= b2);
    return (b1, b2);
}

@method_id(16)
fun testDoUntilCodegen(i: bool, n: int) {
    var cnt = 0;
    do { cnt += 1; } while (i);
    do { cnt += 1; } while (!!i);
    do { cnt += 1; } while (n);
    return (cnt, !i, !n);
}

@method_id(17)
fun testConstNegateCodegen() {
    return (!0, !1, !true, !false, !!true, !!false);
}

@method_id(18)
fun testBoolNegateOptimized(x: bool) {
    return (x, !x, !!x, !!!x, !!!!true);
}

fun eqX(x: bool) { return x; }

@method_id(19)
fun testBoolCompareOptimized(x: bool) {
    return (x == true, x != true, eqX(x) == false, eqX(x) != false, !!(x == !false));
}



/**
    method_id | in  | out
@testcase | 0  | 0   | -1
@testcase | 0  | 1   | -1
@testcase | 0  | 2   | -1
@testcase | 0  | 3   | -1
@testcase | 0  | 4   |  0
@testcase | 0  | 5   |  0
@testcase | 11 | 0   | -1
@testcase | 11 | 1   | -1
@testcase | 11 | 256 | -1
@testcase | 11 | 8   |  0
@testcase | 12 | 0 0   | -1 -1 -1 -1 -1 -1
@testcase | 12 | -1 -1 | 0 0 0 0 0 0
@testcase | 12 | 7 0   | -8 -1 0 -1 -8 -1
@testcase | 14 | -1    | 0 0 -1 -1 0 0 6
@testcase | 14 | 0     | 0 0 -1 -1 -1 -1 6
@testcase | 15 | -1 -1 | 0 -1
@testcase | 15 | -1 0  | 0 -1
@testcase | 16 | 0 0   | 3 -1 -1
@testcase | 17 |       | -1 0 0 -1 -1 0
@testcase | 18 | 0     | 0 -1 0 -1 -1
@testcase | 18 | -1    | -1 0 -1 0 -1
@testcase | 19 | 0     | 0 -1 -1 0 0
@testcase | 19 | -1    | -1 0 0 -1 -1

@fif_codegen
"""
  boolWithBitwiseConst PROC:<{
    0 PUSHINT	                //  '3
    -1 PUSHINT	                //  '3 '5
    0 PUSHINT	                //  '3 '5 '7
    -1 PUSHINT	                //  '3 '5 '7 '8
  }>
"""

@fif_codegen
"""
  testDoUntilCodegen PROC:<{   //  i n
    0 PUSHINT	//  i n cnt=0
    UNTIL:<{
      INC	//  i n cnt
      s2 PUSH	//  i n cnt i
      NOT	//  i n cnt '6
    }>	//  i n cnt
    UNTIL:<{
      INC	//  i n cnt
      s2 PUSH	//  i n cnt i
      NOT	//  i n cnt '9
    }>	//  i n cnt
    UNTIL:<{
      INC	//  i n cnt
      OVER	//  i n cnt n
      0 EQINT	//  i n cnt '12
    }>	//  i n cnt
    s0 s2 XCHG	//  cnt n i
    NOT	//  cnt n '13
    SWAP	//  cnt '13 n
    0 EQINT	//  cnt '13 '14
  }>
"""

@fif_codegen
"""
  testConstNegateCodegen PROC:<{
    TRUE	//  '0
    FALSE	//  '0 '1
    FALSE	//  '0 '1 '2
    TRUE	//  '0 '1 '2 '3
    TRUE	//  '0 '1 '2 '3 '4
    FALSE	//  '0 '1 '2 '3 '4 '5
  }>
"""

@fif_codegen
"""
  testBoolNegateOptimized PROC:<{   //  x
    DUP	        //  x x
    NOT	        //  x '1
    OVER	    //  x '1 x
    NOT	        //  x '1 '2
    s2 s(-1) PUXC
    TRUE	    //  x '1 x '2 '3
  }>
"""

@fif_codegen
"""
  testBoolCompareOptimized PROC:<{  //  x
    DUP	    //  x x
    NOT	    //  x '1
    OVER	//  x '1 x
    eqX CALLDICT	//  x '1 '2
    NOT	    //  x '1 '3
    s2 PUSH	//  x '1 '3 x
    eqX CALLDICT	//  x '1 '3 '4
    s3 PUSH	//  x '1 '3 '4 x
  }>
"""
*/
