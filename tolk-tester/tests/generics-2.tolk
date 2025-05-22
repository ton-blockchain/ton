struct Empty {}

struct Wrapper<T> {
    value: T;
}

type PairAlias<A, B> = Pair<A, B>;
struct Pair<T1, T2> {
    first: T1,
    second: T2
}

type MyInt = int;
type WrappedInt = Wrapper<int>;
type WrapperAlias<T> = Wrapper<T>;

struct DeepNested<T> {
    wrapper: WrapperAlias<Wrapper<T>>;
    pair: PairAlias<T, T>;
}


fun eq<T>(v: T): T { return v; }

fun swap<A, B>(p: Pair<A, B>): Pair<B, A> {
    return { first: p.second, second: p.first };
}

fun wrap<T>(value: T): Wrapper<T> {
    return { value };
}

fun wrap2<T>(value: T) {
    return WrapperAlias<T> { value };
}

fun makePair<A, B>(a: A, b: B) {
    var c: Pair<A,B> = { first: a, second: b };
    return c;
}


@method_id(101)
fun test1() {
    var c1: Wrapper<int> = { value: 23 };
    __expect_type(c1, "Wrapper<int>");
    var c2: WrapperAlias<int> = { value: 23 };
    __expect_type(c2, "WrapperAlias<int>");
    __expect_type(c2 as Wrapper<int>, "Wrapper<int>");
    __expect_type(c2 as WrappedInt, "WrappedInt");
    return (c1, c2);
}

@method_id(102)
fun test2(c: Wrapper<int>) {
    var c2 = Wrapper<int> { value: c.value + 1 };
    var c3 = Wrapper { value: c.value + c2.value };
    __expect_type(c3, "Wrapper<int>");
    var c4: Wrapper<int8>? = c.value > 10 ? null : { value: c.value };
    return (c, c2, c3, c4);
}

@pure
fun getWrappervalue1<T>(c: Wrapper<T>) {
    return c.value;
}

@pure
fun Wrapper<T>.getWrappervalue1(self) {
    return self.value;
}

@pure
fun getWrappervalue2<T>(c: T) {
    return c.value;
}

@pure
fun T.getWrappervalue2(self) {
    return self.value;
}

@method_id(103)
fun test3() {
    var c1 = wrap(10);
    var c2 = wrap(10 as int?);
    var c3 = wrap2<(int, int)?>(null);
    __expect_type(c1, "Wrapper<int>");
    __expect_type(c2, "Wrapper<int?>");
    __expect_type(c3, "Wrapper<(int, int)?>");
    return (getWrappervalue1(c1), getWrappervalue1(c2), getWrappervalue1(c3), 777, getWrappervalue2(c1), getWrappervalue2(c2), getWrappervalue2(c3));
}

@method_id(104)
fun test4() {
    var c1 = wrap(10);
    var c2 = wrap(c1);
    var c3 = wrap(Wrapper { value: beginCell() });
    __expect_type(c2, "Wrapper<Wrapper<int>>");
    __expect_type(c3, "Wrapper<Wrapper<builder>>");
    __expect_type(c3.value, "Wrapper<builder>");
    __expect_type(c3.value.value, "builder");
    c3.value.value.storeInt(1, 32).storeInt(2, 32).storeInt(3, 32);
    var c4 = wrap2(Wrapper { value: getWrappervalue1(c3.value).endCell().beginParse() });
    __expect_type(c4, "Wrapper<Wrapper<slice>>");
    var sumOf1 = 0;
    sumOf1 += getWrappervalue1(getWrappervalue1(c4)).loadInt(32);     // a function returns a copy
    sumOf1 += getWrappervalue2(c4.value).loadInt(32);                   // so original slice isn't mutated
    return (c1.value += c4.value.value.loadInt(32), c1.getWrappervalue1(), c1.value += c4.value.value.loadInt(32), c1.getWrappervalue2(), c1, sumOf1);
}

@method_id(105)
fun test5(setNull: bool) {
    var c1: Wrapper < WrappedInt >? = { value: { value: 80 } };
    __expect_type(c1, "Wrapper<WrappedInt>");
    if (setNull) {
        c1 = null;
    }
    __expect_type(c1, "Wrapper<WrappedInt>?");
    return c1;
}

