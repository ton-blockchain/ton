fun foo1(x: int): int {
  if (x == 1) {
    return 1;
  }
  return 2;
}

@inline
fun foo2(x: int): int {
  if (x == 1) {
    return 11;
  }
  return 22;
}

@inline_ref
fun foo3(x: int): int {
  if (x == 1) {
    return 111;
  }
  return 222;
}

@method_id(101)
fun test1(x: int): (int, int, int) {
  return (foo1(x)+1, foo2(x)+1, foo3(x)+1);
}

global g: int;

@inline
fun foo_repeat() {
  g = 1;
  repeat(5) {
    g *= 2;
  }
}

@inline
fun foo_until(): int {
  g = 1;
  var i: int = 0;
  do {
    g *= 2;
    i += 1;
  } while (i < 8);
  return i;
}

@inline
fun foo_while(): int {
  g = 1;
  var i: int = 0;
  while (i < 10) {
    g *= 2;
    i += 1;
  }
  return i;
}

@method_id(102)
fun test2() {
  foo_repeat();
  var x: int = g;
  foo_until();
  var y: int = g;
  foo_while();
  var z: int = g;
  return (x, y, z);
}

@inline
fun foo_big(x: int): int {
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  x = x * 10 + 1;
  return x;
}

@method_id(103)
fun test3(x: int): int {
  return foo_big(x) * 10 + 5;
}

fun main() {}

/**
      method_id | in | out
@testcase | 101 | 1  | 2 12 112
@testcase | 101 | 2  | 3 23 223
@testcase | 102 |    | 32 256 1024
@testcase | 103 | 9  | 9111111111111111111111111111111111111111111111111115
*/
