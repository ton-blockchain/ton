struct JustInt {
    value: int;
}

struct JustIntWrapper {
    int: JustInt;
}

type MInt = int;
type JustIntAlias = JustInt;
type JustIntAlias2 = JustIntAlias;

struct Storage {
    owner: User,
    lastPoint: Point,
}

struct Point {
    x: int;
    y: MInt
}

struct User {
    id: int;
    name: slice;
}

struct OuterStorage {
    st: Storage,
    stMaybe: Storage?
}

struct WithTensorInside {
    coords: (int, int);
    tup: [int, int];
    otherCoords: (int, int)?;
    otherTup: tuple;
}

fun sumCoords(p: Point) {
    return p.x + p.y;
}

fun Point.sumCoords(self) {
    return self.x + self.y;
}

fun getStorage1(x: int): Storage {
    val owner = User { id: 1, name: "u" };
    return {
        owner,
        lastPoint: { x, y: 10, },
    };
}

fun generatePoint(x: int, y: int): Point {
    return { x, y };
}

fun assignCoords(mutate p: Point) {
    p.x = 10;
    p.y = 20;
}

fun Point.assignCoords(mutate self) {
    self.x = 10;
    self.y = 20;
}

@method_id(101)
fun test1() {
    var i1 = JustInt { value: 1 };
    var i2: JustInt = JustInt { value: 2, };
    var i3: JustInt = { `value`: 3 };
    __expect_type(i3, "JustInt");
    __expect_type(i3!, "JustInt");
    return (i1, i2, i3, [i1, JustInt{value:5}]);
}

@method_id(102)
fun test2() {
    var i1 = JustIntAlias2 { value: 1 };
    var i2: JustInt = JustIntAlias2 { value: 2, };
    var i3: JustIntAlias2 = { `value`: 3 };
    __expect_type(i1.value, "int");
    __expect_type(i1.value + i2.value + i3.value, "int");
    __expect_type(i3, "JustIntAlias2");
    __expect_type([i2, i3], "[JustInt, JustIntAlias2]");
    __expect_type([i2] as [JustIntAlias], "[JustIntAlias]");
    return (i1, i2, i3, [i1, JustIntAlias{value:5}]);
}

@method_id(103)
fun test3() {
    return generatePoint(5, 6);
}

@method_id(104)
fun test4() {
    var p: Point = { x: 10, y: 20 };
    return (p == null, p, p = {x:30,y:40}, p != null);
}

@method_id(105)
fun test5() {
    var b = PointAlias { y: 20, x: 10 };
    var p: PointAlias = Point { x: b.x, y: b.y };
    p.x += 5;
    return (sumCoords(p), p.y += 10, p.sumCoords());
}

struct BacktickNames {
    `my()id`: MInt,
    `100500`: int;
}

@method_id(106)
fun test6() {
    val b: BacktickNames = { `100500`: 20, `my()id`: 10 };
    var p: Point = { x: 0, y: 0 };
    p.x += b.`my()id`;
    p.y += b.100500;
    return p;
}

@method_id(107)
fun test7() {
    var s = getStorage1(5);
    s.owner.name = beginCell().storeInt(s.lastPoint.x, 32).endCell().beginParse();
    var s2 = s;
    assignCoords(mutate s.lastPoint);
    return (s.owner.name.loadInt(32), s2.owner.name.loadInt(32), s.lastPoint, s2.lastPoint.sumCoords());
}

struct Empty{}
struct OuterEmpty { nothing: EmptyAlias }
type EmptyAlias = Empty;

@method_id(108)
fun test8() {
    var e1: Empty = {};
    var o1: OuterEmpty = { nothing: e1 };
    var o2 = OuterEmpty { nothing: {} };
    __expect_type(o2.nothing, "EmptyAlias");
    var o3 = Empty{} as Empty?;
    var o4 = null as Empty?;
    var o5: EmptyAlias? = o3!;
    var o6: OuterEmpty? = { nothing: {} };
    var o7 = o3! as EmptyAlias?;
    var o8 = o6 as OuterEmpty?;
    __expect_type(o3, "Empty?");
    __expect_type(o6, "OuterEmpty");
    __expect_type(o8, "OuterEmpty?");
    return (e1, Empty{}, EmptyAlias{}, o2, o1, 777, o3, 777, o4, 777, o5, 777, o6, 777, o7, 777, o8, 777, o3!, 777);
}

fun maxCoord(p: Point): int8 {
    return p.x > p.y ? p.x : p.y;
}

fun Point.maxCoord(self): int8 {
    return self.x > self.y ? self.x : self.y;
}

@method_id(109)
fun test9() {
    var p = Point { x: 80, y: 100 };
    p.assignCoords();
    return (maxCoord({y: 70, x: 30}), maxCoord(generatePoint(30, 20)), maxCoord(p), p.maxCoord(), Point{x:-80,y:-80}.maxCoord());
}

