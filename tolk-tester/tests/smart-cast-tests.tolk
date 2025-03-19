// the goal of this file is not only to @testcase results —
// but to check that this file compiles

struct Point {
    x: int;
    y: int;
}

struct PointOfNulls {
    x: int?;
    y: int?;
    sub: Point?;
}

fun rand(): int
    asm "RANDU256";

fun getNullableInt(): int? { return 5; }
fun getNullableSlice(): slice? { return null; }
fun takeNullableInt(a: int?) {}
fun takeNullableSlice(a: slice?) {}
fun increment(mutate x: int) { x += 1; }
fun int.increment(mutate self) { self += 1; }
fun assignToInt(mutate x: int, value: int) { x = value; }
fun int.assignToInt(mutate self, value: int) { self = value; }
fun assignToNullableInt(mutate x: int?, value: int) { x = value; }
fun int?.assignToNullableInt(mutate self, value: int) { self = value; }
fun sameTensor(t: (int, int)) { return t; }
fun sameTensor2(t: (int?, Pair4N)) { return t; }
fun eq<T>(v: T) { return v; }
fun getTwo<X>(): X { return 2 as X; }
fun assignNullTo<T>(mutate v: T?) { v = null; }

type Pair4N = (slice, slice, slice, builder)?;

fun test1(): int {
    var x = getNullableInt();
    var y = getNullableInt();
    if (x != null && y != null) {
        __expect_type(x, "int");
        __expect_type(y, "int");
        return x + y;
    }
    return -1;
}

fun test2() {
    var (x, y) = (getNullableInt(), getNullableInt());
    if (x == null || y == null) {
        return null;
    }
    __expect_type(x, "int");
    __expect_type(y, "int");
    return x + y;
}

fun test3(): int {
    var ([x, y]) = [getNullableInt(), getNullableInt()];
    if (x != null) {
        if (((y)) != null) {
            __expect_type(x, "int");
            __expect_type(y, "int");
            return x + y;
        }
        return x;
    }
    if (rand() > -1) {
        if (y == null) { return -1 }
        else { return y }
    }
    return 0;
}

fun test4() {
    var x = getNullableInt();
    if (x != null && x > 0) {
        var x = getNullableInt();
        if ((x) != null && x + 10 < 0) {
            var x = getNullableInt();
            return 10 > 3 && 10 < 10 && x != null && x + 8 > 10;
        }
    }
    if (x != null && x < 1) {
        return false;
    }
    if (x == null && x == null) {
        __expect_type(x, "null");
        return true;
    }
    return x < x + 3;
}

fun test5() {
    var (a, (b, c)) = (getNullableInt(), (getNullableInt(), getNullableInt()));
    if (a == null) { return -1; }
    if (!(b != null)) { return -2; }
    if (rand() ? c == null && c == null : c == null) { return -3; }
    return a + b + c;
}

fun test6() {
    var a: int? = 5;
    __expect_type(a, "int");
    __expect_type(a != null ? a : null, "int");
    __expect_type(a == null ? "" : a, "int");
    takeNullableInt(a);
    __expect_type(a, "int");
    if (rand()) {
        a = null;
    } else {
        if (rand()) { a = null; }
        else { a = null; }
    }
    __expect_type(a, "null");
    takeNullableSlice(a);   // ok, `slice?` is `slice | null`, here a definitely null
    var b: int? = true ? null : "sl";
    __expect_type(b, "null");
    takeNullableInt(b);
    takeNullableSlice(b);   // same reason
    var c: int? = 10;
    __expect_type(c, "int");
    takeNullableSlice(c = null);
}

fun test7() {
    var (a, b, c, d) = (getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt());
    if (a == null && true) { return -1; }
    if (true && true && 1 && !0 && b == null) { return -2; }
    if (true ? c == null && (((c))) == null && true : false) { return -3; }
    if (!true ? rand() > 0 : a != null && (d == null && b != null)) { return -4; }
    return a + b + c + d;
}

fun test8(x: int?, y: int?) {
    var allGt1 = x != null && x > 1 && y != null && y > 1;
    var xGtY = x != null && y != null && x > y;
    var xLtEq0 = x == null || x < 0;
    (x = 0) < rand() || x > 10;
    return x + 0;
}

