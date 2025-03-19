fun incrementInPlace(mutate x: int, byValue: int): void {
    x = x + byValue;
}

fun int.incrementInPlace(mutate self, byValue: int): void {
    self = self + byValue;
}

fun incrementTwoInPlace(mutate x: int, mutate y: int, byValue: int): int {
    x.incrementInPlace(byValue);
    y += byValue;
    return x + y;
}

fun int.incrementTwoInPlace(mutate self, mutate y: int, byValue: int): int {
    self.incrementInPlace(byValue);
    y += byValue;
    return self + y;
}

@method_id(101)
fun testIncrement1() {
    var x = 50;
    var y = 30;
    incrementInPlace(mutate x, 10);
    incrementInPlace(mutate x, 10);
    incrementInPlace(mutate y, 10);
    y.incrementInPlace(10);
    incrementInPlace(mutate y, 10);
    return (x, y);
}

@method_id(102)
fun testIncrement2() {
    var x = 50;
    var y = 30;
    val sum1 = incrementTwoInPlace(mutate x, mutate y, 10);
    val sum2 = x.incrementTwoInPlace(mutate y, 10);
    return (x, y, sum1, sum2);
}


fun load_next(mutate cs: slice): int {
    return cs.loadInt(32);
}

fun slice.myLoadInt(mutate self, len: int): int
    asm(-> 1 0) "LDIX";
fun builder.myStoreInt(mutate self, x: int, len: int): self
    asm(x self len) "STIX";

@inline_ref
fun unpack_utils_info(mutate utils_info_sl: slice): (int, int) {
    return (
        utils_info_sl.myLoadInt(32),
        utils_info_sl.myLoadInt(32)
    );
}

@method_id(103)
fun testSlices1() {
    var b: builder = beginCell().storeInt(1, 32).myStoreInt(2, 32);
    b.myStoreInt(3, 32);
    var c: cell = b.myStoreInt(4, 32).storeInt(5, 32).endCell();
    var cs = c.beginParse();
    var first = cs.preloadInt(32);
    unpack_utils_info(mutate cs);
    return (first, cs.myLoadInt(32), cs.loadInt(32));
}

fun slice.load_decimal_symbol(mutate self): int {
    // load decimal from bits using utf-8 table
    var n: int = self.loadUint(8);
    n = n - 48;
    assert(n >= 0) throw 400;
    assert(n <= 9) throw 400;
    return n;
}

@method_id(104)
fun testSlices2() {
    var cs = "123";
    return (cs.load_decimal_symbol(), cs.load_decimal_symbol(), cs.load_decimal_symbol());
}

global v1: int;
global v2: int;
global v3: int;

@method_id(105)
fun testGlobals() {
    v1 = 0;
    v2 = 0;
    v3 = 100;
    v3 += incrementTwoInPlace(mutate v1, mutate v2, 5);
    return (v1, v2, v3);
}

fun withNameShadowing(mutate x: int, pivot: int, extra: int) {
    x += pivot;
    if (pivot < 100) {
        var x = 100 + extra;
        if (pivot < 50) {
            var x = 50 + extra;
            return x + extra;
        } else {
            x += extra;
            return x + extra;
        }
    } else {
        x += extra;
        return -100 + extra;
    }
}

@method_id(106)
fun testNameShadowing() {
    var x = 0;
    var sum = 0;
    sum += withNameShadowing(mutate x, 100, 10);
    sum += withNameShadowing(mutate x, 50, 10);
    sum += withNameShadowing(mutate x, 0, 10);
    return (x, sum);
}

type Pair2 = (int, int);
fun Pair2.updateTwoItems(mutate self, byValue: int) {
    val (first, second) = self;
    self = (first + byValue, second + byValue);
}
fun updateTwoItems(mutate pair: Pair2, byValue: int) {
    val (first, second) = pair;
    pair = (first + byValue, second + byValue);
}

global t107_1: int;
global t107_2: int;

@method_id(107)
fun testMutableTensor() {
    var t = (40, 50);
    t.updateTwoItems(10);
    updateTwoItems(mutate t, 10);
    t107_1 = 1;
    t107_2 = 2;
    (t107_1, t107_2).updateTwoItems(10);
    updateTwoItems(mutate (t107_1, t107_2), 10);
    return (t, t107_1, t107_2);
}

@pure
fun builder.myStoreUint(mutate self, x: int, len: int): self
    asm(x self len) "STIX";

@pure
fun myStoreUint(mutate b: builder, x: int, len: int): void
    asm(x b len) "STUX";
@pure
fun storeUint(mutate b: builder, x: int, len: int): void
    asm(x b len) "STUX";

@pure
fun builder.myStoreU32(mutate self, x: int): self {
    return self.storeUint(x, 32);
}
@pure
fun myStoreU32(mutate b: builder, x: int) {
    b.storeUint(x, 32);
}

fun getSumOfNumbersInCell(c: cell): int {
    var sum = 0;
    var s = c.beginParse();
    var n_numbers = s.remainingBitsCount() / 32;
    repeat (n_numbers) {
        sum += s.loadUint(32);
    }
    return sum;
}

@method_id(110)
fun testStoreChaining() {
    var b = ((beginCell()).storeUint(1, 32)).storeUint(2, 32).storeUint(3, 32);
    b.storeUint(4, 32);
    b.myStoreUint(5, 32).storeUint(6, 32);
    storeUint(mutate b, 7, 32);
    b = b.storeUint(8, 32);
    b = b.storeUint(9, 32).storeUint(10, 32);

    return b.bitsCount();
}