@method_id(110)
fun test10(notNull: bool) {
    var p: Point? = null;
    __expect_type(p, "null");
    if (notNull) {
        p = { x: 1, y: 2 };
        __expect_type(p, "Point");
        __expect_type(p.x, "int");
        __expect_type(p.y, "MInt");
        __expect_type(p.x + p.y, "int");
        assignCoords(mutate p);
    }
    __expect_type(p, "Point?");
    return p;
}

@method_id(111)
fun test11(notNull: bool) {
    var os: OuterStorage = {
        st: { owner: { id: 0, name: "" }, lastPoint: { x: 0, y: 0 } },
        stMaybe: notNull ? { owner: { id: 1, name: "" }, lastPoint: { x: 1, y: 1 } } : null,
    };
    if (os.stMaybe == null) {
        os!.st!.lastPoint!.y = 2;
        os.stMaybe = { owner: { id: 2, name: "" }, lastPoint: { x: 2, y: 3 } };
    } else {
        os.stMaybe.owner.id += 1;
        os.stMaybe.lastPoint = { x: 3, y: 4 };
    }
    if (!notNull) {
        assignCoords(mutate os.stMaybe.lastPoint);
        os.stMaybe.lastPoint.assignCoords();
    }
    return (os.st.lastPoint, os.stMaybe.lastPoint, os.stMaybe.lastPoint.sumCoords());
}

@method_id(112)
fun test12(notNull: bool): Point? {
    return notNull ? { x: 1, y : 2 } : null;
}

@method_id(113)
fun test13() {
    return null as User?;
}

@method_id(114)
fun test14() {
    var p: Point? = { y: 2, x: 1 };
    __expect_type(p, "Point");
    __expect_type(p as Point, "Point");
    __expect_type(p as Point?, "Point?");
    return p;
}

@method_id(115)
fun test15() {
    var os = OuterStorage {
        st: { owner: { id: 0, name: "" }, lastPoint: { x: 0, y: 0 } },
        stMaybe: null,
    };
    __expect_type(os.stMaybe, "Storage?");
    return os.stMaybe;
}

fun pushZero(mutate t: tuple) {
    t.push(0);
}

@method_id(116)
fun test16(): WithTensorInside? {
    var wt: WithTensorInside = {
        coords: (1, 2),
        tup: [3, 4],
        otherCoords: null,
        otherTup: createEmptyTuple(),
    };
    wt.coords = (-1, -2);
    wt.coords.1 -= 2;
    wt.tup.0 += 5;
    wt.otherCoords!.0 = 7;
    wt.otherTup.push(0);
    pushZero(mutate wt.otherTup);
    (wt.otherTup.1, wt.otherTup.0) = (11, 10);
    return wt;
}

@method_id(117)
fun test17(x: JustInt?) {
    var w1: JustIntWrapper = { int: { value: x == null ? -1 : x.value } };
    var w2: JustIntWrapper? = x == null ? x : { int: { value: x.value } };
    return (x, x!, x!.value, w1, w2);
}

fun sumXY<P>(point: P) { return point.x + point.y; }

@method_id(118)
fun test18() {
    var p = `Point` { x: 8, `y`: 9 } as `PointAlias`?;
    if (p != null) {
        return sumXY(p);
    }
    return sumXY(p = getStorage1(80).lastPoint);
}

global gPoint: Point;
global gPointN: Point?;

@method_id(119)
fun test19() {
    gPoint = { x: 1, y: 2 };
    assignCoords(mutate gPoint);
    gPointN = getStorage1(9).lastPoint;
    return (gPoint, gPointN, gPointN!.x);
}

@method_id(120)
fun test20(setNull: bool) {
    gPoint = generatePoint(8, 9);
    gPointN = setNull ? null : gPoint;
    if (!setNull) {
        gPoint.x = gPointN!.y = 80;
    } else {
        (gPoint as Point?)!.y += 5;
    }
    return (gPoint, gPointN);
}

global gJustInt: JustInt;
global gJustIntN: JustInt?;

@method_id(121)
fun test21(setNull: bool) {
    var value = 8;
    gJustInt = { value };
    gJustIntN = setNull ? null : { value: value += 9 };
    (gJustInt.value, value) = (value, gJustInt.value);
    return (gJustInt, gJustIntN, value, value * (gJustIntN == null ? 1 : gJustIntN!.value));
}

fun asm_func(x1: JustInt, x2: int, x3: JustIntAlias, x4: JustIntAlias, x5: int, x6: JustInt, x7: int):
    (int, JustInt, int, JustInt, int, int, JustIntAlias2)
    asm (x4 x5 x6 x7 x1 x2 x3->0 1 2 3 4 5 6) "NOP";