fun test9() {
    var x = getNullableInt();
    var y = getNullableInt();
    if (x == null || y == null) {
        return -1;
    }
    __expect_type(x, "int");
    __expect_type(y, "int");
    return x + y;
}

fun test10(): int {
    var (x, y) = (getNullableInt(), getNullableInt());
    if (x == null) {
        if (y == null) { return -1; }
        __expect_type(x, "null");
        __expect_type(y, "int");
        return y;
    }
    if (y == null) {
        return x;
    }
    __expect_type(x, "int");
    __expect_type(y, "int");
    return x + y;
}

@method_id(111)
fun test11() {
    var [x, y] = [getNullableInt(), getNullableInt()];
    if (eq(10) < 0) { return x == null || y == null ? -1 : x + y; }
    if (true && (x == null || y == null) && !!true) { return 0; }
    return x + y;
}

fun test12() {
    var (x, y) = (getNullableInt(), getNullableInt());
    if (rand() ? x == null || y == null : x == null || y == null) { return -1; }
    __expect_type(x, "int");
    __expect_type(y, "int");
    return x + y;
}

fun test13() {
    var x: int? = getNullableInt();
    var y: int? = 10;
    var z = getNullableInt();
    var w = getNullableInt();
    beginCell().storeInt(x!, 32).storeInt(x = getNullableInt()!, 32).storeInt(x, 32)
               .storeInt(y, 32).storeInt(z = 10, 32).storeInt(x + y + z, 32)
               .storeInt(w == null ? -1 : w, 32).storeInt(!(null == w) ? w : -1, 32);
}

fun test14() {
    var (x, y) = (getNullableInt(), getNullableInt());
    if (x == null) {
        x = 0;
    }
    if (y == null) {
        if (rand()) { return 0; }
        else { y = 0; }
    }
    return x + y;
}

fun test20() {
    var t = (getNullableInt(), getNullableInt());
    if (t.0 != null && t.1 != null) {
        __expect_type(t.0, "int");
        __expect_type(t.1, "int");
        return t.0 + t.1;
    }
    t.0 = 10;
    if (t.1 == null) {
        t.1 = 20;
    }
    __expect_type(t.0, "int");
    __expect_type(t.1, "int");
    return t.0 + t.1;
}

fun test21() {
    var t = (getNullableInt(), (getNullableInt(), getNullableInt()));
    if (t.0 != null && t.1.0 != null) {
        if (t.1.1 != null) { return t.0 + t.1.0 + t.1.1; }
        return t.0 + t.1.0;
    }
    if (t.0 != null) {
        return t.0 + 0;
    }
    __expect_type(t.0, "null");
    __expect_type(t.1.0, "int?");
    return t.1.0 == null ? -1 : t.1.0 + 0;
}

fun test22() {
    var t = (getNullableInt(), (getNullableInt(), getNullableInt()));
    if (t.0 == null || t.1.0 == null || t.1.1 == null) {
        return -1;
    }
    return t.0 + t.1.0 + t.1.1;
}

@method_id(123)
fun test23() {
    var (x: int?, y: int?, z: int?) = (getNullableInt(), getNullableInt(), getNullableInt());
    ((x = 1, 0).0, (y = 2, 1).0) = (3, z = 4);
    return x + y + z;
}

@method_id(124)
fun test24(x: int?) {
    if (x == null) {
        __expect_type(x, "null");
        assignToNullableInt(mutate x, 10);
        __expect_type(x, "int?");
        x.assignToNullableInt(x! + 5);
    } else {
        __expect_type(x, "int");
        increment(mutate x);
        x.increment();
        __expect_type(x, "int");
    }
    __expect_type(x, "int?");
    return x;
}

fun test25() {
    var x = (getNullableInt(), getNullableInt(), getNullableInt());
    x.0 = x.2 = rand();
    return (x.0) + ((x.2));
}

