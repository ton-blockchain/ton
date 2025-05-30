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

fun alwaysThrow123(): never {
  throw 123;
}

fun alwaysThrowX(x: int): never {
  if (x > 10) { throw (x, beginCell()); }
  else { throw (x, null); }
}

fun anotherNever(throw123: bool): never {
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

fun mySetCode(newCode: slice): void
    asm "SETCODE";

fun testCodegen3(numberId: int, paramVal: cell) {
  if (numberId == -1000) {
    var cs = paramVal.beginParse();
    mySetCode(cs);
    throw 0;
  }
  paramVal.beginParse();
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

fun main() {
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
@testcase | 113 |       | 2048

@code_hash 26411074751358281917876479601714698321674833208179417883888954015536273820035

@fif_codegen
"""
  testCodegen1 PROC:<{  // x
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
  testCodegen2 PROC:<{  // x
    DUP	//  x x
    10 GTINT	//  x '2
    IFJMP:<{	//  x
      DROP	//
      alwaysThrow123 CALLDICT
    }>	//  x
    10 LESSINT	//  '5
    IFJMP:<{	//
      FALSE	//  '6
      anotherNever CALLDICT
    }>	//
    0 PUSHINT	//  '8=0
  }>
"""

@fif_codegen
"""
  testCodegen3 PROC:<{  //  numberId paramVal
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
<{
  11 PUSHPOW2
  THROWANY
}>CONT
"""
*/
