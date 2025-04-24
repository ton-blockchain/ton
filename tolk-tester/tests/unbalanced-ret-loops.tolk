fun main() { }

@method_id(1)
fun foo_repeat(x: int): int {
  repeat(10) {
    x += 10;
    if (x >= 100) {
      return x;
    }
  }
  return -1;
}

@method_id(2)
fun foo_while(x: int): int {
  var i: int = 0;
  while (i < 10) {
    x += 10;
    if (x >= 100) {
      return x;
    }
    i += 1;
  }
  return -1;
}

@method_id(3)
fun foo_until(x: int): int {
  var i: int = 0;
  do {
    x += 10;
    if (x >= 100) {
      return x;
    }
    i += 1;
  } while (i < 10);
  return -1;
}

@method_id(4)
fun test4(x: int): (int, bool) {
  var s = 0;
  var reached = false;
  do {
    x = x - 1;
    s = s + 1;
    if (x < 10) {
      reached = true;
    }
  } while (!reached);
  return (s, reached);
}

/**
   method_id | in | out
@testcase | 1 | 40 | 100
@testcase | 1 | 33 | 103
@testcase | 1 | -5 | -1
@testcase | 2 | 40 | 100
@testcase | 2 | 33 | 103
@testcase | 2 | -5 | -1
@testcase | 3 | 40 | 100
@testcase | 3 | 33 | 103
@testcase | 3 | -5 | -1
@testcase | 4 | 18 | 9 -1

@code_hash 12359153928622198176298534554187062238616102949658930329300859312625793323482
*/
