import "@stdlib/common.tolk"
type MInt = int;

global callOrder: tuple;

fun int.zero() { return 0; }
fun int.plus1(self) { return self + 1; }
fun MInt.plusN(self, n: int) { return self + n; }
fun int.main(self) { return self; }

struct Point {
    x: int;
    y: int;
}

fun Point.create0() { return Point.create(0, 0); }
fun Point.create(x: int, y: int): Point { return {x,y}; }

fun Point.getMaxCoord(self) { return self.x > self.y ? self.x : self.y; }
fun Point.incX(mutate self) { self.x += 1; }
fun Point.incXMutateY(mutate self, mutate curY: int) { self.x += 1; curY = self.y; }

fun Point?.isNull(self) { return self == null; }

struct Pair<A,B> {
    first: A;
    second: B;
}

fun Pair<int,int>.create0(): Pair<int?,int?> {
    return { first: 0, second: 0 };
}
fun Pair<A,B>.create0(): Pair<A,B> {
    return { first: 0, second: 0 };
}
fun Pair<A,B>.createFrom<U,V>(first: U, second: V): Pair<A,B> {
    return { first: first as A, second: second as B };
}
fun Pair<T1,T2>.is0(self) {
    return self.first == 0 && self.second == 0;
}
fun Pair<A,B>.compareWith<U,V>(self, f: U, s: V) {
    return self.first == f && self.second == s;
}

type Tuple2Int = [int, int];
fun [int, int].getLast(self) { return self.1; }
fun [int, MInt].assignLast(mutate self, v: int) { self.1 = v; }
fun Tuple2Int.getSum(self) { return self.0 + self.1; }

fun int.calcExactF(self) { return self; }
fun int8.calcExactF(self) { return self * 2; }
fun int8?.calcExactF(self) { return self == null ? 0 : self * 3; }

fun T.copy(self) { return self; }
fun T.assign<T2>(mutate self, value: T2) { self = value; }

struct Wrapper<T> { item: T; }

fun Wrapper<T>.create(initial: T): Wrapper<T> { return { item: initial }; }
fun Wrapper<T>.getItem(self) { return self.item; }
fun Wrapper<int8>.getItem(self) { return 100500; }
fun createWrapper<T>(item: T) { return Wrapper<T>.create(item); }

@method_id(101)
fun test1() {
    var i = int.zero() + MInt.zero();
    i = i.plus1();
    return (i.plus1(), i, i.plusN(5), i.plusN(i.plus1()));
}

@method_id(102)
fun test2(a: int8, b: int16) {
    var c = b as coins?;
    return (a.plus1(), b.plusN(a), c!.plus1());
}

@method_id(103)
fun test3() {
    var p = Point.create0();
    var yy = 10;
    p.incX();
    Point.incX(mutate p);
    p.incXMutateY(mutate yy);
    p.y = p.x;
    Point.incXMutateY(mutate p, mutate yy);
    return (p, p.getMaxCoord(), Point.getMaxCoord(p), yy);
}

@method_id(104)
fun test4() {
    var p0 = Point.create0();
    var p5 = Point.create(5,5) as Point?;
    var pN = null as Point?;
    return (p0.isNull(), p5.isNull(), pN.isNull(), Point.isNull(null), Point.isNull({x:3,y:3}));
}

@method_id(105)
fun test5(a: MInt, b: int) {
    var t = [a,b];
    return (t.getSum(), t.assignLast(a+b), t.getSum(), Tuple2Int.getLast(t));
}

@method_id(106)
fun test6() {
    var x = 5;
    var x8 = x as int8;
    return (
        x.calcExactF(), x8.calcExactF(), (8 as int8?).calcExactF(), (null as int8?).calcExactF(),
        (5 as int16).calcExactF(), (x8 as coins).calcExactF(), (x8 as MInt).calcExactF(),
        int8.calcExactF(x)
    );
}

@method_id(107)
fun test7() {
    __expect_type(5.copy(), "int");
    __expect_type((5 as int99).copy(), "int99");
    __expect_type(beginCell().storeInt(1,32).copy(), "builder");
    __expect_type(((null, null) as (MInt?,bool?)).copy(), "(MInt?, bool?)");
    return (
        beginCell().storeInt(1,32).endCell().beginParse().loadInt(32).copy() + 1,
        stringHexToSlice("01").copy().loadInt(8),
        stringHexToSlice("FF").loadInt(8),
    );
}