fun JustInt.someOp(mutate self, y: int): int {
    val (newX, newY) = (self.value + y, y * 10);
    self = { value: newX };
    return newY;
}

@method_id(122)
fun test22(x: JustInt) {
    return asm_func(x, x.value += 1, x, x, x.someOp(x.value / 20), x, x.value = x.value * 2);
}

@method_id(123)
fun test23(p: Point) {
    var complex = p as Point | (JustInt | Storage);
    return match (complex) {
        JustInt => 10 * complex.value,
        PointAlias => 20,
        Storage => 30 + complex.lastPoint.x,
    };
}

@method_id(124)
fun test24(x: JustInt | JustIntAlias2 | JustIntAlias): JustInt {
    return match (x) {
        JustIntAlias => x
    };
}

fun getPointOrStorage(getPoint: bool): Point | Storage {
    return getPoint
           ? Point { x: 10, y: 20 }
           : Storage { owner: { id: 10, name: "" }, lastPoint: { x: 30, y: 40 } };
}

@method_id(125)
fun test25() {
    var r1 = getPointOrStorage(true);
    if (r1 !is Storage && r1 !is builder && r1 != null) {
        match (val d = getPointOrStorage(false)) {
            Point => {
                r1.x += d.x;
                r1.y += d.y;
            }
            Storage => {
                r1.x += d.lastPoint.x;
                r1.y += d.lastPoint.y;
            }
        }
    }
    return r1;
}

fun acceptSomeUnion(r: Point | int | builder) {
    return (r is Point) ? r.x + r.y : (r is int) ? r : null;
}

@method_id(126)
fun test26() {
    __expect_type(acceptSomeUnion, "(Point | int | builder) -> int?");
    return acceptSomeUnion({ x: 8, y: 10 })! + acceptSomeUnion(100)!;
}

struct Has2EmptyNullable {
    f1: Empty?;
    f2: Empty?
}

fun get2EmptyNullable(null1: bool, null2: bool): Has2EmptyNullable {
    return { f1: null1 ? null : {}, f2: null2 ? null : {} };
}

@method_id(127)
fun test27() {
    var (t1, t2) = (get2EmptyNullable(true, true), get2EmptyNullable(false, false));
    var (t3, t4) = (t1 as Has2EmptyNullable?, null as Has2EmptyNullable?);
    return (t1, t2, 777, t1.f1 == null, t1.f2 == null, t2.f1 == null, t2.f2 == null, 777, t3, 777, t3!, 777, t4);
}

fun getEmptyOrIntInt(getEmpty: bool): Empty | (int, int) {
    return getEmpty ? {} : (1, 2);
}

@method_id(128)
fun test28(getEmpty: bool) {
    var t1: Empty | (int, int) = getEmptyOrIntInt(getEmpty);
    var t2 = (getEmpty ? Empty{} : Has2EmptyNullable{ f1: null, f2: {} }) as EmptyAlias | Has2EmptyNullable | null;
    return (t1, t1 is Empty, t1 is (int, int), t1 is builder, 777, t2, t2 is Empty, t2 is Has2EmptyNullable, t2 == null);
}

struct Empty1 {}
struct Empty2 {}

@method_id(129)
fun test29() {
    var c = (null as Empty1 | Empty2?);
    if (c == null) {
        return c;
    }
    return null;
}

@method_id(130)
fun test30(is1: bool) {
    var e: Empty1 | Empty2 = is1 ? Empty1{} : Empty2 {};
    var n: Empty1 | int | Empty2 = 4;
    var c: Empty1 | Empty2? = null;
    if (is1) {
        n = e;
    } else {
        c = e;
    }
    __expect_type(test29(), "null");
    return (e, 777, n, 777, c, 777, (test29() as Empty1? | Empty2), (test29() as Empty1 | Empty2 | null)!);
}

struct IntOrNull {
    e: Empty;
    x: int?;
}

@method_id(131)
fun test31() {
    var s1 = { x: 5, e: {} } as IntOrNull?;
    var s2 = { e: {}, x: null } as IntOrNull?;
    var s3 = null as IntOrNull?;
    return (s1, s2, s3, 777, s1 == null, s2 == null, s3 == null, 777, s1!.x == null, s2!.x == null);
}

struct PointDef0 {
    x: int = 0,
    y: int = 0,
}

@method_id(132)
fun test32() {
    var p1: PointDef0 = { x: 10, y: 20 };
    var p2: PointDef0 = { x: 10 };
    var p3: PointDef0 = { y: 20 };
    var p4: PointDef0 = { };
    return (p1, p2, p3, p4, PointDef0{});
}

struct WithDefaults {
    f1: (bool, int) = (true, 0),
    f2: int,
    f3: slice? = stringHexToSlice("010203"),
    f4: PointDef0? = null,
    f5: int32 | int64 = 0 as int32,
}

