import "@stdlib/tvm-dicts.tolk"

type MIntN = MInt?
type MInt = int
type MInt_v2 = int
type MVoid = void
type Pair2_v1 = | (int, int)
type Pair2_v2 = (MInt, MInt)
type MBool = | bool
type Tuple2Int = [int, int]
type UserId = int
type OwnerId = int

struct Point { x: int; y: int }
type PointAlias = Point;
type PointAlias2 = PointAlias;

fun rand(): uint256
    asm "RANDU256";

fun test1(x: MInt): MVoid {
    var y = x;
    var z: MInt = 2;
    __expect_type(x, "MInt");
    __expect_type(y, "MInt");
    __expect_type(z, "MInt");

    __expect_type(x + y, "int");
    __expect_type((x + y) as MInt, "MInt");
    __expect_type(x!, "MInt");
    __expect_type(~x, "int");
    __expect_type(x as int, "int");
    __expect_type(x as int8, "int8");
    __expect_type(rand() ? x : y, "MInt");
    __expect_type((x, 1, y), "(MInt, int, MInt)");

    __expect_type(rand() ? (1, 2) : (1, 2) as Pair2_v1, "(int, int)");
    __expect_type(rand() ? (1, 2) : (1, 2) as Pair2_v2, "(int, int)");
    __expect_type(rand() ? (1, 2) as Pair2_v1 : (1, 2), "Pair2_v1");
    __expect_type(rand() ? (1, 2) as Pair2_v2 : (1, 2) as Pair2_v2, "Pair2_v2");

    // since `Pair2_v1` and `Pair2_v2` underlying type is equal, a union of them can't be created
    var d1 = (1, 2) as Pair2_v1 | Pair2_v2;
    var d2 = (1, 2) as Pair2_v2 | Pair2_v1;
    __expect_type(d1, "Pair2_v1");
    __expect_type(d2, "Pair2_v2");
    __expect_type(rand() ? (1, 2) as Pair2_v1 : (1, 2) as Pair2_v2, "Pair2_v1");

    __expect_type(!x, "bool");

    __expect_type(x as int?, "int?");
    __expect_type(x as MInt?, "MInt?");
    __expect_type(x as MIntN, "MIntN");

    __expect_type(PointAlias{x:0,y:0}, "Point");
    __expect_type(PointAlias2{x:0,y:0}, "Point");
    __expect_type(Point{x:0,y:0} as PointAlias, "PointAlias");

    if (x) { return; }
}

fun test2() {
    __expect_type(test1, "(MInt) -> void");
    __expect_type(test1(1), "void");
}

fun test3(x: MIntN, y: MInt?) {
    __expect_type(x, "MIntN");
    __expect_type(x!, "MInt");
    __expect_type(y!, "MInt");
    if (x != null) {
        __expect_type(x, "MInt");
    }
    __expect_type(x, "MIntN");
    var (z1, z2) = (x, x!, );
    __expect_type(z1, "MIntN");
    __expect_type(z2, "MInt");
}

@method_id(104)
fun test4(x: MIntN, y: MIntN) {
    if (x != null && y != null) {
        __expect_type(x, "MInt");
        __expect_type(x + y, "int");
        return x + y;
    }
    __expect_type(x, "MIntN");
    __expect_type(x!, "MInt");
    __expect_type(rand() ? x : y, "MIntN");
    __expect_type(rand() ? x : 0, "MInt?");
    __expect_type(rand() ? 0 : x, "int?");
    __expect_type(rand() ? 0 : x!, "int");
    return y!;
}

fun takeTensor_v1(v: Pair2_v1) { return v.0 + v.1; }
fun takeTensor_v2(v: Pair2_v2) { return v.0 + v.1; }
fun takeTensor_v3(v: (int, int)) { return v.0 + v.1; }

fun test5() {
    var x: Pair2_v1 = (1, 2);
    var y = (3, 4) as Pair2_v2;
    var z = (5, 6);

    __expect_type(x, "Pair2_v1");
    __expect_type(y, "Pair2_v2");

    takeTensor_v1(x); takeTensor_v2(x as Pair2_v2); takeTensor_v3(x);
    takeTensor_v1(y as Pair2_v1); takeTensor_v2(y); takeTensor_v3(y);
    takeTensor_v1(z); takeTensor_v2(z); takeTensor_v3(z);

    var t = (y, x);
    return t.0.1 + t.1.0;
}

fun test6() {
    var (x1: MInt?, x2: MIntN) = (5, 5);    // smart cast
    __expect_type(x1, "MInt");
    __expect_type(x2, "MInt");
    var (y1: MInt?, y2: MIntN) = (null, null);
    __expect_type(y1, "null");
    __expect_type(y2, "null");
    var z: Pair2_v2? = (5, 5);
    __expect_type(z, "Pair2_v2");
    __expect_type(z.0, "MInt");
}

fun someFn1(v: MInt): MIntN { return v as MIntN; }

