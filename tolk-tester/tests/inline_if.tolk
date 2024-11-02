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
fun main(x: int): (int, int, int) {
  return (foo1(x)+1, foo2(x)+1, foo3(x)+1);
}
/**
   method_id | in | out
@testcase | 0 | 1  | 2 12 112
@testcase | 0 | 2  | 3 23 223
*/
