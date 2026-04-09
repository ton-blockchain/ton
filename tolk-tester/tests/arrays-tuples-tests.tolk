struct Point {
    x: int = 10
    y: int = 20
}

struct BigStruct {      // > 15 slots, additional stack instructions (SETINDEXVAR, etc.)
    t1: (int, int, int, int)
    t2: (int?, int?, int?, int?)
    t3: (bool, bool, bool, bool) = (true, true, true, true),
    t4: (null, null, null, null) = (null, null, null, null)
}

fun int.increment(mutate self): self {
    self += 1;
    return self;
}

fun int.assign(mutate self, new: int) {
    self = new;
}

@noinline
fun array<int?>.countNotNull(self) {
    var (i, nn) = (0, 0);
    repeat (self.size()) {
        nn += (self.get(i) != null) as int;
        i += 1;
    }
    return -nn;
}

@noinline
fun array<Point>.create2(): array<Point> {
    return [
        {},
        {x: 100, y: 200},
    ];
}

@noinline
fun array<Point?>.create3(): array<Point?> {
    return [
        {},
        {x: 100, y: 200},
        null,
    ];
}

fun array<T>.concatWith<T2>(mutate self, with: array<T2>) {
    var i = 0;
    repeat (with.size()) {
        self.push(with.get(i));
        i += 1;
    }
}

struct ContainerOfPoints {
    id: int = 0
    twoPoints: [Point, Point]
    twoOptionalPoints: [Point?, Point?]
}

struct ShapedContainerOfPoints {
    twoContainers: [ContainerOfPoints, ContainerOfPoints]
    amount: int = 2
}

fun throwPointIfNot(x: int) {
    if (x == 0) {
        throw (123, Point{})
    }
}

struct LinkedTree<T> {
    value: T
    next: array<LinkedTree<T>>
}

fun LinkedTree<T>.flattenValues(self): array<T> {
    var result: array<T> = [];
    self.flattenValuesInto(mutate result);
    return result;
}

fun LinkedTree<T>.flattenValuesInto(self, mutate out: array<T>): void {
    out.push(self.value);
    var i = 0;
    while (i < self.next.size()) {
        self.next.get(i).flattenValuesInto(mutate out);
        i += 1;
    }
}

struct StrangeLinkedShape {
    next1: [StrangeLinkedShape?] = [null]
}

fun StrangeLinkedShape.getDepth(self): int {
    if (self.next1.0 == null) {
        return 0;
    }
    return 1 + self.next1.0.getDepth();
}

fun eq<T>(v: T) { return v }


@method_id(101)
fun test101() {
    var t1 = [];
    var t2 = [1];
    val rand = ([1] as [int]).0;

    __expect_type([], "array<unknown>");
    __expect_type([1], "array<int>");
    __expect_type([1,2,3], "array<int>");
    __expect_type([1 as int8, 2 as int8], "array<int8>");
    __expect_type([1 as unknown, 2 as unknown], "array<unknown>");
    __expect_type([1, 2] as array<unknown>, "array<unknown>");
    __expect_type([1, "aba"] as array<unknown>, "array<unknown>");

    __expect_type([null], "array<null>");
    __expect_type([1, null], "array<int?>");
    __expect_type([Point{}, null], "array<Point?>");

    __expect_type([[]], "array<array<unknown>>");
    __expect_type([[[]]], "array<array<array<unknown>>>");
    __expect_type([[], [], []], "array<array<unknown>>");
    __expect_type([[1], [2], array<int> []], "array<array<int>>");
    __expect_type([[], []] as array<array<int>>, "array<array<int>>");
    __expect_type([[Point{}], [Point{}, Point{}]], "array<array<Point>>");
    __expect_type([null, [1, null], [null, 4]], "array<array<int?>?>");

    __expect_type((1, [1]), "(int, array<int>)");

    __expect_type((rand ? [1,2] : [3, null]) as array<int?>, "array<int?>");
    __expect_type(rand ? [""] : [] as array<string>, "array<string>");
    __expect_type(rand ? [] : [""] as tuple, "array<unknown>");

    __expect_type(array<int> [], "array<int>");
    __expect_type(tuple [1, "aba", []], "tuple");
    __expect_type(array<array<slice?>?> [[null], null], "array<array<slice?>?>");
    __expect_type(array<Point> [{}, {y: 30}], "array<Point>");

    __expect_type((rand ? array<int?> [1,2] : array<int?> [3, null]), "array<int?>");
    __expect_type(rand ? [""] : array<string> [], "array<string>");
    __expect_type(rand ? [] : tuple [""], "array<unknown>");

    t1.push(123);
    t2.push(456);
    return (t1.get(0), t2.first(), t2.last(), t2.size());
}