fun createNestedWrapperOrNull<T>(setNull: bool, initVal: T) {
    if (setNull) {
        return null;
    }
    var c1 = Wrapper<Wrapper<T>> {value:{value:initVal}};
    var c2: Wrapper< Wrapper<(T)> > = {value:{value:initVal}};
    var c3: Wrapper< Wrapper<((T | T))> > = WrapperAlias {value:{value:initVal}};
    var c4: Wrapper< Wrapper<((T | T))> > = Wrapper<Wrapper<T>> {value:c3.value};
    var c5: Wrapper< WrapperAlias<((T | T))> > = WrapperAlias<Wrapper<T>> {value:c3.value};
    var c6: WrapperAlias< WrapperAlias<((T | T))> > = Wrapper<Wrapper<T>> {value:c3.value};
    return c1;
}

@method_id(106)
fun test6() {
    var c1 = createNestedWrapperOrNull(true, (1, 2));
    var c2 = createNestedWrapperOrNull(false, (1, 2));
    __expect_type(c1, "Wrapper<Wrapper<(int, int)>>?");
    getWrappervalue1(c2!);
    getWrappervalue2(c2!);
    return (c1, 777, c2);
}

@method_id(107)
fun test7(i8: int8) {
    var c1 = Pair { first: 10, second: i8 };
    var c2 = eq(c1);
    __expect_type(c2, "Pair<int, int8>");
    var c3: Pair<int?, (int, int)?> = { first: null, second: null };
    return (c2.first += 10, c2, swap(c2), 777, c3);
}

@method_id(108)
fun test8(lastNull: bool) {
    var c1 = Wrapper { value: () };
    var c2 = Pair<Empty, Pair<Empty,Empty>> { first: {}, second: {first:{},second:{}} };
    var c3 = Pair { first: c1, second: Wrapper { value: Empty{} } };
    var c4 = Pair { first: c1, second: Wrapper { value: lastNull ? null : Empty{} } };
    __expect_type(c4, "Pair<Wrapper<()>, Wrapper<Empty?>>");
    return (c1, c2, c3, 777, c4, 777, c4.first, c4.second.value);
}

@method_id(109)
fun test9() {
    var c1: WrappedInt = { value: 10 };
    var c2: WrappedInt = WrappedInt { value: 20 };
    var c3: WrappedInt = Wrapper { value: 30 };
    var c4: Wrapper<int> = { value: 40 };
    c1 = c2 = c3 = c4;
    __expect_type(c1, "WrappedInt");
    __expect_type(c1 as Wrapper<int>, "Wrapper<int>");

    var c5: Pair<Wrapper<int>, WrappedInt> = { first: {value:50}, second: {value:50} };
    c5.first = c1; c5.first = c4;
    c5.second = c1; c5.second = c4;

    var c6 = Wrapper<Wrapper<int>> { value: { value: 60 } };
    var c7 = Wrapper<WrappedInt> { value: { value: 70 } };
    c6 = c7; c7 = c6;
    c6 as Wrapper<WrappedInt>;
    c7 as Wrapper<Wrapper<int>>;

    var p1: PairAlias<MyInt, WrappedInt> = { first: 42, second: WrappedInt { value: 200 } };
    __expect_type(p1, "PairAlias<MyInt, WrappedInt>");
    __expect_type(p1 as Pair<int, Wrapper<int>>, "Pair<int, Wrapper<int>>");
    var p2: Pair<int, Wrapper<int>> = p1;

    __expect_type(DeepNested<int> {
        wrapper: wrap(wrap(999)),
        pair: makePair(10, 20),
    }, "DeepNested<int>");

    return (c5, c7);
}

type Int16Or8 = int8 | int16;

