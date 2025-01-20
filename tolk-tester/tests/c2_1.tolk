fun check_assoc(op: (int, int) -> int, a: int, b: int, c: int) {
  return op(op(a, b), c) == op(a, op(b, c));
}

fun main(x: int, y: int, z: int): bool {
  return check_assoc(`_+_`, x, y, z);
}

/**
    method_id | in         | out
@testcase | 0  | 2 3 9      | -1
@testcase | 0  | 11 22 44   | -1
@testcase | 0  | -1 -10 -20 | -1
*/
