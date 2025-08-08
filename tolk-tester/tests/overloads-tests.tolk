struct Wrapper<T> { item: T }
struct Nothing<T> {}
struct MyPair<K, V> {}

struct Point { x: int8; y: int8 }

type MInt = int
type MSlice = slice
type PointAlias = Point

fun T.realValue(self): int1 { return 0 }
fun TT?.realValue(self): int2 { return 0 }
fun int.realValue(self): int3 { return 0 }
fun int8.realValue(self): int4 { return 0 }
fun int?.realValue(self): int5 { return 0 }
fun Nothing<U>.realValue(self): int6 { return 0 }
fun Nothing<Nothing<T>>.realValue(self): int7 { return 0 }
fun Nothing<T?>.realValue(self): int8 { return 0 }

fun test1() {
    __expect_type(beginCell().realValue(), "int1");
    __expect_type((5 as int|slice).realValue(), "int1");
    __expect_type((beginCell() as builder?).realValue(), "int2");
    __expect_type((null as null|Nothing<int>).realValue(), "int2");
    __expect_type((null as Nothing<int>|null).realValue(), "int2");
    __expect_type(5.realValue(), "int3");
    __expect_type((5 as int?).realValue(), "int5");
    __expect_type((5 as int8).realValue(), "int4");
    __expect_type((5 as int8?).realValue(), "int2");
    __expect_type(Nothing<int>{}.realValue(), "int6");
    __expect_type(Nothing<MInt>{}.realValue(), "int6");
    __expect_type(Nothing<int?>{}.realValue(), "int8");
    __expect_type(Nothing<MInt?>{}.realValue(), "int8");
    __expect_type(Nothing<slice>{}.realValue(), "int6");
    __expect_type(Nothing<Nothing<int>>{}.realValue(), "int7");
    __expect_type(Nothing<Nothing<Nothing<int>>>{}.realValue(), "int7");
    __expect_type(Nothing<Nothing<Nothing<int>>?>{}.realValue(), "int8");
}

fun T.doSome2(self): int { return 0 }
fun int8.doSome2(self): cell { return createEmptyCell() }
fun int16.doSome2(self): slice { return "" }

fun test2() {
    __expect_type(5.doSome2(), "int");
    __expect_type((5 as int8).doSome2(), "cell");
    __expect_type(((5 as int8) as int16).doSome2(), "slice");
    __expect_type((5 as int?).doSome2(), "int");
    __expect_type(5.doSome2().doSome2(), "int");
    __expect_type((1, 2).doSome2(), "int");
}

fun MyPair<int, slice>.mapFn(self): int1 { return 0 }
fun MyPair<K, slice>.mapFn(self): int2 { return 0 }
fun MyPair<int, V>.mapFn(self): int3 { return 0 }
fun MyPair<K, V>.mapFn(self): int4 { return 0 }

fun test3() {
    __expect_type(MyPair<int, slice>{}.mapFn(), "int1");
    __expect_type(MyPair<MInt, slice>{}.mapFn(), "int1");
    __expect_type(MyPair<int8, slice>{}.mapFn(), "int2");
    __expect_type(MyPair<int8, MSlice>{}.mapFn(), "int2");
    __expect_type(MyPair<int, slice?>{}.mapFn(), "int3");
    __expect_type(MyPair<int8, cell>{}.mapFn(), "int4");
    __expect_type(MyPair<never, (slice,slice)>{}.mapFn(), "int4");
}

fun (int|slice).unionMix(self): slice { return "" }
fun int.unionMix(self): bool { return true }

fun test4() {
    __expect_type((5 as int).unionMix(), "bool");
    __expect_type(("".unionMix()), "slice");
    __expect_type(((5 as int|slice).unionMix()), "slice");
}

fun WrapA<T>.dup(self): WrapA<T> { return self }
fun WrapA<WrapA<U>>.dup(self): WrapA<WrapA<U>> { return self }
struct WrapA<T> {}

fun test5() {
    __expect_type(WrapA<int>{}.dup(), "WrapA<int>");
    __expect_type(WrapA<WrapA<slice>>{}.dup(), "WrapA<WrapA<slice>>");
}

type AliasBuilder1 = builder
type AliasBuilder2 = builder

fun AliasBuilder1.myBAlias(self): int1 { return 0 }
fun AliasBuilder2.myBAlias(self): int2 { return 0 }
fun builder?.myBAlias(self): int3 { return 0 }
fun T.myBAlias(self): int4 { return 0 }
fun U?.myBAlias(self): int5 { return 0 }