@method_id(110)
fun test10() {
    var p1 = Pair<int|builder, int8|int16> { first: 5, second: 10 as int8 };
    var p2: Pair<builder|int, int16|int8> = { first: 10, second: 5 as int16 };
    var p3 = Pair { first: 20 as int|builder, second: (20 as int8) as Int16Or8 };
    __expect_type(p1, "Pair<int | builder, int8 | int16>");
    __expect_type(p2, "Pair<builder | int, int16 | int8>");
    __expect_type(p3, "Pair<int | builder, Int16Or8>");
    p1 = p2; p1 = p3; p2 = p1; p2 = p3; p3 = p1; p3 = p2;
    p2 as Pair<int|builder, int8|int16>; p3 as Pair<int|builder, int8|int16>;
    p1 as Pair<builder|int, int16|int8>; p3 as Pair<builder|int, int16|int8>;
    p1 as Pair<int|builder,Int16Or8>;    p3 as Pair<int|builder,Int16Or8>;
    return p3;
}

@method_id(111)
fun test11() {
    // type_id of these instantiations are the same
    var c1 = { value: 5 } as int | Wrapper<int|slice|builder>;
    var c2 = { value: 5 } as int | Wrapper<int|builder|slice>;
    var c3 = { value: 5 } as int | Wrapper<builder|slice|int>;
    var c4 = { value: 5 } as int | Wrapper<slice|int|builder>;
    c1 = c1; c1 = c2; c1 = c3; c1 = c4;
    c2 = c1; c2 = c2; c2 = c3; c2 = c4;
    c3 = c1; c3 = c2; c3 = c3; c3 = c4;
    c4 = c1; c4 = c2; c4 = c3; c4 = c4;
    return (c1, c2, c3, c4);
}

@method_id(112)
fun test12(is1: bool) {
    var c1: Wrapper<int|builder> = { value: 10 };
    var c2: Wrapper<builder|int> = { value: 20 };
    var c3: Wrapper<int|builder> | WrapperAlias<builder|int> = is1 ? c1 : c2;
    __expect_type(c3, "Wrapper<int | builder>");
    var m3: Wrapper<int|builder> | Wrapper<builder|int|null> = is1 ? c1 : Wrapper<builder?|int> { value: null };
    __expect_type(m3, "Wrapper<int | builder> | Wrapper<builder | int | null>");
    return (c3, 777, m3);
}

fun makeIdentity<T>(): (T) -> T {
    return eq<T>;
}

@method_id(113)
fun test13() {
    return makeIdentity<Pair<int,bool>>()({ first: 30, second: true });
}

struct Storage<T> {
    Wrapper: Wrapper<T>;
    backup: T?;
}

@method_id(114)
fun test14() {
    var s1 = Storage { Wrapper: wrap(999), backup: null };
    var s2 = Storage { Wrapper: wrap(makePair(1, "")), backup: makePair(2, "") };
    __expect_type(s1, "Storage<int>");
    __expect_type(s2, "Storage<Pair<int, slice>>");
    return (s1, s2.backup!.first);
}

struct DualStorage<T, U> {
    primary: Wrapper<T>;
    secondary: Wrapper<U>;
}

type IntOrBuilder = int | builder;

@method_id(115)
fun test15() {
    var s1 = DualStorage { primary: wrap(10), secondary: wrap(false), };
    __expect_type(s1, "DualStorage<int, bool>");
    var s2: DualStorage<IntOrBuilder, bool> = { primary: wrap(100 as IntOrBuilder), secondary: wrap(true), };
    var s3 = DualStorage<int|builder, bool> { primary: wrap(200 as IntOrBuilder), secondary: wrap(true), };
    return (s1, s2 = s3, s3 = s2);
}

type ComplexWrapPair<T> = Wrapper<Pair<T, T>>;

@method_id(116)
fun test16() {
    var c1: ComplexWrapPair<int> = wrap(makePair(1, 2));
    __expect_type(c1, "ComplexWrapPair<int>");
    var c2 = wrap(makePair(3, 4));
    __expect_type(c2, "Wrapper<Pair<int, int>>");
    if (0) { c1 = c2; c2 = c1; }
    c1 as Wrapper<Pair<int,int>>; c2 as ComplexWrapPair<int>;
    return (c1, c2);
}

fun transform<T, U>(x: T, f: (T) -> U): U {
    return f(x);
}