@method_id(102)
fun test102() {
    return (
        Point{} as unknown,
        Point{} as Point? as unknown,
        ((1, 2) as unknown as Point).y,
        5 as int|slice as unknown as unknown,
        ((5 as int|slice) as unknown as Point).y,
        match (val v = [5, 1] as unknown as int | slice) { int => v, slice => throw 123 },
        [
            null as unknown,
            1 as unknown,
            () as unknown,
            [1] as unknown,
            (1, 2) as unknown,
            (1, 2) as (int, int)? as unknown,
        ],
        5 is unknown,
        (5 as int?) !is unknown,
    )
}

@method_id(103)
fun test103() {
    var a1 = [1, 2];
    var a2 = [(), ()];
    var a3 = [[] as [], [] as []];
    var a4 = [Point{}, Point{x: 100, y: 200}];
    var a5 = [Point{} as Point?, Point{x: 100, y: 200}];
    var a6 = [[], [1] as [int] as unknown];

    __expect_type(a1, "array<int>");
    __expect_type(a2, "array<()>");
    __expect_type(a3, "array<[]>");
    __expect_type(a4, "array<Point>");
    __expect_type(a5, "array<Point?>");
    __expect_type(a6, "array<unknown>");

    return (
        a1.get(0),
        a2.get(0),
        a2.get(0) as unknown,
        a3.get(0),
        a4.get(0),
        a5.get(0),
        a5.get(0)!,
        a5.get(0)! as unknown,
        [1, 2, 3].last(),
    )
}

@method_id(104)
fun test104() {
    var a: array<int?> = [1, 2, null, 3, null, 4];
    return (a.get(2), a.countNotNull());
}

@method_id(105)
fun test105() {
    var l1: array<Point> = [{x: 100}, {}];
    val sh: [Point, Point?] = [{x: 100}, {}];
    val l2 = array<array<Point?>> [[{x:100}], [{}]];
    l1.set({y:50}, 1);
    return (l1.get(0).x, l1.get(1).y, sh.0.x, sh.1!.y, l2.get(0).get(0)!.x);
}

@method_id(106)
fun test106() {
    var t1 =  array<int | bool | null> [];
    t1.push(10);
    t1.push(true);
    t1.push(false);
    t1.push(null);
    var out = [];
    var i = 0;
    while (i < t1.size()) {
        match (val kk = t1.get(i)) {
            int => { out.push(kk * 2) }
            bool => { out.push(!kk) }
            null => { out.push(-100) }
        }
        i = i + 1;
    }
    return out;
}

@method_id(107)
fun test107() {
    return [
        1,
        Point{},
        (),
        (null, null)
    ] as tuple
}

@method_id(108)
fun test108(): array<unknown> {
    var v = -5;
    return [
        v,
        Point { y: v += 15, },
        v *= 5,
        Point { x: v /= 5, y: v },
        v,
        (),
        (v.increment(), v *= 10, v),
        v,
    ];
}

@method_id(109)
fun test109() {
    var arr1 = array<unknown> [
        (10, 20),
        (30, 40),
    ];
    var arr2 = array<(int, int)> [
        (10, 20),
        (30, 40),
    ];
    return (arr1.pop(), arr1, arr2.pop(), arr2, arr1.first(), arr2.first());
}

@method_id(110)
fun test110() {
    return (
        [Point{}, null, null as Point?],    // it's array<Point?>
        [Point{}, null, null as Point?] as [Point, null, Point?],
    )
}

@method_id(111)
fun test111() {
    var sh: [Point?, Point?] = [Point{}, null];
    return sh;
}

@method_id(112)
fun test112() {
    var sh = [Point{} as Point?, Point{}, null];
    __expect_type(sh, "array<Point?>");
    return sh;
}