fun test6(b: builder, b1: AliasBuilder1, b2: AliasBuilder2, b1n: AliasBuilder1?, b2n: AliasBuilder2?) {
    __expect_type(b1.myBAlias(), "int1");
    __expect_type(b2.myBAlias(), "int2");
    __expect_type(b1n.myBAlias(), "int3");
    __expect_type((b2n as builder?).myBAlias(), "int3");

    __expect_type((b as builder|slice).myBAlias(), "int4");
    __expect_type(0.myBAlias(), "int4");
    __expect_type((0 as int|int16).myBAlias(), "int4");
    __expect_type(b2n.myBAlias(), "int3");
    __expect_type((null as builder|slice?).myBAlias(), "int5");
    __expect_type((0 as int?).myBAlias(), "int5");
}

type AssetBuilder = builder
type BalanceBuilder = builder

fun AssetBuilder.validate(self): int1 { return 0 }
fun BalanceBuilder.validate(self): int2 { return 0 }
fun Wrapper<AssetBuilder>.validate(self): int3 { return 0 }
fun Wrapper<BalanceBuilder>.validate(self): int4 { return 0 }

fun test7() {
    var a: AssetBuilder = beginCell();
    var b: BalanceBuilder = beginCell();

    __expect_type(a.validate(), "int1");
    __expect_type(b.validate(), "int2");
    __expect_type(Wrapper{item:a}.validate(), "int3");
    __expect_type(Wrapper{item:b}.validate(), "int4");
}

fun (slice|TT).supposeMe(self): TT { if (self is slice) { throw 123 } return self }
fun T.supposeMe(self): int2 { return 0 }
fun int|slice|null.supposeMe(self): int3 { return 0 }

fun test8(a: cell|slice, b: cell|()|slice|builder, c:slice, d:int|slice, e:null|(slice|int), f: int?, g:cell) {
    __expect_type(a.supposeMe(), "cell");
    __expect_type(b.supposeMe(), "cell | () | builder");
    __expect_type(c.supposeMe(), "slice");
    __expect_type(d.supposeMe(), "int");
    __expect_type(e.supposeMe(), "int3");
    __expect_type(f.supposeMe(), "int2");
    __expect_type(g.supposeMe(), "int2");
}

fun MyPair<K, V>.mySize(self): int1 { return 0 }
fun MyPair<int8, V>.mySize(self): int2 { return 0 }
fun MyPair<K, MyPair<K, K>>.mySize(self): int3 { return 0 }
fun MyPair<K, MyPair<K, V>>.mySize(self): int4 { return 0 }
fun MyPair<K, MyPair<AnotherK, V>>.mySize(self): int5 { return 0 }

fun test9(
    m1: MyPair<int16, slice>, m2: MyPair<int8, slice>,
    m3: MyPair<int4, MyPair<int4, int4>>, m4: MyPair<Point, MyPair<Point, PointAlias>>,
    m5: MyPair<int8, MyPair<int8, int32>>, m6: MyPair<int8, MyPair<int8, int8?>>,
    m7: MyPair<Point, MyPair<uint256, slice>>, m8: MyPair<Point, MyPair<bool, Point>>,
    m9: MyPair<int8, MyPair<int8, int8>?>, m10: MyPair<address, MyPair<int9, Point>?>,
    m11: MyPair<address, MyPair<address, int32>>, m12: MyPair<bool, MyPair<bool, int8?>>,
) {
    __expect_type(m1.mySize(), "int1");
    __expect_type(m2.mySize(), "int2");
    __expect_type(m3.mySize(), "int3");
    __expect_type(m4.mySize(), "int3");
    __expect_type(m5.mySize(), "int4");
    __expect_type(m6.mySize(), "int4");
    __expect_type(m7.mySize(), "int5");
    __expect_type(m8.mySize(), "int5");
    __expect_type(m9.mySize(), "int2");
    __expect_type(m10.mySize(), "int1");
    __expect_type(m11.mySize(), "int4");
    __expect_type(m12.mySize(), "int4");
}

fun MyPair<K, MyPair<K, K|slice>>.h1(self): int1 { return 0 }
fun MyPair<K, MyPair<K, K|V|slice>>.h1(self): int2 { return 0 }
fun MyPair<K, MyPair<K|slice, K|slice>>.h1(self): int3 { return 0 }
fun MyPair<K, MyPair<K2|slice, V2|slice>>.h1(self): int4 { return 0 }

