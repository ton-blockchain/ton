@deprecated
fun twice(f: int -> int, x: int) {
  return f (f (x));
}

fun sqr(x: int) {
  return x * x;
}

fun main(x: int): int {
  var f = sqr;
  return twice(f, x) * f(x);
}

@method_id(4)
fun pow6(x: int): int {
  return twice(sqr, x) * sqr(x);
}

/**
    method_id | in | out
@testcase |  0 | 3  | 729
@testcase |  0 | 10 | 1000000
@testcase |  4 | 3  | 729
@testcase |  4 | 10 | 1000000
*/