fun test26() {
    var x = [getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(),
        getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt()];
    if (~(x.0 = rand())) { return; }
    if ((x.1 = rand()) < (x.2 = rand())) { return; }
    else if (!(x.2 <=> (x.3 = rand()))) { return; }
    x.5 = (x.4 = rand()) ? (x.6 = rand()) : (x.6 = rand());
    if ((x.7 = rand()) as int) { return; }
    if (((((x.8 = rand()) != null)))) { return; }
    if ([x.1, (x.9 = rand())!].1) { return; }
    val result = x.0+x.1+x.2+x.3+x.4+x.5+x.6+x.7+x.8+x.9;
}

fun test27() {
    var (x, _) = ([getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(),
        getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt()], []);
    +(x.0 = rand());
    x.0 += [((x.1 = rand()) < (x.2 = rand() + x.1)) as int].0;
    !(x.2 <=> (x.3 = rand() + x.2));
    x.5 = (x.4 = rand()) ? (x.6 = rand()) : (x.6 = rand());
    (x.7 = rand()) as int;
    (((((x.8 = rand()) != null))));
    [x.1, (x.9 = rand())!].1;
    return x.0+x.1+x.2+x.3+x.4+x.5+x.6+x.7+x.8+x.9;
}

fun test28() {
    var x = (getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt());
    __expect_type((x.0 = rand(), x.0 += (x.1 = rand()) as int, !(x.1 <=> (x.2 = rand() + x.0)) == null, (x.3 = rand()) ? x.3 : (!x.3) as int),
                  "(int, int, bool, int)");
}

fun test29() {
    var x = (getNullableInt(), getNullableInt(), getNullableInt(), getNullableInt());
    __expect_type([x.0 = rand(), ((x.0 += (x.1 = rand()) as int)), !(x.1 <=> (x.2 = rand() + x.0)) == null, (x.3 = rand()) ? x.3 : (!x.3) as int],
                  "[int, int, bool, int]");
}

@method_id(130)
fun test30(initial5: bool) {
    var t: (int?, (int?, (int?, int?))) = initial5
       ? (getNullableInt(), (getNullableInt(), (getNullableInt(), getNullableInt())))
       : (null, (null, (null, null)));
    if (t.0 == null || t.1.0 == null || t.1.1.0 == null || t.1.1.1 == null) {
        if (t.1.0 == null || t.1.1.0 == null) {
            if (t.1.1.0 == null) {
                t.1.1.0 = 4;
            }
            __expect_type(t.1.1.0, "int");
            __expect_type(t.1.1.1, "int?");
            __expect_type(t.1.0, "int?");
            t.1.1.1 = 3;
            t.1.0 = 2;
            __expect_type(t.1.1.1, "int");
            __expect_type(t.1.0, "int");
        }
        if (((((t.1.1.1)))) != null) {}
        else { t.1.1.1 = 3; }
        t.0 = 1;
    }
    return t.0 + t.1.0 + t.1.1.0 + t.1.1.1;
}

fun test31() {
    var t = (getNullableInt(), getNullableInt());
    t.0 == null ? (t.0, t.1) = (1, 2) : (t.1, t.0) = (4, 3);
    return t.0 + t.1;
}

@method_id(132)
fun test32() {
    var t: (int?, (int?, int?)?, (int?, int?)) = (getNullableInt(), (getNullableInt(), getNullableInt()), (getNullableInt(), getNullableInt()));
    if (t.0 == null) { return -1; }
    t.1 != null && t.1.0 == null ? t.1 = (1, 2) : t.1 = (3, 4);
    if (t.2.1 != null) { t.2.0 = 1; t.2.1 = 2; }
    else { [t.2.0, t.2.1] = [3, 4]; }
    return t.0 + t.1.0! + t.1.1! + t.2.0 + t.2.1;
}

@method_id(133)
fun test33(): int {
    var x = getNullableInt();
    repeat (eq(x = 5)) {
        __expect_type(x, "int");
        increment(mutate x);
    }
    return x;
}

fun test34() {
    var (x, y) = (getNullableInt(), getNullableInt());
    if (rand()) { throw (x = 1, y = 2) }
    else { throw (x = 3, y = (1, getNullableInt()!).1) }
    return x + y;
}