@method_id(108)
fun test8(x: int32) {
    var cp = x.copy();
    x.assign(10);
    int32.assign(mutate x, x+cp);
    (x as int18).assign(x+2);
    __expect_type(x, "int32");
    return (x, cp);
}

@method_id(109)
fun test9() {
    var w1 = Wrapper<int>.create(5);
    var w2 = Wrapper<int8>.create(10);
    var w3 = createWrapper(20 as int16);
    var w4 = Wrapper<Wrapper<slice>>.create({item:beginCell().storeInt(30,32).storeInt(31,32).endCell().beginParse()});
    __expect_type(w1, "Wrapper<int>");
    __expect_type(w2, "Wrapper<int8>");
    __expect_type(w3, "Wrapper<int16>");
    __expect_type(w4, "Wrapper<Wrapper<slice>>");
    __expect_type(w4.getItem(), "Wrapper<slice>");
    __expect_type(w4.getItem().getItem(), "slice");
    return (
        w1.getItem(), w2.getItem(), w3.getItem(), w4.item.item.loadInt(32),
        Wrapper<int>.getItem(w1), w1.getItem(), Wrapper<Wrapper<slice>>.getItem(w4).getItem().loadInt(32)
    );
}


fun T.isAnotherNull(self) { callOrder.push(1); return self == null; }
fun int.isAnotherNull(self) { callOrder.push(2); return false; }
fun int?.isAnotherNull(self) { callOrder.push(3); return false; }

@method_id(110)
fun test10() {
    callOrder = createEmptyTuple();
    (
        5.isAnotherNull(), (5 as int?).isAnotherNull(), beginCell().isAnotherNull(), (beginCell() as builder?).isAnotherNull(),
        null.isAnotherNull(), true.isAnotherNull(), ((1,null) as (int,slice?)).isAnotherNull()
    );
    return (callOrder, null.isNull());
}

fun int.anyMethod(self) { callOrder.push(1); }
fun slice?.anyMethod(self) { callOrder.push(2); }
fun T.anyMethod(self) { callOrder.push(3); }
fun callAnyMethod<T>(obj: T) { return obj.anyMethod(); }

@method_id(111)
fun test11() {
    callOrder = createEmptyTuple();
    (callAnyMethod(10), callAnyMethod(null as slice?), callAnyMethod<builder?>(null),
     callAnyMethod<int?>(null), callAnyMethod<slice?>(null));
    return callOrder;
}

fun int?.arbitraryMethod(self) { return self; }
fun Wrapper<MInt>.arbitraryMethod(self) { return self; }
fun Wrapper<int8>.arbitraryMethod(self) { return beginCell(); }
fun Wrapper<Wrapper<int>>.arbitraryMethod(self) { return beginCell(); }
fun T?.arbitraryMethod(self): never { throw 123; }

fun test12() {
    __expect_type(10.arbitraryMethod(), "int?");
    __expect_type((10 as int8?).arbitraryMethod(), "int?");
    __expect_type((10 as coins?)!.arbitraryMethod(), "int?");

    __expect_type(null.arbitraryMethod(), "int?");
    __expect_type((10 as coins?).arbitraryMethod(), "int?");

    __expect_type(Wrapper { item: 10 }.arbitraryMethod(), "Wrapper<MInt>");
    __expect_type(Wrapper<int> { item: 10 as int8 }.arbitraryMethod(), "Wrapper<MInt>");

    __expect_type(Wrapper { item: 10 as int8 }.arbitraryMethod(), "builder");
    __expect_type(Wrapper { item: Wrapper { item: 10 } }.arbitraryMethod(), "builder");

    __expect_type((Wrapper { item: 10 } as Wrapper<int>?).arbitraryMethod(), "never");
    __expect_type((null as slice?).arbitraryMethod(), "never");
}

type BuilderOrInt = builder|int;
fun (int|builder).isInt(self) { return self is int; }

