fun foo(x: int): int {
  try {
    if (x == 7) {
      throw 44;
    }
    return x;
  } catch {
    return 2;
  }
}

@inline
fun foo_inline(x: int): int {
  try {
    assert(!(x == 7)) throw 44;
    return x;
  } catch {
    return 2;
  }
}

@inline_ref
fun foo_inlineref(x: int): int {
  try {
    if (x == 7) { throw (44, 2); }
    return x;
  } catch (_, arg) {
    return arg as int;
  }
}

@method_id(101)
fun test(x: int, y: int, z: int): int {
  y = foo(y);
  return x * 100 + y * 10 + z;
}

@method_id(102)
fun test_inline(x: int, y: int, z: int): int {
  y = foo_inline(y);
  return x * 100 + y * 10 + z;
}

@method_id(103)
fun test_inlineref(x: int, y: int, z: int): int {
  y = foo_inlineref(y);
  return x * 100 + y * 10 + z;
}

@inline
fun foo_inline_big(
        x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int, x8: int, x9: int, x10: int,
        x11: int, x12: int, x13: int, x14: int, x15: int, x16: int, x17: int, x18: int, x19: int, x20: int
): int {
  try {
    if (x1 == 7) {
      throw 44;
    }
    return x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10 + x11 + x12 + x13 + x14 + x15 + x16 + x17 + x18 + x19 + x20;
  } catch {
    return 1;
  }
}

@method_id(104)
fun test_inline_big(x: int, y: int, z: int): int {
  y = foo_inline_big(
          y, y + 1, y + 2, y + 3, y + 4, y + 5, y + 6, y + 7, y + 8, y + 9,
          y + 10, y + 11, y + 12, y + 13, y + 14, y + 15, y + 16, y + 17, y + 18, y + 19);
  return x * 1000000 + y * 1000 + z;
}

fun foo_big(
        x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int, x8: int, x9: int, x10: int,
        x11: int, x12: int, x13: int, x14: int, x15: int, x16: int, x17: int, x18: int, x19: int, x20: int
): int {
  try {
    if (x1 == 7) {
      throw (44, 1);
    }
    return x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10 + x11 + x12 + x13 + x14 + x15 + x16 + x17 + x18 + x19 + x20;
  } catch (code, arg) {
    return arg as int;
  }
}

@method_id(105)
fun test_big(x: int, y: int, z: int): int {
  y = foo_big(
          y, y + 1, y + 2, y + 3, y + 4, y + 5, y + 6, y + 7, y + 8, y + 9,
          y + 10, y + 11, y + 12, y + 13, y + 14, y + 15, y + 16, y + 17, y + 18, y + 19);
  return x * 1000000 + y * 1000 + z;
}

@method_id(106)
fun test_catch_into_same(x: int): int {
  var code = x;
  try {
    assert(x <= 10, 44);
  } catch(code) {
    return code;
  }
  return code;
}


@method_id(107)
fun test_catch_into_same_2(x: int): int {
  var code = x;
  try {
    if (x > 10) {
      throw 44;
    }
  } catch(code) {
  }
  return code;
}

global after046: int;

// this bug existed in FunC and is fixed in v0.4.6
fun bug_046_internal(op: int) {
  if (op == 1) {
    return;
  } else if (op == 2) {
    return;
  } else {
    throw 1;
  }
}

fun bug_046_called() {
  after046 = 0;
  try {
    bug_046_internal(1337);
    after046 = 1;  // shouldn't be called
  } catch(n) {
    return;
  }
  return;
}

@method_id(108)
fun bug_046_entrypoint() {
  bug_046_called();
  return after046;
}

global g_reg: int;

@method_id(109)
fun test109(): (int, int) {
  var l_reg = 10;
  g_reg = 10;
  try {
    // note, that regardless of assignment, an exception RESTORES them to previous (to 10)
    // it's very unexpected, but is considered to be a TVM feature, not a bug
    g_reg = 999;
    l_reg = 999;
    bug_046_internal(999);  // throws
  } catch {
  }
  // returns (10,10) because of an exception, see a comment above
  return (g_reg, l_reg);
}

@noinline
fun alwaysThrow123(): never {
  throw 123;
}

fun alwaysThrowX(x: int): never {
  if (x > 10) { throw (x, beginCell()); }
  else { throw (x, null); }
}

@noinline
fun anotherNever(throw123: bool) {    // auto-infer `never`
  if (throw123) { alwaysThrow123(); }
  alwaysThrowX(456);
}

fun testCodegen1(x: int) {
  if (x > 10) {
    throw 123;
    anotherNever(true); // unreachable, will be dropped
  }
  else if (x < 10) {
    throw x;
    return -123;        // unreachable, will be dropped
  }
  return 0;
}

fun testCodegen2(x: int) {
  if (x > 10) {
    alwaysThrow123();
    anotherNever(true); // unreachable, will be dropped
  }
  else if (x < 10) {
    anotherNever(false);
    return -123;        // unreachable, will be dropped
  }
  return 0;
}

@method_id(110)
fun test110(b: bool) {
  try {
    if (b == true) { testCodegen1(100); }
    testCodegen1(5);
    return -1;
  } catch (ex) {
    return ex;
  }
}

@method_id(111)
fun test111(b: bool) {
  try {
    if (b == true) { testCodegen2(100); }
    testCodegen2(5);
    return -1;
  } catch (ex) {
    return ex;
  }
}

fun mySetCodeAsm(newCode: slice): void
    asm "SETCODE";

fun mySetCode(newCode: slice) {
  mySetCodeAsm(newCode);
}

fun testCodegen3(numberId: int, paramVal: cell) {
  if (numberId == -1000) {
    var cs = paramVal.beginParse();
    mySetCode(cs);
    throw 0;
  }
  paramVal.beginParse();
}