@method_id(113)
fun test113() {
    var a3 = array<Point>.create2();
    return (
        a3.size(),
        a3.first(),
        a3.last(),
        a3.pop(),
        a3.size(),
        a3.get(0)!,
    )
}

@method_id(114)
fun test114() {
    var a3 = array<Point?>.create3();
    return (
        a3.size(),
        a3.first(),
        a3.last(),
        a3.pop(),
        a3.push({}),
        a3.size(),
        a3.get(2)!,
    )
}

@method_id(115)
fun test115() {
    var a1: array<Point> = [Point{}];
    var a2: array<Point?> = a1;
    var a3: array<int> = [1, 2, 3];
    var a4: array<int | slice> = a3;
    var a5: array<int | slice | Point> = a3;
    var a6: array<int | slice | Point> = a4;

    assert (a2.get(0) != null) throw 123;
    assert (a2.get(0)!.x == 10) throw 123;

    var i = 1;
    repeat (a3.size()) {
        var ith_a4 = a4.get(i - 1);
        var ith_a5 = a5.get(i - 1);
        var ith_a6 = a6.get(i - 1);
        assert (ith_a4 is int && ith_a4 == i) throw 123;
        assert (ith_a5 is int && ith_a5 == i) throw 123;
        assert (ith_a6 is int && ith_a6 == i) throw 123;
        i += 1;
    }

    return (a2, a4, a5.last());
}

@method_id(116)
fun test116() {
    var a1: array<Point> = [];
    var a2: array<Point?> = [];
    var a3: array<Point?> = [];
    var a4: array<int?> = [];
    var a5: array<int | slice | null> = [];

    a2 = a1 = [Point{}];
    (a3, a4, a5) = ([null, null], [null, null], [null, null]);

    return (a1, a2, a3, a4, a5);
}

@method_id(117)
fun test117() {
    var a1: [Point] = [Point{}];
    var a2: [Point?] = [Point{}];

    return (a1, a2);
}

@method_id(118)
fun test118() {
    var a1: [Point] = [Point{}];
    var a2: [Point?] = [null];
    var a3: [Point] = [Point{}];
    var a4: [Point?] = [null];

    a1 = a2 = [Point{}];
    a4 = a3 = [Point{}];

    return (a1, a2, a3, a4);
}

@method_id(119)
fun test119() {
    var a1: [(int, int)?, int?, int | slice | null, int | Point | null]? = null;
    var a2: [bool?, Point?, Point?, (int,int,int,int)?]? = null;
    var a3: [(int,int,int,int)?, int?, unknown, array<unknown>?]? = null;

    return (a1, a2, a3);
}

@method_id(120)
fun test120() {
    var a1: [(int, int)?, int?, int | slice | null, int | Point | null]? = null;
    var a2: [bool?, Point?, Point?, (int,int,int,int)?]? = null;
    var a3: [(int,int,int,int)?, int?, unknown, array<unknown>?]? = null;

    a1 = a2 = a3 = [null, null, null, null];

    return (a1, 777, a2, 777, a3);
}

@method_id(121)
fun test121(nullMode: int): array<Point?>? {
    return nullMode == 1 ? [null, null] : nullMode == 2 ? [Point{}] : null;
}

@method_id(122)
fun test122() {
    var sh = ShapedContainerOfPoints {
        twoContainers: [
            ContainerOfPoints {
                twoPoints: [Point {}, Point{}],
                twoOptionalPoints: [null, null],
            },
            ContainerOfPoints {
                twoPoints: [Point {}, Point{}],
                twoOptionalPoints: [null, null],
            }
        ],
    };
    sh.twoContainers.1.twoPoints.0.x += 989;
    sh.twoContainers.0.twoOptionalPoints.1 = Point{};

    // smart cast works
    __expect_type(sh.twoContainers.0.twoOptionalPoints.1, "Point");
    sh.twoContainers.0.twoOptionalPoints.1.y += 979;

    if (sh.twoContainers.1.twoOptionalPoints.0 == null) {
        sh.twoContainers.1.twoOptionalPoints.0 = Point{};
    }
    sh.twoContainers.1.twoOptionalPoints.0.x.assign(1777);

    return (sh.twoContainers.0, sh.twoContainers.1);
}

