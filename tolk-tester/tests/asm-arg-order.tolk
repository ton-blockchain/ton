@pure
fun empty_tuple2(): tuple
asm "NIL";
@pure
fun tuple.tpush2<X>(mutate self, x: X): void
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
fun int.asm_func_3(self, y: int, z: int): tuple
asm (y z self -> 0) "3 TUPLE";
@pure
fun asm_func_4(a: int, b: (int, (int, int)), c: int): tuple
asm (b a c -> 0) "5 TUPLE";

@pure
fun tuple.asm_func_modify(mutate self, b: int, c: int): void
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

@pure
fun asmAPlus1TimesB(a: int, b: int): int
    asm(b a) "1 ADDCONST MUL";

@pure
fun int.plus1TimesB(self, b: int): int
    asm(b self) "1 ADDCONST MUL";

@pure
fun get2Pure() { return 2; }
@pure
fun get10Pure() { return 10; }

fun get2Impure() { return 2; }
fun get10Impure() { return 10; }

global g2: int;
global g10: int;

fun setG2(v: int) { g2 = v; return v; }

@method_id(27)
fun test27() {
    return asmAPlus1TimesB(2, 10);
}

@method_id(28)
fun test28() {
    return asmAPlus1TimesB(get2Pure(), get10Pure());
}

@method_id(29)
fun test29() {
    return asmAPlus1TimesB(get2Impure(), get10Impure());
}

@method_id(30)
fun test30() {
    g2 = 2;
    g10 = 10;
    return asmAPlus1TimesB(g2, g10);
}

@method_id(31)
fun test31() {
    g2 = 2;
    g10 = 10;
    return asmAPlus1TimesB(g2 += 2, g10 += g2);
}

@method_id(32)
fun test32() {
    return 2.plus1TimesB(10);
}

@method_id(33)
fun test33(x: int) {
    return ((x += 10).plus1TimesB(2), (x += 20).plus1TimesB(x), ((x /= (g2=2)).plus1TimesB(x*g2)), setG2(7).plus1TimesB(g2));
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
@testcase | 27 |    | 30
@testcase | 28 |    | 30
@testcase | 29 |    | 30
@testcase | 30 |    | 30
@testcase | 31 |    | 70
@testcase | 32 |    | 30
@testcase | 33 | 0  | 22 930 480 56

@fif_codegen
"""
  test27 PROC:<{
    10 PUSHINT
    2 PUSHINT
    1 ADDCONST MUL
  }>
"""

@fif_codegen
"""
  test28 PROC:<{
    get10Pure CALLDICT
    get2Pure CALLDICT
    1 ADDCONST MUL
  }>
"""

@fif_codegen
"""
  test29 PROC:<{
    get2Impure CALLDICT
    get10Impure CALLDICT
    SWAP
    1 ADDCONST MUL
  }>
"""

@fif_codegen
"""
  test30 PROC:<{
    ...
    g10 GETGLOB
    g2 GETGLOB
    1 ADDCONST MUL
  }>
"""

@fif_codegen
"""
  test32 PROC:<{
    10 PUSHINT
    2 PUSHINT
    1 ADDCONST MUL
  }>
"""

@code_hash 78671986831403867804966279036762472603849672357801214378328975900111280733054
*/