fun test7() {
    var f1: (int) -> int? = someFn1;
    var f2: (int) -> MInt? = someFn1;
    var f3: (MInt_v2) -> MInt_v2? = someFn1 as (MInt_v2) -> MInt_v2?;

    f1 = f2; f1 = f3;
    f2 = f1; f2 = f3 as (int) -> MInt?;
    f3 = f1; f3 = f2 as (MInt_v2) -> MInt_v2?;
}

fun test8() {
    0 as MInt;
    0 as MInt?;
    0 as MIntN;

    (1, 2) as Pair2_v2;
    (1, 2) as Pair2_v2?;
    (((1, 2) as Pair2_v2?) as (int, int)?) as Pair2_v1?;

    someFn1 as (int) -> int?;
    someFn1 as (int) -> MInt?;
    someFn1 as (MInt_v2) -> MInt_v2?;
}

@method_id(109)
fun test9(b: MBool): MBool {
    if (!b) {
        __expect_type(b, "MBool");
        return !b;
    }
    return !!(5 as MInt);
}

@method_id(110)
fun test10() {
    var x1: Pair2_v1 = (5, 6);
    var (a1, b1) = x1;
    __expect_type(a1, "int");
    var x2: Pair2_v2? = x1 as (int, int);
    var (a2, b2) = x2;
    __expect_type(a2, "MInt");
    var x3: Tuple2Int = [9, 10];
    var [a3, b3, ] = x3;
    return a1 + a2 + a3 + b1 + b2 + b3;
}

fun analyzeTensor1<T1, T2>(a: (T1, T2)): (T1, T2) { return a; }
fun analyzeTensor2<T1, T2>(a: (T1, T2)?): (T1, T2)? { return a; }

fun test11() {
    var (x1: (int8, int16), x2: Pair2_v1, x3: Pair2_v2) = ((1,2), (3,4), (5,6));
    __expect_type(analyzeTensor1(x1), "(int8, int16)");
    __expect_type(analyzeTensor1(x2), "(int, int)");
    __expect_type(analyzeTensor1(x3), "(MInt, MInt)");
    __expect_type(analyzeTensor2(x1), "(int8, int16)?");
    __expect_type(analyzeTensor2(x2), "(int, int)?");
    __expect_type(analyzeTensor2(x3), "(MInt, MInt)?");
}

fun dict.getFakeDepth(self) { return 1; }
fun cell.getFakeDepth(self) { return 2; }

@method_id(112)
fun test12(makeNotNull: bool) {
    var d = createEmptyDict();
    if (makeNotNull) {
        d.iDictSet(32, 123, "");
    }
    var t = createEmptyTuple();
    if (d != null) {
        __expect_type(d.getFakeDepth, "(cell) -> int");
        t.push(d.getFakeDepth());
    } else {
        __expect_type(d.getFakeDepth, "(dict) -> int");
        t.push(d.getFakeDepth());
    }
    t.push(d.getFakeDepth());
    return t;
}

struct Wrapper<T> {
    value: T;
}
type WrapperOfInt = Wrapper<int>
type WrapperOfMInt = Wrapper<MInt>
type WrapperAlias<T> = Wrapper<T>
type WrapperAliasOfInt = WrapperAlias<int>
type WrapperAliasOfMInt = WrapperAlias<MInt>

fun WrapperOfInt.myFn(self): int1 { return 0 }
fun WrapperOfMInt.myFn(self): int2 { return 0 }
fun WrapperAliasOfInt.myFn(self): int3 { return 0 }
fun WrapperAliasOfMInt.myFn(self): int4 { return 0 }

fun test13(a: WrapperAlias<int>, b: WrapperAlias<MInt>, c: Wrapper<int>, d: Wrapper<MInt>,
    e: WrapperAliasOfInt, f: WrapperAliasOfMInt, g: WrapperOfInt, h: WrapperOfMInt) {
    __expect_type(g.myFn(), "int1");
    __expect_type(h.myFn(), "int2");
    __expect_type(e.myFn(), "int3");
    __expect_type(f.myFn(), "int4");

    a = a; a = b; a = c; a = d; a = e;
    b = a; b = b; b = c; b = d;        b = f;
    c = a; c = b; c = c; c = d; c = e; c = f; c = g; c = h;
    d = a; d = b; d = c; d = d; d = e; d = f; d = g; d = h;
    e = a;        e = c; e = d; e = e;
           f = b; f = c; f = d;        f = f;
                  g = c; g = d;               g = g;
                  h = c; h = d;                      h = h;

    a as WrapperAlias<int>;
    a as WrapperAlias<MInt>;
    a as Wrapper<int>;
    a as Wrapper<MInt>;
    a as WrapperOfInt;
    a as WrapperOfMInt;
    a as WrapperAliasOfInt;
    a as WrapperAliasOfMInt;

    match (a) { WrapperAlias<int> => {} }
    match (a) { WrapperAlias<MInt> => {} }
    match (a) { Wrapper<int> => {} }
    match (a) { Wrapper<MInt> => {} }
    // match (a) { WrapperOfInt => {} }
    // match (a) { WrapperOfMInt => {} }
    match (a) { WrapperAliasOfInt => {} }
    // match (a) { WrapperAliasOfMInt => {} }

    b as WrapperAlias<int>;
    b as WrapperAlias<MInt>;
    b as Wrapper<int>;
    b as Wrapper<MInt>;
    b as WrapperOfInt;
    b as WrapperOfMInt;
    b as WrapperAliasOfInt;
    b as WrapperAliasOfMInt;

    match (b) { WrapperAlias<int> => {} }
    match (b) { WrapperAlias<MInt> => {} }
    match (b) { Wrapper<int> => {} }
    match (b) { Wrapper<MInt> => {} }
    // match (b) { WrapperOfInt => {} }
    // match (b) { WrapperOfMInt => {} }
    // match (b) { WrapperAliasOfInt => {} }
    match (b) { WrapperAliasOfMInt => {} }

    match (e) { WrapperAliasOfInt => {} }

    match (f) { WrapperAliasOfMInt => {} }
    match (f) { Wrapper<int> => {} }
    match (f) { Wrapper<MInt> => {} }
    // match (f) { WrapperAlias<int> => {} }
    match (f) { WrapperAlias<MInt> => {} }

    __expect_type(a, "WrapperAlias<int>");
    __expect_type(b, "WrapperAlias<MInt>");
    __expect_type(c, "Wrapper<int>");
    __expect_type(d, "Wrapper<MInt>");
    __expect_type(e, "WrapperAliasOfInt");
    __expect_type(f, "WrapperAliasOfMInt");
    __expect_type(g, "WrapperOfInt");
    __expect_type(h, "WrapperOfMInt");

    __expect_type(g.myFn(), "int1");
    __expect_type(h.myFn(), "int2");
    __expect_type(e.myFn(), "int3");
    __expect_type(f.myFn(), "int4");
}

