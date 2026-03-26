
struct JustIntOrNull {
    v: int?
}

fun getIntOrDefault(v: int?, def: int) {
    __expect_type(v ?? def, "int");
    __expect_type(v ?? null, "int?");
    return v ?? def;
}

fun getIntOrIntOr0(v1: int?, v2: int?) {
    __expect_type(v1 ?? v2, "int?");
    __expect_type(v1 ?? v2 ?? 0, "int");
    return v1 ?? v2 ?? 0;
}

fun env(s: string): int? {
    if (s.beginParse().bitsEqual("one".literalSlice())) { return 1; }
    if (s.beginParse().bitsEqual("two".literalSlice())) { return 2; }
    return null;
}

fun alwaysThrow() {
    throw 123
}

fun getIntAndThrow(): int {
    throw 123
}

fun acceptInt(v: int): int { return v; }


fun main(x: int?) {
    // test inferring
    __expect_type(x ?? 8, "int");
    __expect_type(8 ?? 8, "int");
    __expect_type(8 ?? x, "int");
    __expect_type(x ?? x, "int?");
    __expect_type(null ?? 8, "int");
    __expect_type(x ?? null, "int?");
    __expect_type("" ?? null, "string");
    __expect_type(x ?? x ?? x, "int?");
    __expect_type(x ?? x ?? x ?? 9, "int");
    __expect_type((x ?? "asdf") as int | string | bool, "int | string | bool");
    __expect_type([x ?? (__expect_type(x, "null"), 8 as int?).1], "array<int?>");

    var my: int? = 5;
    __expect_type(my, "int");
    x ?? (my = x);
    __expect_type(my, "int?");
    x ?? (my = 5);
    __expect_type(my, "int?");
    (my = 0) ?? x;
    __expect_type(my, "int");

    var iors: int | string = x ?? "asdf" ?? null;
    var i32or: int32 | int64 | null = ([x].get(0) as unknown as int32?) ?? 8 as int64;

    if (0) {
        __expect_type(alwaysThrow() ?? 6, "never");
    }
    return (iors is int, i32or is int32, i32or);
}


@method_id(101)
fun test1() {
    return (getIntOrDefault(null, 100), getIntOrDefault(7, 100));
}

@method_id(102)
fun test2() {
    return (
        getIntOrIntOr0(8, 7), getIntOrIntOr0(8, null),
        getIntOrIntOr0(null, 7), getIntOrIntOr0(null, 0),
        getIntOrIntOr0(null, null),
    )
}

@method_id(103)
fun test3() {
    return (
        env("one") ?? 9, env("two") ?? 9, env("asdf") ?? 9,
        env("one") ?? env("asdf") ?? 9,
        env("asdf") ?? env("two") ?? 9,
        env("asdf") ?? env("hhh") ?? 9,
    )
}

@method_id(104)
fun test4() {
    var rr: int;
    try { rr = null ?? getIntAndThrow(); }
    catch(ex) { rr = ex; }
    return (
        9 ?? getIntAndThrow(),
        null ?? null ?? 8 ?? getIntAndThrow(),
        env("kkk") ?? env("two") ?? getIntAndThrow(),
        null ?? null,
        rr,
    )
}

@method_id(105)
fun test5(setNull: bool) {
    var xy: (int, int)? = setNull ? null : (10, 20);
    __expect_type(xy ?? (0, 0), "(int, int)");
    var tens1: (int, int) = xy ?? (0 as int32, 0 as int64);
    var tens2: (int | int32, int | int64) = xy ?? (0 as int32, 0 as int64);
    var tens3: (int | int32, int | int64) = xy ?? (0, 0);
    return [tens1, tens2, tens3] as tuple;
}


@method_id(106)
fun test6(xy: int?): int | (int, int) {
    if (xy == null) {
        return xy ?? 8
    }
    if (xy == 0) {
        return xy ?? (1, 2)
    }
    return xy ?? 100
}

@method_id(107)
fun test7(i: int?, b: bool?) {
    return (i ?? b ?? ()) as bool | () | int8
}

@method_id(108)
fun test8() {
    var x: int = 1;
    val y = x ?? (__expect_type(x, "int"), acceptInt(x)).1;
    return (y, x);
}

@method_id(109)
fun test9(v: int?) {
    var root = JustIntOrNull { v };
    val chosen = root.v ?? (root.v = 11);
    __expect_type(root.v, "int");
    return acceptInt(root.v) + chosen;
}

@noinline
fun checkSC(s: slice?, c: cell?) {
    var sOrC : slice | cell? = s ?? c ?? null;
    return (sOrC is slice, sOrC is cell);
}

@method_id(110)
fun test10() {
    return (
        checkSC(createEmptySlice(), createEmptyCell()),
        checkSC(createEmptySlice(), null),
        checkSC(null, createEmptyCell()),
        checkSC(null, null),
    )
}

struct Wrap<T = unknown> { u: T }

@method_id(111)
fun test11(caseId: int) {
    var w: Wrap? = match (caseId) {
        0 => null,
        1 => ({ u: null }),
        else => ({ u: 7 }),
    };
    var fb999 = Wrap { u: 999 as unknown };
    var wOr999 = w ?? fb999;
    __expect_type(wOr999, "Wrap<unknown>");
    var ui: int? = wOr999.u as int?;
    var is999 = ui != null && ui == 999;
    return (w == null, ui == null, is999);
}




/**
@testcase | 0   | 8         | -1 -1 8 46
@testcase | 0   | null      | 0 0 8 48
@testcase | 101 |           | 100 7
@testcase | 102 |           | 8 8 7 0 0
@testcase | 103 |           | 1 2 9 1 2 9
@testcase | 104 |           | 9 8 2 (null) 123
@testcase | 105 | -1        | [ [ 0 0 ] [ 0 46 0 48 ] [ 0 1 0 1 ] ]
@testcase | 105 | 0         | [ [ 10 20 ] [ 10 1 20 1 ] [ 10 1 20 1 ] ]
@testcase | 106 | 80        | (null) 80 1
@testcase | 106 | 0         | (null) 0 1
@testcase | 106 | null      | (null) 8 1
@testcase | 107 | 80 -1     | 80 42
@testcase | 107 | null -1   | -1 2
@testcase | 107 | null null | (null) typeid-2
@testcase | 108 |           | 1 1
@testcase | 109 | 8         | 16
@testcase | 109 | null      | 22
@testcase | 110 |           | -1 0 -1 0 0 -1 0 0
@testcase | 111 | 0         | -1 0 -1
@testcase | 111 | 1         | 0 -1 0
@testcase | 111 | 2         | 0 0 0
 */
