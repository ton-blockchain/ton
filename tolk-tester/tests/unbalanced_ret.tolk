fun main(x: int): (int, int) {
  var y: int = 5;
  if (x < 0) {
    x *= 2;
    y += 1;
    if (x == -10) {
      return (111, 0);
    }
  }
  return (x + 1, y);
}
/**
   method_id | in | out
@testcase | 0 | 10 | 11 5
@testcase | 0 | -5 | 111 0
@testcase | 0 | -4 | -7 6
*/