fun test35() {
    var (x, y, z, t) = (getNullableInt(), getNullableInt(), getNullableInt(), (getNullableInt(), getNullableInt()));
    assert (x != null, 404);
    assert (t.0 != null && true && !(t.1 == null) && !(z = 4)) throw (y = 404);
    __expect_type(y, "int?");
    return x + t.0 + t.1 + z;
}

fun test36() {
    var x = getNullableInt();
    assert (x == null, x + 0);      // check that x is int there
    __expect_type(x, "null");
}

fun test37() {
    var (x, code) = (getNullableInt()!, getNullableInt());
    try {
    } catch(code) {
        x = 20;
        return x + code;   // code is scoped
    }
    return code == null ? x : x + code;
}

fun assignNull2<T1, T2>(mutate x: T1?, mutate y: T2?) {
    x = null;
    y = null;
}

fun test38() {
    var (x: int?, y: int?) = (1, 2);
    __expect_type(x, "int");
    __expect_type(y, "int");
    assignNull2<int, int>(mutate x, mutate y);
    __expect_type(x, "int?");
    __expect_type(y, "int?");
    if (x != null) {
        if (y == null) { return -1; }
        return x + y;
    }
    var t: (int?, slice?) = (null, null);
    if (!false) { t.0 = 1; }
    if (true) { t.1 = beginCell().endCell().beginParse(); }
    __expect_type(t.0, "int");
    __expect_type(t.1, "slice");
    t.0 + t.1.loadInt(32);
    assignNull2(mutate t.0, mutate t.1);
    __expect_type(t.0, "int?");
    __expect_type(t.1, "slice?");
    t.0 != null && t.1 != null ? t.0 + t.1.loadInt(32) : -1;
    return t.0 != null && t.1 != null ? t.0 + t.1.loadInt(32) : -1;
}

@method_id(139)
fun test39() {
    var x: (int?, int?)? = (4, null);
    x.1 = 10;
    x.1 += 1;
    x!.1 += 1;
    return (x!.0! + x.1);
}

@method_id(140)
fun test40(second: int?) {
    var x: (int?, int?)? = (4, second);
    if (x.1 != null) {
        val result = x.1 + x!.1 + x!!.1 + x.1! + x!!.1!!;
    }
    if (x!.1 != null) {
        val result = x.1 + x!.1 + x!!.1 + x.1! + x!!.1!!;
    }
    if (!(x!!.1 != null)) {
        return -1;
    }
    return x.1 + x!.1 + x!!.1 + x.1! + x!!.1!!;
}

@method_id(141)
fun test41() {
    var t: (int, int)? = null;
    return sameTensor(t = (1, 2));
}

@method_id(142)
fun test42() {
    var t: (int?, (int?, (int, int)?)?) = (getNullableInt(), (1, (2, 3)));
    t.1 = (3,null);
    __expect_type(t.1, "(int?, (int, int)?)");
    __expect_type(t, "(int?, (int?, (int, int)?)?)");
    return (t, t.1);
}

@method_id(143)
fun test43() {
    var t1: ((int, int), int?) = ((1, 2), 3);
    var t2: ((int?, int?), (int?,int?)?) = ((null, null), (null, 5));
    t2.0 = t1.0 = (10, 11);
    t2.1 = t1.1 = null;
    return (t1, t2);
}

@method_id(144)
fun test44() {
    var t1: ((int, int), int?) = ((1, 2), 3);
    var t2: ((int?, int?), (int?,int?)?) = ((null, null), (null, 5));
    t1.0 = t2.0 = (10, 11);
    t1.1 = t2.1 = null;
    __expect_type(t1, "((int, int), int?)");
    __expect_type(t2, "((int?, int?), (int?, int?)?)");
    return (t1, t2);
}

@method_id(145)
fun test45() {
    var t: (int?, (int?, (int, int)?)?) = (getNullableInt(), (1, (2, 3)));
    var t2 = sameTensor2(t.1 = (3,null));
    return (t, t2, t.1);
}

fun autoInfer46() {
    var t1: int? = 3;
    var t2: (int, int)? = (4, 5);
    __expect_type(t1, "int");
    __expect_type(t2, "(int, int)");
    return (t1, t2);        // proven to be not null, inferred (int, (int,int))
}