@method_id(117)
fun test17() {
    var id = makeIdentity<MyInt>();
    var result = transform(100 as MyInt, id);
    __expect_type(result, "MyInt");
    var r2 = transform(wrap(123), wrap<Wrapper<int>>);
    __expect_type(r2, "Wrapper<Wrapper<int>>");
    return (result, r2);
}

struct UnionHolder<T> {
    item: Wrapper<T>;
    extra: Wrapper<T | bool>;
}

@method_id(118)
fun test18() {
    var uh: UnionHolder<int | slice> = { item: wrap(123 as int|slice), extra: wrap("" as int|slice|bool) };
    __expect_type(uh, "UnionHolder<int | slice>");
    __expect_type(uh.item, "Wrapper<int | slice>");
    __expect_type(uh.extra, "Wrapper<int | slice | bool>");

    var ub = UnionHolder<bool|int> { item: wrap(123 as int|bool), extra: wrap(true as int|bool) };
    __expect_type(ub.extra, "Wrapper<bool | int>");

    return (uh.item, 777, ub);
}

type StrangeInt<T> = int;
type StrangeAlsoInt = StrangeInt<()>;

@method_id(119)
fun test19() {
    var i1: StrangeInt<builder> = 20;
    var i2: StrangeInt<slice> = 30;
    var i3: StrangeInt<StrangeInt<int>> = 40 as StrangeAlsoInt;
    i1 = i2; i2 = i1; i1 = i3; i3 = i1;
    return (i1 as StrangeInt<()>, i3 as StrangeAlsoInt, i1 is StrangeInt, i1 is StrangeInt<cell>, i3 is StrangeInt, i3 is StrangeAlsoInt);
}

@method_id(120)
fun test20() {
    var w1: Wrapper<int8> | int = { value: 10 };
    var w2: int | Wrapper<uint32> = { value: 30 };
    var w3: slice | Wrapper<(int8, int16)> = { value: (50, 30) };
    __expect_type(w3.value, "(int8, int16)");
    var w4 = { value: null } as int | Wrapper<int?>;
    var w5 = { value: null } as int | Wrapper<DualStorage<slice,builder>?>;
    var w6: int | Wrapper<int> | null | slice | builder | (int, int) = { value: 10 };
    if (10 > 3) {
        w6 = beginCell();
    }
    __expect_type(w6, "builder | Wrapper<int>");
    return (w4, 777, w5, w5 is Wrapper, w6 is builder);
}

struct AnotherWrapper<int> {
    value: int;
}

fun constructAnotherWrapper<slice>(value: slice): AnotherWrapper<slice> {
    return AnotherWrapper<slice> { value };
}

fun test21(w: AnotherWrapper<builder>) {
    var w2: AnotherWrapper<builder> = { value: beginCell() };
    var w3 = constructAnotherWrapper(beginCell());
    __expect_type(w2, "AnotherWrapper<builder>");
    __expect_type(w3, "AnotherWrapper<builder>");
    __expect_type(constructAnotherWrapper<Wrapper<int>>, "(Wrapper<int>) -> AnotherWrapper<Wrapper<int>>");
    w.value = w2.value;
    w3.value = beginCell();
    w = w2;
}

fun test22(w: int | WrapperAlias<int> | slice) {
    match (w) {
        int => {}
        WrapperAlias<MyInt> => {}
        slice => {}
    }
    __expect_type(w, "int | Wrapper<MyInt> | slice");
    match (w) {
        MyInt => {}
        slice => {}
        WrapperAlias => {}
    }
};

@method_id(123)
fun test23() {
    var w1: Wrapper<int> = { value: 10 };
    var w2: Wrapper<Wrapper<int>> = { value: w1 };
    var w3: Wrapper<Wrapper<Wrapper<int>>> = { value: w2 };
    var w4: Wrapper<Wrapper<Wrapper<Wrapper<int>>>> = { value: w3 };
    var w5: Wrapper<Wrapper<Wrapper<Wrapper<Wrapper<int>>>>> = { value: w4 };
    eq<Wrapper<int>>(w1);
    eq<Wrapper<Wrapper<int>>>(w2);
    eq<Wrapper<Wrapper<Wrapper<int>>>>(w3);
    return (w1, w2, w3, w4, w5);
}

