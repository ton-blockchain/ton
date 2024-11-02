fun main() { }
@method_id(1)
fun steps(x: int): int {
  var n = 0;
  while (x > 1) {
    n += 1;
    if (x & 1) {
      x = 3 * x + 1;
    } else {
      x >>= 1;
    }
  }
  return n;
}

/**
    method_id | in | out
@testcase | 1 |   1 | 0
@testcase | 1 |   2 | 1
@testcase | 1 |   5 | 5
@testcase | 1 |  19 | 20
@testcase | 1 |  27 | 111
@testcase | 1 | 100 | 25
*/
