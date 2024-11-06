@inline
fun foo(x: int): int {
  if (x < 0) {
    x *= 2;
    if (x == -10) {
      return 111;
    }
  }
  return x + 1;
}
fun main(x: int): int {
  return foo(x) * 10;
}
/**
   method_id | in | out
@testcase | 0 | 10 | 110
@testcase | 0 | -5 | 1110
@testcase | 0 | -4 | -70
*/
