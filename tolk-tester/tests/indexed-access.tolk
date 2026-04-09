import "@stdlib/reflection"

type Tup2Int = [int, MInt];
type Tensor2Int = (int, MInt);
type MInt = int;

struct Point {
    x: int
    y: int
}

@pure
fun tuplePush<T>(mutate t: tuple, value: T): void
    asm "TPUSH";
@pure
fun getBuilderBitsCount(b: builder): int
    asm "BBITS";

fun MInt.increment(mutate self) {
    self += 1;
}

fun increment(mutate v: MInt) {
    v += 1;
}

fun increment2(mutate a: int, mutate b: int) {
    a += 1;
    b += 1;
}

fun assign1020(mutate a: int, mutate b: MInt) {
    a = 10;
    b = 20;
}

fun int.plus(mutate self, y: int): int {
    val newVals = (self + y, y * 10);
    self = newVals.0;
    return newVals.1;
}

fun plus(mutate x: int, y: int): int {
    val newVals = (x + y, y * 10);
    x = newVals.0;
    return newVals.1;
}

fun eq<X>(v: X): X { return v; }

global gTup: [int]
global gTens: (int, int)

@method_id(100)
fun testCodegenSimple() {
    var t1 = [1] as [int];
    t1.0 = 2;
    debug.printString("");
    var t2 = [[1]] as [[int]];
    t2.0.0 = 2;
    debug.printString("");
    gTup = [1];
    gTup.0 = 2;
    debug.printString("");
    gTens = (1,2);
    gTens.1 = 4;
    debug.printString("");
    return (t1, t2, gTup, gTens);
}

@method_id(101)
fun test101() {
    var t = (1, (2, 3), [4, 5, [6, 7]] as [int, int, [int, int]], 8);
    (t.0, t.1.0, t.2.0) = (2, 3, 5);
    t.3.increment();
    t.2.1 += (t.1.1 += 1) - t.1.1 + 1;
    increment2(mutate t.2.2.0, mutate t.2.2.1);
    return t;
}

global t102: (int, (int, MInt), [int, int, Tup2Int], MInt);

@method_id(102)
fun test102() {
    t102 = (1, (2, 3), [4, 5, [6, 7]], 8);
    (t102.0, t102.1.0, t102.2.0) = (2, 3, 5);
    t102.3.increment();
    t102.2.1 += (t102.1.1 += 1) - t102.1.1 + 1;
    increment2(mutate t102.2.2.0, mutate t102.2.2.1);
    return t102;
}

global t103: (int, int);

@method_id(103)
fun test103() {
    t103 = (5, 5);
    assign1020(mutate t103.0, mutate t103.1);
    var t = (5, 5);
    assign1020(mutate t.0, mutate t.1);
    return (t103, t);
}

global t104: [[int, int]];

@method_id(104)
fun test104() {
    var m = [[5, 5]] as [[coins, coins]];
    (m.0.0, m.0.1) = (10, 20);
    t104 = [[5, 5]];
    (t104.0.0, t104.0.1) = (10, 20);
    return (t104, m);
}

@method_id(105)
fun test105(x: int, y: int): (tuple, unknown, (int?, int), int, int) {
    var ab = (createEmptyTuple(), (x as int?, y), tuple.size);
    ab.0.push(1);
    tuplePush(mutate ab.0, 2);
    ab.1.0 = null;
    ab.1.1 += 10;
    var cb = ab.2;
    return (ab.0, ab.0.get(1), ab.1, cb(ab.0), ab.2(ab.0));
}

@method_id(106)
fun test106(x: int, y: int) {
    var ab = [createEmptyTuple(), [x as int?, y], tuple.size] as [array<unknown>, [int?, int], array<unknown> -> int];
    ab.0.push(1);
    tuplePush(mutate ab.0, 2);
    ab.1.0 = null;
    ab.1.1 += 10;
    var cb = ab.2;
    return (ab.0, ab.1, cb(ab.0), ab.2(ab.0));
}

@method_id(107)
fun test107() {
    var ab = createEmptyTuple();
    ab.push(1);
    ab.push(beginCell().storeInt(1, 32));
    return (ab.get(0), getBuilderBitsCount(ab.get(1) as builder));
}

global t108: [int, [int, [int]]];

@method_id(108)
fun test108(last: int) {
    t108 = [1, [2, [last]]];
    t108.1.1.0.increment();
    var t: [int, [int, [int]]] = [1, [2, [last]]];
    t.1.1.0.increment();
    return (t108, t);
}

@method_id(109)
fun test109(x: (int, int)): (int, int, int, int, int, int, int) {
    return (x.1, x.1.plus(x.1 / 20), x.1, x.1 = x.1 * 2, x.1, x.1 += 1, x.1);
}