global gShaped: ShapedContainerOfPoints;

@method_id(123)
fun test123() {
    gShaped = ShapedContainerOfPoints {
        twoContainers: [
            ContainerOfPoints {
                twoPoints: [Point {}, Point{}],
                twoOptionalPoints: [null, null],
            },
            ContainerOfPoints {
                twoPoints: [Point {}, Point{}],
                twoOptionalPoints: [null, null],
            }
        ],
    };
    gShaped.twoContainers.1.twoPoints.0.x += 989;
    gShaped.twoContainers.0.twoOptionalPoints.1 = Point{};

    // smart cast works
    __expect_type(gShaped.twoContainers.0.twoOptionalPoints.1, "Point?");
    gShaped.twoContainers.0.twoOptionalPoints.1!.y += 979;

    if (gShaped.twoContainers.1.twoOptionalPoints.0 == null) {
        gShaped.twoContainers.1.twoOptionalPoints.0 = Point{};
    }
    gShaped.twoContainers.1.twoOptionalPoints.0!.x.assign(1777);

    return (gShaped.twoContainers.0, gShaped.twoContainers.1);
}

fun setPointYInUnion(mutate u: int|Point|null, newY: int) {
    if (u is Point) {
        u.y = newY;
    }
}

@method_id(124)
fun test124() {
    var sh: [int|builder, int|Point, int|Point|null, int|Point|null, int|(int,int,int)] =
        [beginCell(), 5, Point{}, null, 5];
    if (sh.0 is builder) {
        sh.0.storeUint(123, 32);
    }
    if (sh.1 is int) {
        sh.1 += 1;
    }
    setPointYInUnion(mutate sh.2, 200);
    setPointYInUnion(mutate sh.3, 200);

    __expect_type(sh.4, "int | (int, int, int)");
    if (sh.4 is int) {
        sh.4 = (0, 0, sh.4);
    }
    __expect_type(sh.4, "(int, int, int)");

    return [
        match (sh.0) { builder => sh.0.toSlice().loadUint(32), int => throw 123 },
        sh.1,
        sh.2 is Point,
        sh.3 is Point,
        sh.2,
        sh.3,
        sh.4,
    ] as array<unknown>
}

@method_id(125)
fun test125() {
    try {
        throwPointIfNot(0);
    } catch(err, arg) {
        return (arg == null, arg as Point);
    }
    throw 456;
}

@method_id(126)
fun test126() {
    var l: LinkedTree<int> = {
        value: 0,
        next: [
            {value: 1, next: []},
            {value: 2, next: [
                {value: 3, next: []},
                {value: 4, next: [
                    {value: 5, next: []}
                ]},
            ]}
        ],
    };
    return l.flattenValues();
}

@method_id(127)
fun test127() {
    var arr = [1, 2, 3];
    arr.concatWith(arr);
    arr.concatWith([4, 5]);
    arr.concatWith<int>([]);
    arr.concatWith([6 as int8, 7 as int8]);
    arr.concatWith([8 as int32] as array<int32>);
    return arr;
}

@method_id(128)
fun test128() {
    var sh = StrangeLinkedShape {
        next1: [{
            next1: [{}],
        }],
    };
    return (sh.getDepth(), StrangeLinkedShape{}.getDepth());
}

@method_id(129)
fun test129() {
    var a = [
        BigStruct{
            t1: (1, 2, 3, 4),
            t2: (5, null, 6, null),
            t3: (false, true, false, true),
            t4: (null, null, null, null),
        }
    ];
    a.push({
        t1: (10, 20, 30, 40),
        t2: (50, null, 60, null),
    });
    return (
        a.size(),
        a.first().t3.3,
        a.last().t1.2,
        a.pop().t4.0,
        a.size(),
        a.get(0).t3.2,
    )
}

@method_id(130)
fun test130() {
    var xp = [1, {}, 3] as [int, Point, int];
    var [x1, p1, v1] = [1, Point{}, 3];
    var [x2, p2, v2] = xp;
    return (x1, p1, v1, x2, p2, v2);
}