@method_id(133)
fun test33(): WithDefaults {
    var w1: WithDefaults = { f2: 0 };
    assert(w1.f1.0 && w1.f1.1 == 0 && w1.f3!.remainingBitsCount() == 24 && w1.f4 == null && w1.f5 is int32) throw 100;
    var w2: WithDefaults? = { f1: (false, 55), f2: 10, f5: 8 as int64 };
    assert(w2.f1.0 != true && w2.f3!.remainingBitsCount() == 24 && w2.f4 == null && w2.f5 is int64 && w2.f5 == 8) throw 100;
    var w3: (int, WithDefaults) = (0, { f2: 7, f4: {y: 20} });
    assert(w3.1.f4 != null && w3.1.f4.x == 0 && w3.1.f4.y == 20) throw 100;
    return { f2: 5, f3: null };
}

struct WithNever {
    f1: int;
    f2: never;
    f3: int;
}

@method_id(134)
fun test34() {
    var o1: WithNever = { f1: 10, f3: 20 };   // f2 is `never`, it can be omitted
    __expect_type(o1.f2, "never");
    return o1;
}


fun main(x: int8, y: MInt) {
    __expect_type(PointAlias{x,y}, "Point");
    __expect_type(Point{x,y} as Point, "Point");
    __expect_type(test3(), "Point");
    __expect_type(maxCoord, "(Point) -> int8");
}

type PointAlias = Point;

/**
@testcase | 101 |      | 1 2 3 [ 1 5 ]
@testcase | 102 |      | 1 2 3 [ 1 5 ]
@testcase | 103 |      | 5 6
@testcase | 104 |      | 0 10 20 30 40 -1
@testcase | 105 |      | 35 30 45
@testcase | 106 |      | 10 20
@testcase | 107 |      | 5 5 10 20 15
@testcase | 108 |      | 777 131 777 0 777 777 777 131 777 132 777 777
@testcase | 109 |      | 70 30 20 20 -80
@testcase | 110 | 0    | (null) (null) 0
@testcase | 110 | -1   | 10 20 133
@testcase | 111 | 0    | 0 2 10 20 30
@testcase | 111 | -1   | 0 0 3 4 7
@testcase | 112 | 0    | (null) (null) 0
@testcase | 112 | -1   | 1 2 133
@testcase | 113 |      | (null) (null) 0
@testcase | 114 |      | 1 2
@testcase | 115 |      | (null) (null) (null) (null) 0
@testcase | 116 |      | -1 -4 [ 8 4 ] 7 (null) 0 [ 10 11 ] 135
@testcase | 117 | 5    | 5 5 5 5 5
@testcase | 117 | null | (null) (null) (null) -1 (null)
@testcase | 118 |      | 17
@testcase | 119 |      | 10 20 9 10 133 9
@testcase | 120 | 0    | 80 9 8 80 133
@testcase | 120 | -1   | 8 14 (null) (null) 0
@testcase | 121 | 0    | 17 17 8 136
@testcase | 121 | -1   | 8 (null) 8 8
@testcase | 122 | 100  | 101 50 106 212 100 101 101
@testcase | 124 | 66   | 66
@testcase | 125 |      | (null) (null) 40 60 133
@testcase | 126 |      | 118
@testcase | 127 |      | 0 0 131 131 777 -1 -1 0 0 777 0 0 138 777 0 0 777 (null) (null) 0
@testcase | 128 | -1   | (null) (null) 131 -1 0 0 777 (null) (null) 131 -1 0 0
@testcase | 128 | 0    | 1 2 130 0 -1 0 777 0 131 138 0 -1 0
@testcase | 129 |      | (null)
@testcase | 130 | -1   | 140 777 (null) 140 777 0 777 0 0
@testcase | 130 | 0    | 139 777 4 1 777 139 777 0 0
@testcase | 131 |      | 5 141 (null) 141 (null) 0 777 0 0 -1 777 0 -1
@testcase | 132 |      | 10 20 10 0 0 20 0 0 0 0
@testcase | 133 |      | -1 0 5 (null) (null) (null) 0 0 46
@testcase | 134 |      | 10 20

@fif_codegen
"""
  test6 PROC:<{
    10 PUSHINT   // p.x
    20 PUSHINT   // p.x p.y
  }>
"""

@fif_codegen
"""
  sumXY<Point> PROC:<{  // point.x point.y
    ADD   // '2
  }>
"""
@fif_codegen_avoid sumXY<PointAlias>

@fif_codegen
"""
  test23 PROC:<{ // p.x p.y
    2DROP
    20 PUSHINT   // '10=20
  }>
"""

@fif_codegen
"""
  test29 PROC:<{
    PUSHNULL   // '5
  }>
"""

 */
