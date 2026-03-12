fun unsafe_tuple<X>(x: X): tuple
    asm "NOP";

@noinline
fun inc(x: int, y: int): (int, int) {
    return (x + y, y * 10);
}
@noinline
fun int.`~inc`(mutate self, y: int): int {
    val (newX, newY) = inc(self, y);
    self = newX;
    return newY;
}

@noinline
fun eq<X>(v: X): X { return v; }
@noinline
fun eq2(v: (int, int)) { return v; }
@noinline
fun mul2(mutate dest: int, v: int): int { dest = v*2; return dest; }
@noinline
fun (int, int).multens(mutate self, v: (int, int)): (int, int) { var (f, s) = self; var (m1, m2) = v; self = (f*m1, s*m2); return self; }

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

@noinline
fun foo1(x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int): (int, int, int, int, int, int, int) {
    return (x1, x2, x3, x4, x5, x6, x7);
}

@method_id(15)
fun test_call_1(x: int): (int, int, int, int, int, int, int) {
    return foo1(x, x.`~inc`(x / 20), x, x = x * 2, x, x += 1, x);
}

@noinline
fun foo2(x1: int, x2: int, x3456: (int, int, int, int), x7: int, ): (int, int, int, int, int, int, int) {
    var (x3: int, x4: int, x5: int, x6: int) = x3456;
    return (x1, x2, x3, x4, x5, x6, x7);
}

@method_id(16)
fun test_call_2(x: int): (int, int, int, int, int, int, int) {
    return foo2(x, x.`~inc`(x / 20), (x, x = x * 2, x, x += 1), x);
}

fun asm_func(x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int): (int, int, int, int, int, int, int)
    asm (x4 x5 x6 x7 x1 x2 x3->0 1 2 3 4 5 6) "NOP";

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
fun test_global(x: int) {
    xx = x;
    return (x, xx, xx.`~inc`(xx / 20), eq(xx += (x *= 0)), xx = xx * 2, xx, xx += 1, xx, x);
}

@method_id(20)
fun test_if_else(x: int): (int, int, int, int, int) {
    if (x > 10) {
        return (x.`~inc`(8), x + 1, x = 1, x <<= 3, x);
    } else {
        xx = 9;
        return (x, x.`~inc`(-4), x.`~inc`(-1), (x >= 1) as int, x = x + xx);
    }
}

@method_id(21)
fun test_assign_with_inner(x: int) {
    var cp = x;
    var result = (x, x += 10, [x, x += 20, eq(x -= 50), x], eq2((x, x *= eq(cp /= 2))));
    return result;
}

@method_id(22)
fun test_assign_with_mutate(x: int) {
    var cp = x;
    var (result, _) = ((x, mul2(mutate x, x + 5), x.`~inc`(mul2(mutate cp, x)), x), 0);
    return (result, cp);
}

@method_id(23)
fun test_assign_tensor(x: (int, int)) {
    var fs = (0, 0);
    return (x, x = (20, 30), fs = x.multens((1, 2)), fs.multens(x.multens((-1, -1))), x, fs);
}

global fs: (int, int);
@method_id(24)
fun test_assign_tensor_global(x: (int, int)) {
    fs = (0, 0);
    return (x, x = (20, 30), fs = x.multens((1, 2)), fs.multens(x.multens((-1, -1))), x, fs);
}

struct Point1D {
    c: int;
}

@method_id(25)
fun test_with_single_slot_struct(x: Point1D) {
    var result = (x, x.c += 10, [x, x.c += 20, eq(x.c -= 50), x.c] as array<unknown>, eq2((x.c, x.c *= eq(x.c / 2))));
    return result;
}

@method_id(26)
fun test_load_slice() {
    var s = stringHexToSlice("0000006f000000de");        // 111 and 222
    var tensor = (s, s.loadAny<int32>(), s);
    return (tensor.0.remainingBitsCount(), tensor.0.loadUint(32), tensor.1, tensor.2.remainingBitsCount());
}

@method_id(27)
fun test_skip_in_slice() {
    var s = stringHexToSlice("0000006f000000de");        // 111 and 222
    var tensor = (s, s.skipAny<int32>(), s, s.skipBits(31), s);
    return (tensor.0.remainingBitsCount(), tensor.1.remainingBitsCount(), tensor.2.remainingBitsCount(), tensor.4.remainingBitsCount());
}

@method_id(28)
fun test_store_builder() {
    var b = beginCell();
    var tensor = (b, b.storeAny<int32>(5), b);
    return (tensor.0.bitsCount(), tensor.1.bitsCount(), tensor.2.bitsCount());
}

@method_id(29)
fun test_create_array() {
    var x = 1;
    return [ x, x += 2, x, eq(mul2(mutate x, 5)), x];
}

@method_id(30)
fun test_create_list(): lisp_list<int> {
    var x = 1;
    return [ x, x += 2, x, eq(mul2(mutate x, 5)), x];
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
@testcase | 19 | 100 | 100 100 50 105 210 210 211 211 0
@testcase | 20 | 80  | 80 89 1 8 8
@testcase | 20 | 9   | 9 -40 -10 -1 13
@testcase | 21 | 100 | 100 110 [ 110 130 80 80 ] 80 4000
@testcase | 22 | 100 | 100 210 4200 630 420
@testcase | 23 | 1 1 | 1 1 20 30 20 60 -400 -3600 -20 -60 -400 -3600
@testcase | 24 | 1 1 | 1 1 20 30 20 60 -400 -3600 -20 -60 -400 -3600
@testcase | 25 | 100 | 100 110 [ 110 130 80 80 ] 80 3200
@testcase | 26 |     | 64 111 111 32
@testcase | 27 |     | 64 32 32 1
@testcase | 28 |     | 0 32 32
@testcase | 29 |     | [ 1 3 3 10 10 ]
@testcase | 30 |     | [ 1 [ 3 [ 3 [ 10 [ 10 (null) ] ] ] ] ]

@fif_codegen
"""
  int.~inc() PROC:<{      //  self y
    inc() CALLDICT    //  self newY
  }>
"""

@fif_codegen
"""
  test_assign_tensor_global() PROC:<{   // x.0 x.1
"""

@code_hash 111667756568862912577641874227144621753163521614440586561560498476829147651751
*/