@method_id(131)
fun test131(idx: int) {
    var a0: array<int> = [0];
    a0.set(100, idx);      // dynamic index via `SETINDEXVAR` 
    var a1: array<Point> = [{}];
    a1.set({
        x: 100,
        y: 100
    }, idx); 
    var a2: array<BigStruct> = [{
        t1: (10, 20, 30, 40),
        t2: (50, null, 60, null),
    }];
    a2.set({
        t1: (100, 200, 300, 400),
        t2: (500, null, 600, null),
    }, idx);
    return (a0.get(idx), a1.get(idx), a2.get(idx).t1);
}

global g132: [Point];

@method_id(132)
fun test132() {
    g132 = [{}];
    g132.0.x = 50;
    g132.0.y.increment();
    return g132;
}

@method_id(133)
fun test133() {
    var t: tuple = [1,2,3];
    __expect_type(t, "tuple");
    __expect_type(t.get(0), "unknown");

    t.set("asdf", 0);
    try {
        return (t.get(0) as int == 123) as int;
    } catch (ex) {
        return ex;
    }
}

@method_id(134)
fun test134() {
    var t: tuple = [1];
    var ai = [1];
    var an = array<int?> [1];

    t = ai;         // assigning array<int> to array<unknown>
    t = an;         // and similar works fine
    an = ai;        // without runtime transformations
    return (t, an);
}

@method_id(135)
fun test135() {
    var t1 = array<int> [];
    var t2 = array<Point> [];
    var i = 0;
    repeat (255) {
        t1.push(i);
        t2.push({x: i});
        i += 1;
    }
    return (t1.first(), t1.last(), t2.first(), t2.last(), t2.get(254).x, t2.size());
}

@method_id(136)
fun test136(): tuple {
    return [
        1,
        true,
        (2,3)
    ];
}

@method_id(137)
fun test137() {
    var a: int;
    var b: (int, int);
    return (eq([a, b] = [1, (2, 3)]), a, b);
}

@method_id(138)
fun test138() {
    var sh: [[Point?]?]? = [[null]];
    __expect_type(sh, "[[Point?]?]");
    __expect_type(sh.0!.0, "Point?");
    sh.0!.0 = {};
    __expect_type(sh.0!.0, "Point");
    return sh.0!.0;
}

@method_id(139)
fun test139(assignInt: bool) {
    var i: [int | bool | null] = [null];
    if (assignInt) {
        i.0 = 5;
    } else {
        i.0 = true;
    }
    __expect_type(i.0, "int | bool");
    return i.0;
}

@method_id(140)
fun test140(): [null, Point?] {
    var n1: Point?;
    var n2: Point?;
    [n1, n2] = [null, null as Point?];
    return [n1, n2];
}

@method_id(141)
fun test141(i: int) {
    var u: unknown = i == 0 ? null : i;
    __expect_type(u, "unknown");
    __expect_type(u!, "unknown");
    return (
        u == null,
        u != null,
        u is null,
        u,
        u!,
        u! == null,
        u is unknown,
        u !is unknown,
    )
}

@method_id(142)
fun test142() {
    var tuple = [1, null, 8, null];
    tuple.push(9);
    tuple.push(null);
    var nulls: array<bool> = [];
    var ints: array<int> = [];
    var i = 0;
    repeat (tuple.size()) {
        nulls.push(tuple.get(i) == null);
        ints.push(tuple.get(i)!);
        i += 1;
    }
    return (nulls, ints, tuple is array<int>, tuple is array<int?>, tuple is array<unknown>);
}

@method_id(143)
fun test143() {
    var iorb = array<int | bool> [1, true, 3, false];
    return (
        iorb.get(0) is int,
        iorb.get(1) is int,
        iorb.get(2),
        match (val ii = iorb.get(3)) { int => throw 123, bool => !ii },
    )
}

@method_id(144)
fun test144() {
    var p: array<Point?> | slice = [{}, null];
    return p;
}

@method_id(145)
fun test145(u: unknown) {
    try {
        val u2: unknown = u as (int, int) as unknown;
        throw (42, u2);
    } catch (ex, arg) {
        if (ex == 42) {
            return (ex, arg);
        }
        return (0, 0);
    }
}

@method_id(146)
fun test146() {
    return test145((1, 2));
}