@method_id(113)
fun test13() {
    var u1 = beginCell() as builder|int;
    var u2 = 10 as int|builder;
    var u3 = 20 as BuilderOrInt;
    return (u1.isInt(), u2.isInt(), u3.isInt(), 10.isInt(), beginCell().isInt(), (8 as int8).isInt());
}

fun (int|builder|T).isBuilder(self) { return self is builder; }

@method_id(114)
fun test14() {
    var b1 = beginCell() as BuilderOrInt | slice;
    var b2 = beginCell() as builder|slice|int|Point;
    var i1 = 5 as int|builder|continuation|(int, int);
    var c = beginCell().endCell() as cell?|builder|int;
    var cn = null as cell?|int|int8|builder;
    return (b1.isBuilder(), b2.isBuilder(), i1.isBuilder(), c.isBuilder(), cn.isBuilder());
}

@method_id(115)
fun test15() {
    var p1 = Pair<int,int>.create0();
    __expect_type(p1, "Pair<int?, int?>");
    var p2 = Pair<int8,int16>.create0();
    __expect_type(p2, "Pair<int8, int16>");
    var p3 = Pair<int16,int8>.createFrom(10, (20 as coins?)!);
    __expect_type(p3, "Pair<int16, int8>");
    return (p2.is0(), p2.compareWith(p3.first, p3.second));
}

struct CounterIncrement { inc_by: int; }
struct CounterReset { initial_value: int; }
type CounterMsg = CounterIncrement | CounterReset;

fun CounterReset.onInternalMessage(self) {
    throw 123;
}
fun CounterMsg.onInternalMessage(self) {
    return match (self) {
        CounterIncrement => self.inc_by,
        CounterReset => self.initial_value,
    };
}

@method_id(116)
fun test16() {
    var inc: CounterIncrement = { inc_by: 100 };
    var msg = inc as CounterMsg;
    return (inc.onInternalMessage(), msg.onInternalMessage(), CounterMsg.onInternalMessage(CounterReset{initial_value:5}));
}

fun int.`0`() { return 0; }
fun int.`1`(self) { return self + 1; }

@method_id(117)
fun test17() {
    var zero = int.0();
    return zero.1() + int.0().1() + int.1(int.0());
}

fun Wrapper<T?>.createNull(): Wrapper<T?> { return { item: null }; }
fun Wrapper<T>.createFrom<U>(item: U): Wrapper<T> { return {item}; }

@method_id(118)
fun test18() {
    __expect_type(10.copy, "(int) -> int");
    __expect_type(Wrapper{item:null as int8?}.copy, "(Wrapper<int8?>) -> Wrapper<int8?>");
    __expect_type(Wrapper<int>.createNull, "() -> Wrapper<int?>");
    __expect_type(Wrapper<Wrapper<slice?>?>.createNull, "() -> Wrapper<Wrapper<slice?>?>");
    __expect_type(Wrapper<int?>.createFrom<int8>, "(int8) -> Wrapper<int?>");

    var cb = Wrapper<int?>.createFrom<int8>;
    return cb(10);
}

fun main() {}

/**
@testcase | 101 |        | 2 1 6 3
@testcase | 102 | 5 6    | 6 11 7
@testcase | 103 |        | 4 3 4 4 3
@testcase | 104 |        | 0 0 -1 -1 0
@testcase | 105 | 5 6    | 11 16 11
@testcase | 106 |        | 5 10 24 0 5 5 5 10
@testcase | 107 |        | 2 1 -1
@testcase | 108 | 5      | 17 5
@testcase | 109 |        | 5 100500 20 30 5 5 31
@testcase | 110 |        | [ 2 3 1 1 3 1 1 ] -1
@testcase | 111 |        | [ 1 2 3 3 2 ]
@testcase | 113 |        | 0 -1 -1 -1 0 -1
@testcase | 114 |        | -1 -1 0 0 0
@testcase | 115 |        | -1 0
@testcase | 116 |        | 100 100 5
@testcase | 117 |        | 3
@testcase | 118 |        | 10

@fif_codegen DECLPROC Pair<int16,int8>.createFrom<int,coins>
@fif_codegen DECLPROC Pair<int8,int16>.compareWith<int16,int8>
@fif_codegen DECLPROC CounterMsg.onInternalMessage
 */
