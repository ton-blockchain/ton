global g: int;

@inline
fun foo_repeat() {
  g = 1;
  repeat(5) {
    g *= 2;
  }
}

@inline
fun foo_until(): int {
  g = 1;
  var i: int = 0;
  do {
    g *= 2;
    i += 1;
  } while (i < 8);
  return i;
}

@inline
fun foo_while(): int {
  g = 1;
  var i: int = 0;
  while (i < 10) {
    g *= 2;
    i += 1;
  }
  return i;
}

fun main() {
  foo_repeat();
  var x: int = g;
  foo_until();
  var y: int = g;
  foo_while();
  var z: int = g;
  return (x, y, z);
}

/**
   method_id | in | out
@testcase | 0 |    | 32 256 1024

@code_hash 102749806552989901976653997041637095139193406161777448419603700344770997608788
*/