@method_id(111)
fun testStoreChainingCustom() {
    var b = beginCell().myStoreUint(1, 32).myStoreUint(2, 32).myStoreUint(3, 32);
    b.myStoreUint(4, 32);
    b.myStoreUint(5, 32).myStoreUint(6, 32);
    myStoreUint(mutate b, 7, 32);
    b = b.myStoreUint(8, 32);
    b = b.myStoreUint(9, 32).myStoreUint(10, 32);
    val sum1 = getSumOfNumbersInCell(b.endCell());

    b = beginCell().myStoreU32(1).storeUint(2, 32).myStoreU32(3);
    b.myStoreU32(4);
    b.myStoreU32(5).myStoreU32(6);
    myStoreU32(mutate b, 7);
    b = b.myStoreU32(8);
    b = b.storeUint(9, 32).myStoreU32(10);
    val sum2 = getSumOfNumbersInCell(b.endCell());

    return (sum1, sum2);
}

fun builder.myStoreU32_and_mutate_x(mutate self, mutate x: int): void {
    return myStoreUint(mutate self, x += 10, 32);
}

@method_id(112)
fun testStoreAndMutateBoth() {
    var x = 3;
    var b: builder = beginCell().myStoreUint(1, 32);
    b.myStoreU32_and_mutate_x(mutate x);
    b.myStoreU32(3).myStoreU32_and_mutate_x(mutate x);
    b.myStoreU32_and_mutate_x(mutate x);

    var cs: slice = b.endCell().beginParse();
    var (n1,n2,n3,n4,n5) = (cs.loadUint(32),((cs)).loadUint(32),cs.loadUint(32),cs.loadUint(32),cs.loadUint(32));
    assert(n5 == x) throw 100;

    return [n1,n2,n3,n4,n5];
}

global ccc: builder;

@method_id(113)
fun testStoreChainingForGlobal() {
    ccc = beginCell().storeUint(1, 32).myStoreUint(2, 32).myStoreU32(3);
    ccc.storeUint(4, 32);
    ccc.storeUint(5, 32).myStoreU32(6);
    storeUint(mutate ccc, 7, 32);
    ccc = ccc.myStoreU32(8);
    ccc = ccc.storeUint(9, 32).myStoreUint(10, 32);

    return ccc.bitsCount();
}

fun alwaysThrows(): int { throw 123; return 123; }
fun loadIntFromCell(c: cell, len: int) { return c.beginParse().loadUint(len); }

@method_id(114)
fun testLoadIntForTemporaryObject() {
    val c0 = beginCell().storeUint(0, 32).endCell();
    val c4 = beginCell().storeUint(4, 32).endCell();
    return (
        beginCell().storeUint(1, 32).endCell().beginParse().loadUint(32),
        beginCell().storeUint(2, 32).endCell().beginParse().loadUint(32),
        c0.beginParse().loadUint(32) ? alwaysThrows() : loadIntFromCell(c4, 32)
    );
}

@pure
fun builder.myStoreUint_pure(mutate self): void
    asm "STIX";

fun builder.myStoreUint_impure(mutate self): void
    asm "STIX";

fun testStoreUintPureUnusedResult() {
    var b = beginCell();
    b.myStoreUint_pure();
    var s = b.endCell().beginParse();
    val ii = s.loadUint(32);
    return 0;
}

fun testStoreUintImpureUnusedResult() {
    var b = beginCell();
    b.myStoreUint_impure();
    var s = b.endCell().beginParse();
    val ii = s.loadUint(32);
    return 0;
}

global counter: int;

fun builder.writeNext2(mutate self): self {
    return self.storeUint(counter += 1, 32).storeUint(counter += 1, 32);
}

fun builder.resetCounter(mutate self): self {
    counter = 0;
    return self;
}

@method_id(115)
fun testExplicitReturn() {
    counter = 0;
    return (
        getSumOfNumbersInCell(beginCell().writeNext2().writeNext2().resetCounter().writeNext2().endCell()),
        counter
    );
}


fun main(){}

/**
@testcase | 101 |    | 70 60
@testcase | 102 |    | 70 50 100 120
@testcase | 103 |    | 1 3 4
@testcase | 104 |    | 1 2 3
@testcase | 105 |    | 5 5 110
@testcase | 106 |    | 160 110
@testcase | 107 |    | 60 70 21 22
@testcase | 110 |    | 320
@testcase | 111 |    | 55 55
@testcase | 112 |    | [ 1 13 3 23 33 ]
@testcase | 113 |    | 320
@testcase | 114 |    | 1 2 4
@testcase | 115 |    | 13 2

@fif_codegen
"""
  int.incrementInPlace PROC:<{  //  self byValue
    ADD //  self
  }>
"""

@fif_codegen
"""
  testIncrement2 PROC:<{
    ...
    incrementTwoInPlace CALLDICT        //  x y sum1
    -ROT
    10 PUSHINT  //  sum1 x y '11=10
    int.incrementTwoInPlace CALLDICT        //  sum1 x y sum2
    s1 s3 s0 XCHG3       //  x y sum1 sum2
  }>
"""

@fif_codegen
"""
 load_next PROC:<{  //  cs
    32 LDI      //  '4 cs
    SWAP        //  cs '4
  }>
"""

@fif_codegen
"""
  testStoreUintPureUnusedResult PROC:<{
    0 PUSHINT	//  '11=0
  }>
"""

@fif_codegen
"""
  testStoreUintImpureUnusedResult PROC:<{
    NEWC	//  b
    STIX	//  '2
    DROP	//
    0 PUSHINT	//  '11=0
  }>
"""

 */