@method_id(110)
fun test110(f: int, s: int) {
    var x = [f, s] as [int, int];
    return (x, x.1, x.1.plus(x.1 / 20), x.1, x.1 = x.1 * 2, x.1, x.1 += 1, x.1, x);
}

global xx: Tensor2Int;

@method_id(111)
fun test111(x: (int, int)) {
    xx = x;
    return (x, xx.1, xx.1.plus(xx.1 / 20), eq(xx.1 += (x.1 *= 0)), xx.1 = xx.1 * 2, xx.1, xx.1 += 1, xx.1, x);
}

global yy: [int, int];

@method_id(112)
fun test112(f: int, s: int) {
    yy = [f, s];
    return (yy, yy.1, yy.1.plus(yy.1 / 20), eq(yy.1 += (yy.0 *= 0)), yy.1 = yy.1 * 2, yy.1, yy.1 += 1, yy.1, yy);
}

@pure
@noinline
fun getConstTuple(): Tup2Int {
    return [1,2];
}

@method_id(113)
fun test113() {
    var x = [[1, 2]] as [[int,uint256]];
    return (x, x.0, plus(mutate x.0.0, 10), x.0, x, x.0 = [10, 20], x);
}

@method_id(114)
fun test114(f: int, s: int) {
    var x = ((), (f, s), ());
    return (x, x.1, plus(mutate x.1.0, 10), x.1, x, x.1 = (10, 20), x);
}

@method_id(115)
fun test115() {
    var y: [[[[bool]]]] = [[[[true]]]];
    return (y, ((((y).0).0).0).0 = !y.0.0.0.0, y.0);
}

@method_id(116)
fun test116() {
    var t = createEmptyTuple();
    t.push(1);
    try {
        return t.get(100500) as int;
    } catch(excNo) {
        return excNo;
    }
}

@method_id(117)
fun test117() {
    var t = createEmptyTuple();
    t.push(1);
    try {
        return (t.get(0) as array<int8>).get(0) as MInt;
    } catch(excNo) {
        return excNo;
    }
}

@method_id(118)
fun testCodegenIndexPostfix1(x: (int, int)) {
    var ab = (x.1, x.0);
    return ab;
}

@method_id(119)
fun testCodegenIndexPostfix2(x: (int, (int, int), int)) {
    var y = x;
    return (y.2, y.0, y.1.1);
}

global t121: (int, int);

@method_id(121)
fun test121() {
    t121 = (0, 0);
    t121.0 = t121.1 = 1;
    var tup = [[0, 0]] as [[int,int]];
    tup.0.0 = tup.0.1 = 2;
    return (t121, tup);
}

fun (T1, T2).isFirstComponentGt0(self): bool {
    return self.0 > 0;
}

fun isFirstComponentGt0<T1,T2>(t: (T1, T2)): bool {
    return t.0 > 0;
}

@method_id(122)
fun test122(x: (int, int)) {
    return (
        isFirstComponentGt0(x), isFirstComponentGt0((2, beginCell())), isFirstComponentGt0<int,slice?>((0, null)),
        x.isFirstComponentGt0(), (2, beginCell()).isFirstComponentGt0(), (0, null as slice?).isFirstComponentGt0()
    );
}

@method_id(123)
fun test123() {
    var t = [[10, 20]] as [[int,int]]?;
    ((t!).0).0 = ((t!).0).1 = 100;
    return t;
}


@method_id(124)
fun test124(zero: int) {
    var t = createEmptyTuple();
    t.push(-100);
    t.set(0, zero);
    return t;
}

@method_id(125)
fun test125() {
    var t: [Point] = [{x: 1, y: 2}];
    t.0.x = 123;
    return t;
}

global g126: [Point];

@method_id(126)
fun test126() {
    var t: [Point] = g126 = [{x: 1, y: 2}];
    (t.0.x, g126.0.x, t.0.y, g126.0.y) = (3,3,4,4);
    increment2(mutate t.0.x, mutate t.0.y);
    increment2(mutate g126.0.x, mutate g126.0.y);
    return (t.0, g126, g126.0);
}

fun main(){
}