type Payload14 = MInt | MBool | PointAlias2

fun test14(i: MInt, b: MBool, c: PointAlias2) {
    var p: Payload14 = i;
    p = b;
    p = c;
    p = i as int;
    p = b as bool;
    p = c as Point;
    p = c as PointAlias;
}

type MInt1 = int
type MInt2 = MInt1

fun int.plus0(self) { return self + 0 }
fun MInt1.plus1(self) { return self + 1 }
fun MInt2.plus2(self) { return self + 2 }

@method_id(115)
fun test15() {
    var i: int = 0;
    var m: MInt1 = 10;
    var t: MInt2 = 20;

    if (0) {
        i = m; i = t;
        m = i; m = t;
        t = i; t = m;
    }
    
    return (
        i.plus0(), i.plus1(), i.plus2(),
        m.plus0(), m.plus1(), m.plus2(),
        t.plus0(), t.plus1(), t.plus2(),
    )
}

type StrangeUnionInt = int | UserId
type StrangeUnionInt2 = UserId | OwnerId

struct ManyUnionFields {
    f1: int | UserId
    f2: StrangeUnionInt
    f3: UserId | OwnerId
    f4: OwnerId | UserId
    f5: int | StrangeUnionInt2
    f6: StrangeUnionInt2

    f10: slice | int | UserId
    f11: slice | UserId | int
    f12: OwnerId | () | int | UserId | slice
    f13: OwnerId | cell | StrangeUnionInt2?
}

fun test16(a: ManyUnionFields) {
    __expect_type(a.f1, "int");
    __expect_type(a.f2, "StrangeUnionInt");
    __expect_type(a.f3, "UserId");
    __expect_type(a.f5, "int");
    __expect_type(a.f6, "StrangeUnionInt2");
    a.f1 + a.f2 + a.f3 + a.f4 + a.f5 + a.f6;

    __expect_type(a.f10, "slice | int");
    __expect_type(a.f11, "slice | UserId");
    __expect_type(a.f12, "OwnerId | () | slice");
    __expect_type(a.f13, "OwnerId | cell | null");

    match (a.f13) {
        int => {}
        cell => {}
        null => {}
    }
}

type SnakedCell<T> = cell

fun test17(a: SnakedCell<int>, b: SnakedCell<MInt>, c: SnakedCell<MInt_v2>) {
    a = a; a = b; a = c;
    b = a;
    c = a;

    a as SnakedCell<int>;
    a as SnakedCell<MInt>;
    a as SnakedCell<MInt_v2>;
    b as SnakedCell<int>;
    b as SnakedCell<MInt>;
    b as SnakedCell<MInt_v2>;
    c as SnakedCell<int>;
    c as SnakedCell<MInt>;
    c as SnakedCell<MInt_v2>;
}


fun main(x: MInt, y: MInt?) {
    return y == null ? x : x + y;
}

/**
@testcase | 0   | 3 4 | 7
@testcase | 104 | 1 2 | 3
@testcase | 110 |     | 41
@testcase | 112 | 0   | [ 1 1 ]
@testcase | 112 | -1  | [ 2 1 ]
@testcase | 115 |     | 0 1 2 10 11 12 20 21 22

@fif_codegen
"""
  test9() PROC:<{ //  b
    DUP	        //  b b
    IFNOTJMP:<{	//  b
      NOT	//  '2
    }>	    //  b
    DROP	//
    TRUE	//  '5
  }>
"""
 */