fun takeSomethingAndWrapper<T>(something: T, w: Wrapper<T>) {
    w.value = something;
}

struct HasSomethingAndWrapper<T> {
    something: T;
    w: Wrapper<T>;
}

fun test24() {
    // check that parameter w can be understood from context (after deducing parameter something)
    takeSomethingAndWrapper(0, {value: 0});
    takeSomethingAndWrapper(null as slice?, {value: null});
    takeSomethingAndWrapper(Wrapper { value: 10 }, {value: { value: 20 }});
    // same for creating an object and deducing Ts one by one
    HasSomethingAndWrapper { something: 0, w: { value: 0 } };
    val o = HasSomethingAndWrapper { something: 5 as uint8, w: { value: 5 } };
    __expect_type(o, "HasSomethingAndWrapper<uint8>");
}

fun test25() {
    var p1 = Pair { first: null, second: null };
    __expect_type(p1, "Pair<null, null>");

    try {}
    catch(e, second) {
        var p2 = Pair { first: null, second };
        __expect_type(p2, "Pair<null, unknown>");
    }
}


fun main(c: Wrapper<slice>, d: WrappedInt) {
    __expect_type(c, "Wrapper<slice>");
    __expect_type(c!, "Wrapper<slice>");
    __expect_type(d, "WrappedInt");
    __expect_type(d.value, "int");
    __expect_type(WrappedInt { value: 200 }, "Wrapper<int>");
    __expect_type(Wrapper<slice>{ value: c.value }, "Wrapper<slice>");
    __expect_type(Wrapper<bytes32>{ value: c.value as bytes32 }, "Wrapper<bytes32>");
    __expect_type(Wrapper<bytes32>{ value: Wrapper<bytes32> { value: (c.value as bytes32?)! }.value }, "Wrapper<bytes32>");
    __expect_type(Wrapper{ value: c.value }, "Wrapper<slice>");
    __expect_type(Wrapper{ value: c.value.loadInt(32) }, "Wrapper<int>");
    __expect_type(WrapperAlias{ value: 10 }, "Wrapper<int>");
    __expect_type(WrapperAlias{ value: 10 } as WrapperAlias<int>, "WrapperAlias<int>");
    __expect_type(swap(Pair { first: 3, second: beginCell() }), "Pair<builder, int>");
}

/**
@testcase | 101 |         | 23 23
@testcase | 102 | 9       | 9 10 19 9
@testcase | 102 | 19      | 19 20 39 (null)
@testcase | 103 |         | 10 10 (null) (null) 0 777 10 10 (null) (null) 0
@testcase | 104 |         | 11 11 13 13 13 2
@testcase | 105 | 0       | 80
@testcase | 105 | -1      | (null)
@testcase | 106 |         | (null) (null) 0 777 1 2 142
@testcase | 107 | 5       | 20 20 5 5 20 777 (null) (null) (null) 0
@testcase | 108 | 0       | 777 143 777 143
@testcase | 108 | -1      | 777 0 777 0
@testcase | 109 |         | 40 40 70
@testcase | 110 |         | 20 1 20 42
@testcase | 111 |         | 5 1 132 5 1 132 5 1 132 5 1 132 
@testcase | 112 | -1      | 10 1 777 10 1 133
@testcase | 112 | 0       | 20 1 777 (null) 0 134
@testcase | 113 |         | 30 -1
@testcase | 114 |         | 999 (null) 2
@testcase | 115 |         | 10 0 200 1 -1 200 1 -1
@testcase | 116 |         | 1 2 3 4
@testcase | 117 |         | 100 123
@testcase | 118 |         | 123 1 777 123 1 -1 2
@testcase | 119 |         | 40 40 -1 -1 -1 -1
@testcase | 120 |         | (null) 137 777 (null) (null) 0 139 -1 -1
@testcase | 123 |         | 10 10 10 10 10


@fif_codegen
"""
  test1 PROC:<{
    23 PUSHINT   // c1=23
    DUP          // c1=23 c2=23
  }>
"""

@fif_codegen DECLPROC getWrappervalue1<Wrapper<(int,int)>>
@fif_codegen DECLPROC getWrappervalue2<Wrapper<Wrapper<(int,int)>>>
 */