@method_id(147)
fun test147(flag: bool) {
    var src: Point? = flag ? Point { x: 8, y: 9 } : null;
    return ([src].get(0) ?? null) as unknown;
}

@method_id(148)
fun test148(flag: bool) {
    var src: ()? = flag ? () : null;
    var coalesced = through_unknown(src);
    __expect_type(coalesced, "()?");
    return coalesced == null;
}


fun through_unknown<T>(src: T?): T? {
    var recovered = [src as unknown].get(0) as T?;
    return ((recovered ?? null) as unknown) as T?;
}

fun inferArrayUnknown(): array<unknown> {
    if ([0].get(0)) {
        return [1,2,3];
    }
    return ["aba"];
}

fun main() {
    __expect_type(test145, "(unknown) -> (int, unknown)");
}

/**
@testcase | 101 |    | 123 1 456 2 
@testcase | 102 |    | [ 10 20 ] [ 10 20 129 ] 2 [ 5 1 ] 1 5 [ (null) 1 [] [ 1 ] [ 1 2 ] [ 1 2 130 ] ] 0 -1
@testcase | 103 |    | 1 [] [] 10 20 10 20 129 10 20 [ 10 20 ] 3
@testcase | 104 |    | (null) 4
@testcase | 105 |    | 100 50 100 20 100
@testcase | 106 |    | [ 20 0 -1 -100 ]
@testcase | 107 |    | [ 1 [ 10 20 ] [] [ (null) (null) ] ]
@testcase | 108 |    | [ -5 [ 10 10 ] 50 [ 10 10 ] 10 [] [ 11 110 110 ] 110 ]
@testcase | 109 |    | [ 30 40 ] [ [ 10 20 ] ] 30 40 [ [ 10 20 ] ] [ 10 20 ] 10 20
@testcase | 110 |    | [ [ 10 20 129 ] [ (null) (null) 0 ] [ (null) (null) 0 ] ] [ [ 10 20 ] (null) [ (null) (null) 0 ] ]
@testcase | 111 |    | [ [ 10 20 129 ] [ (null) (null) 0 ] ]
@testcase | 112 |    | [ [ 10 20 129 ] [ 10 20 129 ] [ (null) (null) 0 ] ]
@testcase | 113 |    | 2 10 20 100 200 100 200 1 10 20
@testcase | 114 |    | 3 10 20 129 (null) (null) 0 (null) (null) 0 3 10 20
@testcase | 115 |    | [ [ 10 20 129 ] ] [ [ 1 1 ] [ 2 1 ] [ 3 1 ] ] (null) 3 1
@testcase | 116 |    | [ [ 10 20 ] ] [ [ 10 20 129 ] ] [ [ (null) (null) 0 ] [ (null) (null) 0 ] ] [ (null) (null) ] [ [ (null) 0 ] [ (null) 0 ] ]
@testcase | 117 |    | [ [ 10 20 ] ] [ [ 10 20 129 ] ]
@testcase | 118 |    | [ [ 10 20 ] ] [ [ 10 20 129 ] ] [ [ 10 20 ] ] [ [ 10 20 129 ] ]
@testcase | 119 |    | (null) (null) (null)
@testcase | 120 |    | [ [ (null) (null) 0 ] (null) [ (null) 0 ] [ (null) (null) 0 ] ] 777 [ (null) [ (null) (null) 0 ] [ (null) (null) 0 ] [ (null) (null) (null) (null) 0 ] ] 777 [ [ (null) (null) (null) (null) 0 ] (null) (null) (null) ]
@testcase | 121 | 1  | [ [ (null) (null) 0 ] [ (null) (null) 0 ] ]
@testcase | 121 | 2  | [ [ 10 20 129 ] ]
@testcase | 121 | 3  | (null)
@testcase | 122 |    | 0 [ [ 10 20 ] [ 10 20 ] ] [ [ (null) (null) 0 ] [ 10 999 129 ] ] 0 [ [ 999 20 ] [ 10 20 ] ] [ [ 1777 20 129 ] [ (null) (null) 0 ] ]
@testcase | 123 |    | 0 [ [ 10 20 ] [ 10 20 ] ] [ [ (null) (null) 0 ] [ 10 999 129 ] ] 0 [ [ 999 20 ] [ 10 20 ] ] [ [ 1777 20 129 ] [ (null) (null) 0 ] ]
@testcase | 124 |    | [ 123 [ (null) 6 1 ] -1 0 [ 10 200 129 ] [ (null) (null) 0 ] [ 0 0 5 ] ]
@testcase | 125 |    | 0 10 20
@testcase | 126 |    | [ 0 1 2 3 4 5 ]
@testcase | 127 |    | [ 1 2 3 1 2 3 4 5 6 7 8 ]
@testcase | 128 |    | 2 0
@testcase | 129 |    | 2 -1 30 (null) 1 0
@testcase | 130 |    | 1 10 20 3 1 10 20 3
@testcase | 131 | 0  | 100 100 100 100 200 300 400
@testcase | 132 |    | [ [ 50 21 ] ]
@testcase | 133 |    | 7
@testcase | 134 |    | [ 1 ] [ 1 ]
@testcase | 135 |    | 0 254 0 20 254 20 254 255
@testcase | 136 |    | [ 1 -1 [ 2 3 ] ]
@testcase | 137 |    | [ 1 [ 2 3 ] ] 1 2 3
@testcase | 138 |    | 10 20
@testcase | 139 | -1 | 5 1
@testcase | 139 | 0  | -1 2
@testcase | 140 |    | [ (null) [ (null) (null) 0 ] ]
@testcase | 141 | 8  | 0 -1 0 8 8 0 -1 0
@testcase | 141 | 0  | -1 0 -1 (null) (null) -1 -1 0
@testcase | 142 |    | [ 0 -1 0 -1 0 -1 ] [ 1 (null) 8 (null) 9 (null) ] 0 -1 0
@testcase | 143 |    | -1 0 3 1 -1
@testcase | 144 |    | [ [ 10 20 129 ] [ (null) (null) 0 ] ]
@testcase | 145 | 1  | 0 0
@testcase | 146 |    | 42 [ 1 2 ]
@testcase | 147 | 0  | [ (null) (null) 0 ]
@testcase | 147 | -1 | [ 8 9 129 ]
@testcase | 148 | 0  | -1
@testcase | 148 | -1 | 0

@fif_codegen
"""
  test104() PROC:<{             // 
    1 PUSHINT                   //  '1=1
    2 PUSHINT                   //  '1=1 '2=2
    PUSHNULL                    //  '1=1 '2=2 '3
    3 PUSHINT                   //  '1=1 '2=2 '3 '4=3
    PUSHNULL                    //  '1=1 '2=2 '3 '4=3 '5
    4 PUSHINT                   //  '1=1 '2=2 '3 '4=3 '5 '6=4
    6 TUPLE                     //  a
    DUP                         //  a a
    2 INDEX                     //  a '9
    SWAP                        //  '9 a
    array<int?>.countNotNull() CALLDICT //  '9 '10
  }>
"""

@fif_codegen
"""
  test113() PROC:<{ 
    array<Point>.create2() CALLDICT
    DUP
    TLEN
    OVER
    FIRST 2 UNTUPLE
    s3 PUSH
    LAST 2 UNTUPLE
    s0 s5 XCHG
    TPOP 2 UNTUPLE
    s2 PUSH
    TLEN
    s0 s3 XCHG
    0 INDEX 2 UNTUPLE
    s8 s9 XCHG
    s7 s8 XCHG
    s6 s7 XCHG
    s5 s6 XCHG
    s3 s4 XCHG
    s2 s3 XCHG
  }>
"""

@fif_codegen
"""
  test134() PROC:<{            	// 
    1 PUSHINT               	//  '4=1
    1 TUPLE                 	//  ai
    1 PUSHINT               	//  ai '7=1
    1 TUPLE                 	//  an t
    SWAP                    	//  t an
  }>
"""

@fif_codegen
"""
  test136() PROC:<{            	// 
    1 PUSHINT               	//  '1=1
    TRUE                    	//  '1=1 '2
    2 PUSHINT               	//  '1=1 '2 '3=2
    3 PUSHINT               	//  '1=1 '2 '3=2 '4=3
    2 TUPLE                 	//  '1=1 '2 '5
    3 TUPLE                 	//  '0
  }>
"""
 */