@method_id(112)
fun testCatchUsesVars(x1: int) {
  var x2 = x1 * 2;
  try { alwaysThrow123(); }
  catch (excno) {
    var result = x1 + x2 + excno;
    return result;
  }
  return -1;
}

@method_id(113)
fun testBigExcno() {
  try {
    throw 2048;
    return 10;
  } catch (excno) {
    return excno;
  }
}

get fun some_foo() {
  throw 10;
}

get fun some_bar() {
  try {
    some_foo();
    return 12;
  } catch (e) {
    return e;
  }
}

@method_id(114)
fun test14() {
  return some_bar();
}

@noinline
fun demoWithNonConstExcno(x: int) {
  // when x == 0, `100 / 0` is not evaluated
  assert (x == 0) throw 100 / x;
  return x;
}

@method_id(115)
fun testNonConstExcno() {
  var d0 = demoWithNonConstExcno(0);        // should not throw
  try { return (d0, demoWithNonConstExcno(5)) }
  catch (ex) { return (d0, ex) }
}

@method_id(116)
fun testNonConstExcno2(x: int) {
  try {
    // when x == 0, we get "integer overflow" (excno 4)
    // when x == 1, assert is ok
    // when x > 0, then preloadUint fails with "cell underflow" (excno 9)
    assert(1 / x > 0, "00".hexToSlice().preloadUint(16));
    return 777;
  } catch (excno) {
    return excno;
  }
}

@method_id(117)
fun testNonConstExcno3(x: int) {
  try { assert ((x += 10) > 50) throw x; }
  catch (ex) { return -ex }
  return x;
}

@method_id(118)
fun testThrowWithArgWhenArgEmitsException(x: int) {
  try {
    // when x == 0, we get "integer overflow" (excno 4)
    // otherwise, preloadUint fails with "cell underflow" (excno 9)
    throw (1 / x, stringHexToSlice("00").preloadUint(16));
    return 777;
  } catch (excno) {
    return excno;
  }
}

@inline
fun catchInInlineF(flag: bool): int {
  __expect_inline(true);
  var out = 0;
  try {
    if (flag) {
      throw 77;
    }
    out = 10;
  } catch (excno) {
    out = excno;
  }
  return out;
}

@method_id(119)
fun testMultipleCallsInline(f1: bool, f2: bool) {
  return (
    catchInInlineF(f1),
    catchInInlineF(f2),
  );
}



fun main() {
  // mark used to codegen them
  testCodegen3;
}

/**
    method_id   | in    | out
@testcase | 101 | 1 2 3 | 123
@testcase | 101 | 3 8 9 | 389
@testcase | 101 | 3 7 9 | 329
@testcase | 102 | 1 2 3 | 123
@testcase | 102 | 3 8 9 | 389
@testcase | 102 | 3 7 9 | 329
@testcase | 103 | 1 2 3 | 123
@testcase | 103 | 3 8 9 | 389
@testcase | 103 | 3 7 9 | 329
@testcase | 104 | 4 8 9 | 4350009
@testcase | 104 | 4 7 9 | 4001009
@testcase | 105 | 4 8 9 | 4350009
@testcase | 105 | 4 7 9 | 4001009
@testcase | 106 | 5     | 5
@testcase | 106 | 20    | 44
@testcase | 107 | 5     | 5
@testcase | 107 | 20    | 20
@testcase | 108 |       | 0
@testcase | 109 |       | 10 10
@testcase | 110 | -1    | 123
@testcase | 110 | 0     | 5
@testcase | 111 | -1    | 123
@testcase | 111 | 0     | 456
@testcase | 112 | 5     | 138
@testcase | 113 |       | 2048
@testcase | 114 |       | 10
@testcase | 115 |       | 0 20
@testcase | 116 | 0     | 4
@testcase | 116 | 1     | 777
@testcase | 116 | 2     | 9
@testcase | 117 | 45    | 55
@testcase | 117 | 5     | -15
@testcase | 118 | 0     | 4
@testcase | 118 | 5     | 9
@testcase | 119 | 0 -1  | 10 77

@code_hash 30678353210988159315732784812592428126213301484608964371215840468221297111381

@fif_codegen
"""
  testCodegen1() PROC:<{  // x
    DUP	//  x x
    10 GTINT	//  x '2
    IFJMP:<{	//  x
      123 THROW
    }>	//  x
    DUP	//  x x
    10 LESSINT	//  x '6
    IFJMP:<{	//  x
      THROWANY
    }>	//  x
    DROP	//
    0 PUSHINT	//  '8=0
  }>
"""

@fif_codegen
"""
  testCodegen2() PROC:<{  // x
    DUP	//  x x
    10 GTINT	//  x '2
    IFJMP:<{	//  x
      DROP	//
      alwaysThrow123() CALLDICT
    }>	//  x
    10 LESSINT	//  '5
    IFJMP:<{	//
      FALSE	//  '6
      anotherNever() CALLDICT
    }>	//
    0 PUSHINT	//  '8=0
  }>
"""

@fif_codegen
"""
  testCodegen3() PROC:<{  //  numberId paramVal
    SWAP
    -1000 PUSHINT	//  paramVal numberId '2=-1000
    EQUAL	    //  paramVal '3
    IFJMP:<{	//  paramVal
      CTOS	    //  cs
      SETCODE
      0 THROW
    }>	    //  paramVal
    DROP	//
  }>
"""

@fif_codegen
"""
CONT:<{
  11 PUSHPOW2
  THROWANY
}>
"""

@fif_codegen
"""
foo_inline() PROCINLINE:<{
  CONT:<{
    c2 SAVE
    SAMEALTSAVE
"""
*/
