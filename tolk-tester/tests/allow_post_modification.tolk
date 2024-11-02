fun unsafe_tuple<X>(x: X): tuple
    asm "NOP";

fun inc(x: int, y: int): (int, int) {
  return (x + y, y * 10);
}
fun `~inc`(mutate self: int, y: int): int {
  val (newX, newY) = inc(self, y);
  self = newX;
  return newY;
}

@method_id(11)
fun test_return(x: int): (int, int, int, int, int, int, int) {
  return (x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x);
}

@method_id(12)
fun test_assign(x: int): (int, int, int, int, int, int, int) {
  var (x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int) = (x, x.`~inc`(x / 20), x, x=x*2, x, x+=1, x);
  return (x1, x2, x3, x4, x5, x6, x7);
}

@method_id(13)
fun test_tuple(x: int): tuple {
  var t: tuple = unsafe_tuple([x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x]);
  return t;
}

@method_id(14)
fun test_tuple_assign(x: int): (int, int, int, int, int, int, int) {
  var [x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int] = [x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x];
  return (x1, x2, x3, x4, x5, x6, x7);
}

fun foo1(x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int): (int, int, int, int, int, int, int) {
  return (x1, x2, x3, x4, x5, x6, x7);
}

@method_id(15)
fun test_call_1(x: int): (int, int, int, int, int, int, int) {
  return foo1(x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x);
}

fun foo2(x1: int, x2: int, x3456: (int, int, int, int), x7: int): (int, int, int, int, int, int, int) {
  var (x3: int, x4: int, x5: int, x6: int) = x3456;
  return (x1, x2, x3, x4, x5, x6, x7);
}

@method_id(16)
fun test_call_2(x: int): (int, int, int, int, int, int, int) {
  return foo2(x, x.`~inc`(x / 20), (x, x = x * 2, x, x += 1), x);
}

fun asm_func(x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int): (int, int, int, int, int, int, int)
asm
        (x4 x5 x6 x7 x1 x2 x3->0 1 2 3 4 5 6) "NOP";

@method_id(17)
fun test_call_asm_old(x: int): (int, int, int, int, int, int, int) {
  return asm_func(x, x += 1, x, x, x.`~inc`(x / 20), x, x = x * 2);
}

@method_id(18)
fun test_call_asm_new(x: int): (int, int, int, int, int, int, int) {
  return asm_func(x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x);
}

global xx: int;
@method_id(19)
fun test_global(x: int): (int, int, int, int, int, int, int) {
  xx = x;
  return (xx, xx.`~inc`(xx / 20), xx, xx = xx * 2, xx, xx += 1, xx);
}

@method_id(20)
fun test_if_else(x: int): (int, int, int, int, int) {
  if (x > 10) {
    return (x.`~inc`(8), x + 1, x = 1, x <<= 3, x);
  } else {
    xx = 9;
    return (x, x.`~inc`(-4), x.`~inc`(-1), x >= 1, x = x + xx);
  }
}

fun main() {
}

/**
    method_id | in  | out
@testcase | 11 | 100 | 100 50 105 210 210 211 211
@testcase | 12 | 100 | 100 50 105 210 210 211 211
@testcase | 13 | 100 | [ 100 50 105 210 210 211 211 ]
@testcase | 14 | 100 | 100 50 105 210 210 211 211
@testcase | 15 | 100 | 100 50 105 210 210 211 211
@testcase | 16 | 100 | 100 50 105 210 210 211 211
@testcase | 17 | 100 | 101 50 106 212 100 101 101
@testcase | 18 | 100 | 210 210 211 211 100 50 105
@testcase | 19 | 100 | 100 50 105 210 210 211 211
@testcase | 20 | 80  | 80 89 1 8 8
@testcase | 20 | 9   | 9 -40 -10 -1 13

@fif_codegen
"""
  ~inc PROC:<{
    //  self y
    inc CALLDICT // self newY
  }>
"""
@code_hash 97139400653362069936987769894397430077752335662822462908581556703209313861576
*/
