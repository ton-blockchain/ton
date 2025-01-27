global op: (int, int) -> int;

fun check_assoc(a: int, b: int, c: int): bool {
  return op(op(a, b), c) == op(a, op(b, c));
}

fun unnamed_args(_: int, _: slice, _: int) {
  return true;
}

fun main(x: int, y: int, z: int): bool {
  op = `_+_`;
  if (0) { return null; }
  return check_assoc(x, y, z);
}

@method_id(101)
fun test101(x: int, z: int) {
  return unnamed_args(x, "asdf", z);
}

/**
    method_id | in         | out
@testcase | 0   | 2 3 9      | -1
@testcase | 0   | 11 22 44   | -1
@testcase | 0   | -1 -10 -20 | -1
@testcase | 101 | 1 10       | -1
*/
