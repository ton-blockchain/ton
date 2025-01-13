@method_id(1)
fun test(y: int): int {
  var x: int = 1;
  if (y > 0) {
    return 1;
  }
  return x > 0 ? -1 : 0;
}

@method_id(2)
fun f(y: int): int {
  if (y > 0) {
    return 1;
  }
  return 2;
}

fun main() { }

/**
    method_id | in | out
@testcase | 1  | 10  | 1
@testcase | 1  | -5  | -1
@testcase | 2  | 10  | 1
@testcase | 2  | -5  | 2
*/
