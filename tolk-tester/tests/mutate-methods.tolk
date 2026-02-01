fun increment(mutate x: int): int {
    x += 1;
    return x;
}

@noinline
fun incrementInPlace(mutate x: int, byValue: int): void {
    x = x + byValue;
}

@noinline
fun int.incrementInPlace(mutate self, byValue: int): void {
    self = self + byValue;
}

@noinline
fun incrementTwoInPlace(mutate x: int, mutate y: int, byValue: int): int {
    x.incrementInPlace(byValue);
    y += byValue;
    return x + y;
}

@noinline
fun int.incrementTwoInPlace(mutate self, mutate y: int, byValue: int): int {
    self.incrementInPlace(byValue);
    y += byValue;
    return self + y;
}

struct A {
    f1: int
    f2: int
}

fun A.assign(mutate self, obj: A) {
    self = obj;
}

fun A.assignWith(mutate self, mutate obj: A) {
    self = obj;
    obj.f2 = 0;
}

struct OnlyNullableTensor {
    t: (int, int)?;
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


@noinline
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
    var cs = "123".beginParse();
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

global t107_1: int
global t107_2: int

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
    builder.storeUint(mutate b, 8, 32);
    b = b.storeUint(9, 32).storeUint(10, 32);

    return b.bitsCount();
}

