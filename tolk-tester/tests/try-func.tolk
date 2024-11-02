fun unsafeGetInt<X>(any: X): int
    asm "NOP";

@method_id(11)
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
@method_id(12)
fun foo_inline(x: int): int {
  try {
    assert(!(x == 7)) throw 44;
    return x;
  } catch {
    return 2;
  }
}

@inline_ref
@method_id(13)
fun foo_inlineref(x: int): int {
  try {
    if (x == 7) { throw (44, 2); }
    return x;
  } catch (_, arg) {
    return unsafeGetInt(arg);
  }
}

@method_id(1)
fun test(x: int, y: int, z: int): int {
  y = foo(y);
  return x * 100 + y * 10 + z;
}

@method_id(2)
fun test_inline(x: int, y: int, z: int): int {
  y = foo_inline(y);
  return x * 100 + y * 10 + z;
}

@method_id(3)
fun test_inlineref(x: int, y: int, z: int): int {
  y = foo_inlineref(y);
  return x * 100 + y * 10 + z;
}

@inline
@method_id(14)
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

@method_id(4)
fun test_inline_big(x: int, y: int, z: int): int {
  y = foo_inline_big(
          y, y + 1, y + 2, y + 3, y + 4, y + 5, y + 6, y + 7, y + 8, y + 9,
          y + 10, y + 11, y + 12, y + 13, y + 14, y + 15, y + 16, y + 17, y + 18, y + 19);
  return x * 1000000 + y * 1000 + z;
}

@method_id(15)
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
    return unsafeGetInt(arg);
  }
}

@method_id(5)
fun test_big(x: int, y: int, z: int): int {
  y = foo_big(
          y, y + 1, y + 2, y + 3, y + 4, y + 5, y + 6, y + 7, y + 8, y + 9,
          y + 10, y + 11, y + 12, y + 13, y + 14, y + 15, y + 16, y + 17, y + 18, y + 19);
  return x * 1000000 + y * 1000 + z;
}

@method_id(16)
fun test_catch_into_same(x: int): int {
  var code = x;
  try {
    assert(x <= 10, 44);
  } catch(code) {
    return code;
  }
  return code;
}


@method_id(17)
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

fun main() {
}

/**
    method_id | in    | out
@testcase | 1  | 1 2 3 | 123
@testcase | 1  | 3 8 9 | 389
@testcase | 1  | 3 7 9 | 329
@testcase | 2  | 1 2 3 | 123
@testcase | 2  | 3 8 9 | 389
@testcase | 2  | 3 7 9 | 329
@testcase | 3  | 1 2 3 | 123
@testcase | 3  | 3 8 9 | 389
@testcase | 3  | 3 7 9 | 329
@testcase | 4  | 4 8 9 | 4350009
@testcase | 4  | 4 7 9 | 4001009
@testcase | 5  | 4 8 9 | 4350009
@testcase | 5  | 4 7 9 | 4001009
@testcase | 16 | 5     | 5
@testcase | 16 | 20    | 44
@testcase | 17 | 5     | 5
@testcase | 17 | 20    | 20

@code_hash 73240939343624734070640372352271282883450660826541545137654364443860257436623
*/
