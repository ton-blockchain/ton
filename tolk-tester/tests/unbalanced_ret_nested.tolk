fun foo(y: int): int {
  if (y < 0) {
    y *= 2;
    if (y == -10) {
      return 111;
    }
  }
  return y + 1;
}
fun bar(x: int, y: int): (int, int) {
  if (x < 0) {
    y = foo(y);
    x *= 2;
    if (x == -10) {
      return (111, y);
    }
  }
  return (x + 1, y);
}
fun main(x: int, y: int): (int, int) {
  (x, y) = bar(x, y);
  return (x, y * 10);
}
/**
   method_id | in    | out
@testcase | 0 |  3  3 | 4 30
@testcase | 0 |  3 -5 | 4 -50
@testcase | 0 |  3 -4 | 4 -40
@testcase | 0 | -5  3 | 111 40
@testcase | 0 | -5 -5 | 111 1110
@testcase | 0 | -5 -4 | 111 -70
@testcase | 0 | -4  3 | -7 40
@testcase | 0 | -4 -5 | -7 1110
@testcase | 0 | -4 -4 | -7 -70

@code_hash 68625253347714662162648433047986779710161195298061582217368558479961252943991
*/