@method_id(111)
fun testStoreChainingCustom() {
    var b = beginCell().myStoreUint(1, 32).myStoreUint(2, 32).myStoreUint(3, 32);
    b.myStoreUint(4, 32);
    b.myStoreUint(5, 32).myStoreUint(6, 32);
    myStoreUint(mutate b, 7, 32);
    builder.myStoreUint(mutate b, 8, 32);
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

fun int.add(mutate self, dx: int) { self += dx }
fun int.del(mutate self) { self = 0; return self }

@method_id(117)
fun testReceiverAndNot() {
    var (a1: A, a2: A) = ({ f1: 10, f2: 20 }, { f1: 30, f2: 40 });
    a1.assign(a1);
    A.assign(mutate a1, a1);
    a1.assignWith(mutate a2);
    A.assignWith(mutate a2, mutate a1);
    return (a1, a2);
}

global callOrder: tuple;

fun int.assign(mutate self, v: int) {
    self = v;
}

fun getTensor_12() {
    callOrder.push(12);
    return (1, 2);
}

fun getIntValue5() {
    callOrder.push(5);
    return 5;
}

@method_id(118)
fun testNoExtraFunCallsOnMutate() {
    callOrder = createEmptyTuple();
    getTensor_12().0.assign(getIntValue5());
    return callOrder;
}

fun blockchain.getTuple(self): [int, int] {
    callOrder.push(8);
    return [1, 2];
}

@method_id(119)
fun testNoExtraFunCallsAlso() {
    callOrder = createEmptyTuple();
    var s = blockchain{};
    s.getTuple().1.assign(100);
    return (s, callOrder);
}

global g120: OnlyNullableTensor;

fun setFirstOfTensor(mutate x: (int, int)) {
    x.0 = 1;
}

@method_id(120)
fun testMutateNotNullGlobal(setNull: bool) {
    g120 = { t: setNull ? null : (10, 20) };
    if (g120.t != null) {
        var tmp = g120.t!;
        setFirstOfTensor(mutate tmp);       // can't `mutate g120.t!` (invalid lvalue), use a tmp var
        g120.t = tmp;
    }
    return g120
}

struct Box121 {
    v: int
}

fun Box121.inc(mutate self): self {
    self.v += 1;
    return self;
}

fun Box121.mix(mutate self, mutate x: int) {
    self.v = self.v * 100 + x;
}

@method_id(121)
fun testMutateSelfNested() {
    // total 4663439
    var a = Box121{ v: 7 };
    var b = Box121{ v: 8 };
    a.mix(mutate b.inc().v);
    return (a.v, b.v);
}

struct Counter122 { v: int }

fun Counter122.inc(mutate self): self { self.v += 1; return self; }
fun Counter122.combine(mutate self, mutate x: int, mutate y: int) {
    self.v = self.v + x + y;
    x -= 5;
    y -= 5;
}

@method_id(122)
fun test122() {
    var a = Counter122{v: 100};
    var b = Counter122{v: 10};
    var c = Counter122{v: 1};
    a.combine(mutate b.inc().inc().v, mutate c.inc().inc().v);
    return (a.v, b.v, c.v);
}

struct Box123 { v: int }

fun Box123.id(self): self { return self }
fun Box123.inc(mutate self): self { self.v += 1; return self; }
fun Box123.addMut(mutate self, mutate x: int) { self.v += x; }

fun Box123.doubleMutWithArgOrder(mutate self, mutate x: int): void asm (x self)
    "DROP"
    "DROP"
    "111 PUSHINT"   // new self.v
    "222 PUSHINT"   // new x

@method_id(123)
fun test123() {
    var a = Box123{v: 100};
    var b = Box123{v: 10};
    a.inc().addMut(mutate b.inc().v);
    return (a.v, b.v);
}

struct Box124 {
    v: int
}

@inline
fun Box124.id(self): self {     // not mutate
    return self;
}

fun Box124.inc(mutate self): self {
    self.v += 1;
    return self;
}

@method_id(124)
fun test124() {
    var a = 100;
    var b = Box124{ v: a }.id().inc();
    Box124{ v: a }.v = 500;
    Box124{ v: a }.v.incrementInPlace(10);
    Box124{ v: a }.id().v = 500;
    b.v.incrementInPlace(1);
    return (a, b);
}

global calls125: int;

struct Inner125 { v: int = 0 }
struct Outer125 { inner: Inner125 = {} }

fun makeOuter(): Outer125 {
    calls125 += 1;
    return {}
}

fun Inner125.bump(mutate self): self {
    self.v += 10;
    return self;
}

@method_id(125)
fun test125() {
    calls125 = 0;
    var j = makeOuter().inner.bump().bump();
    j.bump();
    return (calls125, j.v);
}

@method_id(126)
fun test126(k: int): (int, int) {
    var x = k;
    var b = Box123{ v: increment(mutate x) }.inc().inc();
    return (x, b.v);
}

global g127: Box123;

@method_id(127)
fun test127(k: int) {
    g127 = Box123{ v: k };
    val t = g127.id().inc().inc();
    return (g127.v, t.v);
}

@method_id(128)
fun test128() {
    var a1: Box123 = { v: 1 };
    var a2: Box123 = { v: 1 };
    var b1: Box123 = { v: 2 };
    var b2: Box123 = { v: 2 };

    a1.doubleMutWithArgOrder(mutate b1.v);
    a2.doubleMutWithArgOrder(mutate b2.id().v);
    return (a1, a2, b1, b2);
}

struct B129 { x: int }

fun mk129(mutate w: int, v: int): B129 { w += 1; return { x: v }; }
fun B129.id(self): self { return self; }

fun int.inc(mutate self) { self += 1; }

@method_id(129)
fun test129() {
    var b: B129 = { x: 0 };
    var w = 0;
    ((b = mk129(mutate w, 7))).id().x.inc();
    return (w, b.x);
}

@method_id(130)
fun test130() {
    var sh: [B129?] = [null];
    var w = 0;
    (sh.0 = mk129(mutate w, 10))!.id().x += 1;
    // w is mutated once; sh.0.x is modified via return-self writeback
    return (w, sh.0!!.x);
}

fun (int, int).id(self): self { return self; }
fun int.setOne(mutate self, dummy: int) { self = 1; }

@method_id(131)
fun test131() {
    var p = (7, 9);
    p.id().id().id().0.setOne(p.id().id().1 = 5);   // set p.0 = 1, set p.1 = 5
    return p;
}



// return-self wrapper in narrowed union branch: type check, width transition, writeback tracking

struct UA { x: int }
struct UB { y: int }
type UnionAB = UA | UB;

fun UnionAB.id(self): self { return self; }
fun UnionAB.toUA(mutate self) { self = UA { x: 1 }; }

fun int?.idNullable(self): self { return self; }
fun int?.assign42(mutate self) { self = 42; }

@method_id(132)
fun test132(): int {
    var x: int? = 10;
    if (x != null) {        // return-self wrapper in narrowed union branch
        x.idNullable().assign42();
    }
    return x;
}

@method_id(133)
fun test133(): int {
    var sh: [UnionAB] = [UB { y: 7 }];
    if (sh.0 is UB) {       // return-self wrapper in narrowed union branch
        var copy: UnionAB = sh.0.id();
        if (copy is UB) { return copy.y; }
    }
    return -1;
}

@method_id(134)
fun test134(): int {
    var sh: [UnionAB] = [UB { y: 7 }];
    if (sh.0 is UB) {       // return-self in narrowed, chained via id(), mutated, and writes back to a shape
        sh.0.id().toUA();
    }
    if (sh.0 is UA) { return sh.0.x; }
    return -1;
}

fun main(){
    // mark used to codegen them
    load_next;
    testStoreUintPureUnusedResult;
    testStoreUintImpureUnusedResult
}

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
@testcase | 117 |    | 30 0 30 40
@testcase | 118 |    | [ 12 5 ]
@testcase | 119 |    | [ 8 ]
@testcase | 120 | 0  | 1 20 typeid-1
@testcase | 120 | -1 | (null) (null) 0
@testcase | 121 |    | 709 9
@testcase | 122 |    | 115 7 -2
@testcase | 123 |    | 112 11
@testcase | 124 |    | 100 102
@testcase | 125 |    | 1 30
@testcase | 126 | 7  | 8 10
@testcase | 127 | 7  | 9 9
@testcase | 128 |    | 111 111 222 222
@testcase | 129 |    | 1 8
@testcase | 130 |    | 1 11
@testcase | 131 |    | 1 5
@testcase | 132 |    | 42
@testcase | 133 |    | 7
@testcase | 134 |    | 1

@fif_codegen
"""
  int.incrementInPlace() PROC:<{  //  self byValue
    ADD //  self
  }>
"""

@fif_codegen
"""
  testIncrement2() PROC:<{
    ...
    incrementTwoInPlace() CALLDICT        //  x y sum1
    -ROT
    10 PUSHINT  //  sum1 x y '10=10
    int.incrementTwoInPlace() CALLDICT        //  sum1 x y sum2
    s1 s3 s0 XCHG3       //  x y sum1 sum2
  }>
"""

@fif_codegen
"""
 load_next() PROC:<{  //  cs
    32 LDI      //  '3 cs
    SWAP        //  cs '3
  }>
"""

@fif_codegen
"""
  testStoreUintPureUnusedResult() PROC:<{
    0 PUSHINT	//  '10=0
  }>
"""

@fif_codegen
"""
  testStoreUintImpureUnusedResult() PROC:<{
    NEWC	//  b
    STIX	//  '2
    DROP	//
    0 PUSHINT	//  '10=0
  }>
"""

@fif_codegen
"""
  testReceiverAndNot() PROC:<{  //
    30 PUSHINT                  //  '6=30
    40 PUSHINT                  //  obj.f1=30 obj.f2=40
    OVER
    0 PUSHINT                   //  a1.f1=30 a2.f2=40 a2.f1=30 a1.f2=0
    s0 s2 XCHG                  //  a1.f1=30 a1.f2=0 a2.f1=30 a2.f2=40
  }>
"""

@fif_codegen
"""
  test131() PROC:<{
    5 PUSHINT
    1 PUSHINT
    SWAP
  }>
"""

 */
