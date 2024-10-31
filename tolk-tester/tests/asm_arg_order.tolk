@pure
fun empty_tuple2(): tuple
asm "NIL";
@pure
fun tpush2<X>(mutate self: tuple, x: X): void
asm "TPUSH";

@pure
fun asm_func_1(x: int, y: int, z: int): tuple
asm "3 TUPLE";
@pure
fun asm_func_2(x: int, y: int, z: int): tuple
asm (z y x -> 0) "3 TUPLE";
@pure
fun asm_func_3(x: int, y: int, z: int): tuple
asm (y z x -> 0) "3 TUPLE";
@pure
fun asm_func_4(a: int, b: (int, (int, int)), c: int): tuple
asm (b a c -> 0) "5 TUPLE";

@pure
fun asm_func_modify(mutate self: tuple, b: int, c: int): void
asm (c b self) "SWAP TPUSH SWAP TPUSH";

global t: tuple;

fun foo(x: int): int {
  t.tpush2(x);
  return x * 10;
}

@method_id(11)
fun test_old_1(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_1(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(12)
fun test_old_2(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_2(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(13)
fun test_old_3(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_3(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(14)
fun test_old_4(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = empty_tuple2();
  // This actually computes left-to-right even without compute-asm-ltr
  t2 = asm_func_4(foo(11), (foo(22), (foo(33), foo(44))), foo(55));
  return (t, t2);
}

@method_id(15)
fun test_old_modify(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = empty_tuple2();
  t2.asm_func_modify(foo(22), foo(33));
  return (t, t2);
}

@method_id(16)
fun test_old_dot(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = foo(11).asm_func_3(foo(22), foo(33));
  return (t, t2);
}

@method_id(21)
fun test_new_1(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_1(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(22)
fun test_new_2(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_2(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(23)
fun test_new_3(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_3(foo(11), foo(22), foo(33));
  return (t, t2);
}

@method_id(24)
fun test_new_4(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = asm_func_4(foo(11), (foo(22), (foo(33), foo(44))), foo(55));
  return (t, t2);
}

@method_id(25)
fun test_new_modify(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = empty_tuple2();
  t2.asm_func_modify(foo(22), foo(33));
  return (t, t2);
}

@method_id(26)
fun test_new_dot(): (tuple, tuple) {
  t = empty_tuple2();
  var t2: tuple = foo(11).asm_func_3(foo(22), foo(33));
  return (t, t2);
}

fun main() {
}

/**
    method_id | in | out
@testcase | 11 |    | [ 11 22 33 ] [ 110 220 330 ]
@testcase | 12 |    | [ 11 22 33 ] [ 330 220 110 ]
@testcase | 13 |    | [ 11 22 33 ] [ 220 330 110 ]
@testcase | 14 |    | [ 11 22 33 44 55 ] [ 220 330 440 110 550 ]
@testcase | 15 |    | [ 22 33 ] [ 220 330 ]
@testcase | 16 |    | [ 11 22 33 ] [ 220 330 110 ]
@testcase | 21 |    | [ 11 22 33 ] [ 110 220 330 ]
@testcase | 22 |    | [ 11 22 33 ] [ 330 220 110 ]
@testcase | 23 |    | [ 11 22 33 ] [ 220 330 110 ]
@testcase | 24 |    | [ 11 22 33 44 55 ] [ 220 330 440 110 550 ]
@testcase | 25 |    | [ 22 33 ] [ 220 330 ]
@testcase | 26 |    | [ 11 22 33 ] [ 220 330 110 ]

@code_hash 93068291567112337250118419287631047120002003622184251973082208096953112184588
*/
