fun main(a: int, b: int, c: int, d: int, e: int, f: int): (int, int) {
  var D: int = a * d - b * c;
  var Dx: int = e * d - b * f;
  var Dy: int = a * f - e * c;
  return (Dx / D, Dy / D);
}

@method_id(101)
fun testDivMod(x: int, y: int) {
  return [divMod(x, y), modDiv(x, y), mulDivMod(x, y, 10)];
}

/**
    method_id   | in            | out
@testcase | 0   | 1 1 1 -1 10 6 | 8 2
@testcase | 0   | 817 -31 624 -241 132272 272276 | 132 -788
@testcase | 0   | -886 562 498 -212 -36452 -68958 | -505 -861
@testcase | 0   | 448 -433 -444 792 150012 -356232 | -218 -572
@testcase | 0   | -40 -821 433 -734 -721629 -741724 | -206 889
@testcase | 0   | -261 -98 -494 868 -166153 733738 | 263 995
@testcase | 101 | 112 3 | [ 37 1 1 37 33 6 ]
*/