fun test10() {
    __expect_type(MyPair<int8, MyPair<int8, int8|slice>>{}.h1(), "int1");
    __expect_type(MyPair<int8, MyPair<int8, int8|int16|slice>>{}.h1(), "int2");
    __expect_type(MyPair<int8, MyPair<int8|slice, int8|slice>>{}.h1(), "int3");
    __expect_type(MyPair<int8, MyPair<int8|int16|slice, int8|slice>>{}.h1(), "int4");
    __expect_type(MyPair<int8, MyPair<int8|int16|slice, int8|int16|slice>>{}.h1(), "int4");
}

fun (int, U).p(self): int1 { return 0 }
fun (T, U).p(self): T { return self.0 }
fun T.p(self): int3 { return 0 }

fun test11() {
    __expect_type((5, "").p(), "int1");
    __expect_type((5 as int8, "").p(), "int8");
    __expect_type(("", "").p(), "slice");
    __expect_type((1, 2, 3).p(), "int3");
}

fun Wrapper<T|slice>.w(self): int1 { return 0 }
fun Wrapper<T>.w(self): int2 { return 0 }

fun test12() {
    __expect_type(Wrapper<int|slice>{item:0}.w(), "int1");
    __expect_type(Wrapper<PointAlias|slice>{item:""}.w(), "int1");
    __expect_type(Wrapper<Wrapper<int>>{item:{item:0}}.w(), "int2");
    // here the compiler finds T=slice resulting in Wrapper<slice|slice> (it's correct)
    // and chooses (1) due to a deeper shape
    __expect_type(Wrapper<slice>{item:""}.w(), "int1");
}

type MyCell<T> = cell
type MInt8 = int8
type MInt32 = int32

fun MyCell<T>.makeIterator(self): MyIterator<T> {
    return MyIterator<T>.new(self)
}

struct MyIterator<T> {
    data: slice
}

fun MyIterator<T>.new(c: cell): MyIterator<T> {
    return { data: c.beginParse() }
}

fun MyIterator<T>.empty(self): bool {
    return self.data.isEmpty()
}

fun MyIterator<T>.next(mutate self): T {
    return self.data.loadAny<T>()
}

fun MyCell<T>.iterateAndAppend(self, mutate t: tuple) {
    var it = self.makeIterator();
    while (!it.empty()) {
        t.push(it.next())
    }
}

fun MyCell<MInt32>.iterateAndAppend(self, mutate t: tuple) {
    t.push(-777);
}

@method_id(113)
fun test13() {
    var c = beginCell().storeSlice(stringHexToSlice("0102")).endCell();
    var t = createEmptyTuple();
    (c as MyCell<int8>).iterateAndAppend(mutate t);
    (c as MyCell<int16>).iterateAndAppend(mutate t);
    (c as MyCell<MInt8>).iterateAndAppend(mutate t);
    (c as MyCell<MInt32>).iterateAndAppend(mutate t);
    return t;   // [ 1 2 258 1 2 -777 ]
}


fun map<K, V>.mySize(self): int1 { return 0 }
fun map<K, V|cell>.mySize(self): int2 { return 0 }
fun map<int8, V>.mySize(self): int3 { return 0 }
fun map<int8, map<K, V>>.mySize(self): int4 { return 0 }
fun map<int8, map<int16, V>>.mySize(self): int5 { return 0 }

type MInt16 = int16
type MInt16ov = MInt16
type MMap32 = map<int32, map<bool, varint16>>
type MMap8Of32 = map<int8, MMap32>

fun test14(
    m1: map<int8, map<int16, int32>>, m2: map<int8, map<MInt16ov, MInt16ov>>,
    m3: map<int8, map<Point, slice>>, m4: map<int8, MMap32>, m5: MMap8Of32,
    m6: map<int8, map<int8,int8>?>, 
    m7: map<int8, bool|cell>, m8: map<MInt16, ()|cell|Point>,
    m9: map<address, address>,
) {
    __expect_type(m1.mySize(), "int5");
    __expect_type(m2.mySize(), "int5");
    __expect_type(m3.mySize(), "int4");
    __expect_type(m4.mySize(), "int4");
    __expect_type(m5.mySize(), "int4");
    __expect_type(m6.mySize(), "int3");
    __expect_type(m7.mySize(), "int2");
    __expect_type(m8.mySize(), "int2");
    __expect_type(m9.mySize(), "int1");
}

fun main() {
    // besides these positive tests, there are many negative resulting in "ambiguous call"
    return 0
}

/**
@testcase | 0   |   | 0
@testcase | 113 |   | [ 1 2 258 1 2 -777 ]
 */
