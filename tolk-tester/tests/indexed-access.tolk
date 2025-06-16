type Tup2Int = [int, MInt];
type Tensor2Int = (int, MInt);
type MInt = int;

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

global gTup: [int];
global gTens: (int, int);

@method_id(100)
fun testCodegenSimple() {
    var t1 = [1];
    t1.0 = 2;
    debug.printString("");
    var t2 = [[1]];
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
    var t = (1, (2, 3), [4, 5, [6, 7]], 8);
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
    var m = [[5, 5]];
    (m.0.0, m.0.1) = (10, 20);
    t104 = [[5, 5]];
    (t104.0.0, t104.0.1) = (10, 20);
    return (t104, m);
}

@method_id(105)
fun test105(x: int, y: int): (tuple, int, (int?, int), int, int) {
    var ab = (createEmptyTuple(), (x as int?, y), tuple.size);
    ab.0.push(1);
    tuplePush(mutate ab.0, 2);
    ab.1.0 = null;
    ab.1.1 += 10;
    var cb = ab.2;
    return (ab.0, ab.0.1, ab.1, cb(ab.0), ab.2(ab.0));
}

@method_id(106)
fun test106(x: int, y: int) {
    var ab = [createEmptyTuple(), [x as int?, y], tuple.size];
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
    return (ab.0 as int, getBuilderBitsCount(ab.1));
}

global t108: [int, [int, [int]]];

@method_id(108)
fun test108(last: int) {
    t108 = [1, [2, [last]]];
    t108.1.1.0.increment();
    var t = [1, [2, [last]]];
    t.1.1.0.increment();
    return (t108, t);
}

@method_id(109)
fun test109(x: (int, int)): (int, int, int, int, int, int, int) {
    return (x.1, x.1.plus(x.1 / 20), x.1, x.1 = x.1 * 2, x.1, x.1 += 1, x.1);
}

@method_id(110)
fun test110(f: int, s: int) {
    var x = [f, s];
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
    return (yy, yy.1, yy.1.plus(yy.1 / 20), eq(yy.1 += (yy.1 *= 0)), yy.1 = yy.1 * 2, yy.1, yy.1 += 1, yy.1, yy);
}

@pure
fun getConstTuple(): Tup2Int {
    return [1,2];
}

fun testCodegenNoPureIndexedAccess() {
    (getConstTuple().1, getConstTuple().0) = (3, 4);
    return 0;
}

@method_id(113)
fun test113() {
    var x = [[1, 2]];
    return (x, x.0, plus(mutate x.0.0, 10), x.0, x, x.0 = [10, 20], x);
}

@method_id(114)
fun test114(f: int, s: int) {
    var x = ((), (f, s), ());
    return (x, x.1, plus(mutate x.1.0, 10), x.1, x, x.1 = (10, 20), x);
}

@method_id(115)
fun test115() {
    var y = [[[[true]]]];
    return (y, ((((y).0).0).0).0 = !y.0.0.0.0, y.0);
}

@method_id(116)
fun test116() {
    var t = createEmptyTuple();
    t.push(1);
    try {
        return t.100500 as int;
    } catch(excNo) {
        return excNo;
    }
}

@method_id(117)
fun test117() {
    var t = createEmptyTuple();
    t.push(1);
    try {
        return (t.0 as tuple).0 as MInt;
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

fun getT() { return (1, 2); }

@method_id(120)
fun test120() {
    return (getT().0 = 3, getT().0 = sizeof(getT()) * 2, [getT().0 = 5, getT().0 = 6]);
}

@method_id(121)
fun test121(zero: int) {
    var t = createEmptyTuple();
    t.push(-100);
    t.set(0, zero);
    (t.0 as int).increment();
    (((t.0) as int) as int).increment();
    increment(mutate t.0 as int);
    return t;
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

fun main(){}


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
@testcase | 120 |       | 3 4 [ 5 6 ]
@testcase | 121 | 0     | [ 3 ]
@testcase | 122 | 1 2   | -1 -1 0 -1 -1 0
@testcase | 123 |       | [ [ 100 100 ] ]

@fif_codegen
"""
  testCodegenSimple PROC:<{
    1 PUSHINT	    //  '2=1
    SINGLE	        //  t1
    2 PUSHINT	    //  t1 '3=2
    0 SETINDEX	    //  t1
    x{} PUSHSLICE	//  t1 '6
    STRDUMP DROP
    1 PUSHINT	//  t1 '10=1
    SINGLE	    //  t1 '9
    SINGLE	    //  t1 t2
    2 PUSHINT	//  t1 t2 '11=2
    OVER	    //  t1 t2 '11=2 t2
    0 INDEX	    //  t1 t2 '11=2 '14
    SWAP	    //  t1 t2 '14 '11=2
    0 SETINDEX	//  t1 t2 '14
    0 SETINDEX	//  t1 t2
    x{} PUSHSLICE	//  t1 t2 '17
    STRDUMP DROP
    1 PUSHINT	//  t1 t2 '20=1
    SINGLE	    //  t1 t2 '18
    gTup SETGLOB
    2 PUSHINT	    //  t1 t2 '21=2
    gTup GETGLOB	//  t1 t2 '21=2 g_gTup
    SWAP	        //  t1 t2 g_gTup '21=2
    0 SETINDEX	    //  t1 t2 g_gTup
    gTup SETGLOB
    x{} PUSHSLICE	//  t1 t2 '25
    STRDUMP DROP
    1 PUSHINT	//  t1 t2 '28=1
    2 PUSHINT	//  t1 t2 '26=1 '27=2
    PAIR
    gTens SETGLOB
    4 PUSHINT	//  t1 t2 g_gTens.1=4
    gTens GETGLOB
    UNPAIR	    //  t1 t2 g_gTens.1=4 g_gTens.0 g_gTens.1
    DROP	    //  t1 t2 g_gTens.1=4 g_gTens.0
    SWAP	    //  t1 t2 g_gTens.0 g_gTens.1=4
    PAIR
    gTens SETGLOB
    x{} PUSHSLICE	//  t1 t2 '36
    STRDUMP DROP
    gTup GETGLOB	//  t1 t2 g_gTup
    gTens GETGLOB
    UNPAIR	    //  t1 t2 g_gTup g_gTens.0 g_gTens.1
  }>
"""

@fif_codegen
"""
  testCodegenNoPureIndexedAccess PROC:<{
    0 PUSHINT  // '8=0
  }>
"""

@fif_codegen
"""
  testCodegenIndexPostfix1 PROC:<{   //  x.0 x.1
    //  ab.1 ab.0
    SWAP	//  ab.0 ab.1
  }>
"""

@fif_codegen
"""
  testCodegenIndexPostfix2 PROC:<{   //  x.0 x.1.0 x.1.1 x.2
    s2 POP	//  y.0 y.2 y.1.1
    s1 s2 XCHG	//  y.2 y.0 y.1.1
  }>
"""
 */