@method_id(146)
fun test46() {
    var r46_1: (int, (int,int)) = autoInfer46();
    var r46_2: (int, (int,int)?) = autoInfer46();
    return (r46_1, r46_2);
}

@method_id(147)
fun test47() {
    var t1: int? = 3;
    var t2: (int, int)? = (4, 5);
    t1 = t2 = null;
    __expect_type(t1, "null");
    __expect_type(t2, "null");
    var result = (t1, t2);        // proven to be always null, inferred (null, null), 2 slots on a stack
    return (result, 100, result.1, 100, t2 as (int, int)?);
}

fun test48() {
    var t1: int? = getNullableInt();
    if (t1 != null) {
        var (t1 redef, t2) = (10, 5);
        return t1 + t2;
        var t2 redef = getNullableInt()!;
        return t1 + t2;
    }
    return -1;
}

fun test49(x: int?) {
    while (x == null) {
        x = getNullableInt();
    }
    __expect_type(x, "int");
    return x + 1;
}

fun test50() {
    var (x: int?, y: int?) = (1, 2);
    do {
        x = getNullableInt();
        y = getNullableInt();
    } while (x == null || y == null);
    return x + y;
}

fun test51() {
    while (true) { return; }
    // test that no error "control reaches end of function"
}

fun test52() {
    do {  } while (true);
}

fun test53() {
    var x1: int? = getNullableInt();
    var x2: int? = 5;
    var x3: int? = 5;
    var x10: int? = null;
    var x11: int? = 5;
    var x12: int? = 5;
    while (x1 != null) {
        __expect_type(x1, "int");   // because condition
        __expect_type(x2, "int?");  // because re-assigned
        __expect_type(x3, "int?");  // because re-assigned
        __expect_type(x10, "null");
        __expect_type(x11, "int");
        x1 = getNullableInt();
        __expect_type(x1, "int?");
        assignToNullableInt(mutate x2, 5);
        x3.assignToNullableInt(5);
        x11 = 10;
        assignToInt(mutate x12, 5);
    }
    __expect_type(x1, "null");
    __expect_type(x2, "int?");
    __expect_type(x3, "int?");
}

fun test54() {
    var x1: int? = null;
    var x2: int? = 5;
    var x3: int? = 5;
    var x10: int? = null;
    var x11: int? = 5;
    var x12: int? = 5;
    do {
        __expect_type(x1, "int?");  // because re-assigned
        __expect_type(x2, "int?");  // because re-assigned
        __expect_type(x3, "int?");  // because re-assigned
        __expect_type(x10, "null");
        __expect_type(x11, "int");
        x1 = getNullableInt();
        __expect_type(x1, "int?");
        assignToNullableInt(mutate x2, 5);
        if (rand()) { x3.assignToNullableInt(5); }
        x11 = 10;
        assignToInt(mutate x12, 5);
    } while (x1 != null);
    __expect_type(x1, "null");
    __expect_type(x2, "int?");
    __expect_type(x3, "int?");
}

fun eq55<T>(v: T) { return v; }

fun test55() {
    var x: int? = 4;
    while (true) {
        // currently, generic functions are instantiated at the type inferring step
        // in case of loops, type inferring is re-enterable
        // first iteration: x is int, eq<int> instantiated
        // second (final) iteration: x is int?, eq<int?> instantiated
        // (checked via codegen)
        eq55(x);
        __expect_type(x, "int?");   // types are checked (unlike generics instantiated) after inferring
        x = rand() ? 1 : null;
    }
    __expect_type(x, "int?");
}

fun test56() {
    var i: int? = null;
    var (j: int?, k: int?) = (null, null);
    __expect_type(i, "null");
    __expect_type(k, "null");
    i = getTwo();
    [j, ((k))] = [getTwo(), ((getTwo()))];
    __expect_type(i, "int?");
    __expect_type(j, "int?");
    __expect_type(k, "int?");
}

fun test57(mutate x: int?): int {
    if (x == null) { x = 5; }
    else {
        if (x < 10) { x = 10; }
        else { x = 20; }
    }
    if (x != null) {
        return 123;
    }
    __expect_type(x, "int");
    // no "return" needed, because end of function is unreachable
}