/**
@testcase | 101 |     | 2 3 4 [ 5 6 [ 7 8 ] ] 9
@testcase | 102 |     | 2 3 4 [ 5 6 [ 7 8 ] ] 9
@testcase | 103 |     | 10 20 10 20
@testcase | 104 |     | [ [ 10 20 ] ] [ [ 10 20 ] ]
@testcase | 105 | 5 6 | [ 1 2 ] 2 (null) 16 2 2
@testcase | 106 | 5 6 | [ 1 2 ] [ (null) 16 ] 2 2
@testcase | 107 |     | 1 32
@testcase | 108 | 3   | [ 1 [ 2 [ 4 ] ] ] [ 1 [ 2 [ 4 ] ] ]
@testcase | 109 | 0 100 | 100 50 105 210 210 211 211
@testcase | 110 | 0 100 | [ 0 100 ] 100 50 105 210 210 211 211 [ 0 211 ]
@testcase | 111 | 0 100 | 0 100 100 50 105 210 210 211 211 0 0
@testcase | 112 | 0 100 | [ 0 100 ] 100 50 105 210 210 211 211 [ 0 211 ]
@testcase | 113 |       | [ [ 1 2 ] ] [ 1 2 ] 100 [ 11 2 ] [ [ 11 2 ] ] [ 10 20 ] [ [ 10 20 ] ]
@testcase | 114 | 1 2   | 1 2 1 2 100 11 2 11 2 10 20 10 20
@testcase | 115 |       | [ [ [ [ -1 ] ] ] ] 0 [ [ [ 0 ] ] ]
@testcase | 116 |       | 5
@testcase | 117 |       | 7
@testcase | 118 | 1 2   | 2 1
@testcase | 119 | 1 2 3 4 | 4 1 3
@testcase | 121 |       | 1 1 [ [ 2 2 ] ]
@testcase | 122 | 1 2   | -1 -1 0 -1 -1 0
@testcase | 123 |       | [ [ 100 100 ] ]
@testcase | 124 | 0     | [ 0 ]
@testcase | 125 |       | [ [ 123 2 ] ]
@testcase | 126 |       | 4 5 [ [ 4 5 ] ] 4 5

@fif_codegen
"""
  testCodegenSimple() PROC:<{
    1 PUSHINT                   //  '1=1
    1 TUPLE                     //  t1
    2 PUSHINT                   //  t1 '7=2
    0 SETINDEX                  //  t1
    x{} PUSHSLICE
    STRDUMP DROP                //  t1
    1 PUSHINT                   //  t1 '11=1
    1 TUPLE                     //  t1 '10
    1 TUPLE                     //  t1 t2
    2 PUSHINT                   //  t1 t2 '17=2
    OVER                        //  t1 t2 '17=2 t2
    0 INDEX                     //  t1 t2 '21=2 '19
    SWAP                        //  t1 t2 '19 '21=2
    0 SETINDEX                  //  t1 t2 '23
    0 SETINDEX                  //  t1 t2
    x{} PUSHSLICE
    STRDUMP DROP                //  t1 t2
    1 PUSHINT                   //  t1 t2 '26=1
    1 TUPLE                     //  t1 t2 g_gTup
    $gTup SETGLOB               //  t1 t2
    2 PUSHINT                   //  t1 t2 '31=2
    $gTup GETGLOB               //  t1 t2 '34=2 g_gTup
    SWAP                        //  t1 t2 g_gTup '34=2
    0 SETINDEX                  //  t1 t2 g_gTup
    $gTup SETGLOB               //  t1 t2
    x{} PUSHSLICE
    STRDUMP DROP                //  t1 t2
    1 PUSHINT                   //  t1 t2 '36=1
    2 PUSHINT                   //  t1 t2 g_gTens.0=1 g_gTens.1=2
    2 TUPLE
    $gTens SETGLOB              //  t1 t2
    4 PUSHINT                   //  t1 t2 g_gTens.1=4
    $gTens GETGLOB
    2 UNTUPLE                   //  t1 t2 g_gTens.1=4 g_gTens.0 g_gTens.1
    DROP                        //  t1 t2 g_gTens.1=4 g_gTens.0
    SWAP                        //  t1 t2 g_gTens.0 g_gTens.1=4
    2 TUPLE
    $gTens SETGLOB              //  t1 t2
    x{} PUSHSLICE
    STRDUMP DROP                //  t1 t2
    $gTup GETGLOB               //  t1 t2 g_gTup
    $gTens GETGLOB
    2 UNTUPLE                   //  t1 t2 g_gTup g_gTens.0 g_gTens.1
  }>
"""

@fif_codegen
"""
  testCodegenIndexPostfix1() PROC:<{   //  x.0 x.1
    SWAP	//  ab.0 ab.1
  }>
"""

@fif_codegen
"""
  testCodegenIndexPostfix2() PROC:<{   //  x.0 x.1.0 x.1.1 x.2
    s2 POP	//  y.0 y.2 y.1.1
    s1 s2 XCHG	//  y.2 y.0 y.1.1
  }>
"""

@fif_codegen
"""
  test125() PROC:<{
    1 PUSHINT
    2 PUSHINT
    2 TUPLE
    1 TUPLE
    123 PUSHINT
    OVER
    0 INDEX 2 UNTUPLE
    NIP
    2 TUPLE 0 SETINDEX
  }>
"""
 */
