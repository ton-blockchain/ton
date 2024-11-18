fun elseif(cond: int) {
  if (cond > 0) {
    throw(cond);
  }
}

@inline
@method_id(101)
fun foo(x: int): int {
  if (x==1) {
    return 111;
  } else {
    x *= 2;
  }
  return x + 1;
}

fun main(x: int): (int, int) {
  return (foo(x), 222);
}

@method_id(102)
fun test2(x: int) {
  try {
    if (x < 0) { return -1; }
    elseif (x);
  } catch(excNo) {
    return excNo * 1000;
  }
  return 0;
}

/**
   method_id    | in | out
@testcase | 0   | 1  | 111 222
@testcase | 0   | 3  | 7 222
@testcase | 101 | 1  | 111
@testcase | 101 | 3  | 7
@testcase | 102 | -5 | -1
@testcase | 102 | 5  | 5000
*/