@method_id(158)
fun test58() {
    var (x1, x2: int?) = (getNullableInt(), null);
    return (test57(mutate x1), x1, test57(mutate x2), x2);
}

fun test59() {
    var (x1: int?, x2, x3) = (getNullableInt()!, getNullableInt(), 5);
    if ((x2 = x3) != null) {
        __expect_type(x2, "int");
    }
    __expect_type(x2, "int");
    if ((x2 = getNullableInt()) != null) {
        __expect_type(x2, "int");
    }
    __expect_type(x2, "int?");
    if (((x1) = x2) == null) {
        return;
    }
    __expect_type(x1, "int");
}

@method_id(160)
fun test60() {
    var cc1 = match (val a: int | slice = 0 + 10 + 90) {
        int => a + 1,
    };
    var cc2 = match (var a = (0 + 10 + 90) as int | slice) {
        int => (a = 108) + 1,
        slice => 120,
    };
    return (cc1, cc2);
}


@method_id(161)
fun test61(p: Point) {
    var p2: Point? = p;
    __expect_type(p2, "Point");
    if (p.x < 10) {
        assignNullTo(mutate p2);
    }
    __expect_type(p2, "Point?");
    return (p2, p2!);
}

fun test62() {
    var p = PointOfNulls { x: 1, y: null, sub: null };
    __expect_type(p.x, "int?");
    __expect_type(p.sub, "Point?");
    if (p.sub == null) {
        p.x! > 10 ? p.sub = { x: 1, y : 2 } : Point { x: 3, y: 4 };
        __expect_type(p.sub, "Point?");
        p.x! > 10 ? p.sub = { x: 1, y : 2 } : p.sub = eq<Point>({ x: 3, y: 4 });
    }
    __expect_type(p.sub, "Point");
    __expect_type(p!!.sub!!, "Point");
    p.sub.x + p.x! + p.y!;
    return rand() ? null : p;
}

fun test63() {
    __expect_type(test62, "() -> PointOfNulls?");
    var r = test62();
    __expect_type(r!.sub, "Point?");
    if (r != null) {
        r.sub = { x: 1, y: 2 };
    }
    __expect_type(r!.sub, "Point?");
    r!.sub = { x: 1, y: 2 };
    __expect_type(r!.sub, "Point");
}


fun main(x: int?): int {
    return x == null ? -1 : x;
}

/**
@testcase | 0   | 1     | 1
@testcase | 111 |       | 10
@testcase | 123 |       | 7
@testcase | 124 | 4     | 6
@testcase | 124 | null  | 15
@testcase | 130 | -1    | 20
@testcase | 130 | 0     | 10
@testcase | 132 |       | 15
@testcase | 133 |       | 10
@testcase | 139 |       | 16
@testcase | 140 | 5     | 25
@testcase | 141 |       | 1 2
@testcase | 142 |       | 5 3 (null) (null) 0 133 3 (null) (null) 0
@testcase | 143 |       | 10 11 (null) 10 11 (null) (null) 0
@testcase | 144 |       | 10 11 (null) 10 11 (null) (null) 0
@testcase | 145 |       | 5 3 (null) (null) 0 133 3 (null) (null) (null) (null) 0 3 (null) (null) 0
@testcase | 146 |       | 3 4 5 3 4 5 132
@testcase | 147 |       | (null) (null) 100 (null) 100 (null) (null) 0
@testcase | 158 |       | 123 10 123 5
@testcase | 160 |       | 101 109
@testcase | 161 | 9 9   | (null) (null) 0 (null) (null)
@testcase | 161 | 19 0  | 19 0 129 19 0

@stderr warning: expression of type `int` can never be `null`, this condition is always true
@stderr warning: unreachable code
@stderr var t2 redef = getNullableInt()!;

@fif_codegen  eq55<int?> PROC:<{
@fif_codegen  eq55<int> PROC:<{

@fif_codegen
"""
  test60 PROC:<{
    101 PUSHINT	//  cc1
    109 PUSHINT	//  cc1 cc2
  }>
"""
*/
