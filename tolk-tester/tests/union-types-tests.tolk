type MInt = int;
type MNull = null;
type MSlice = slice;
type MIntN = int | MNull;
type IntOrSlice = MInt | slice;
type IntOrSliceN = null | slice | int;
type IntOrSliceOrBuilder = int | slice | builder;

type Pair2 = (int, int);
type Pair2Or3 = Pair2 | (int, int, int);
type SomeIntBits = int8 | int9 | int10 | int32 | uint64;

fun getSlice32(): slice { return beginCell().storeInt(32, 32).endCell().beginParse(); }
fun getIntOrSlice(): int | slice { return 5; }

fun testFlatten() {
    __expect_type(0 as (int | int), "int");
    __expect_type(0 as (int | int | slice | builder | slice), "int | slice | builder");
    __expect_type(0 as (int? | int), "int?");
    __expect_type(0 as (slice | int? | slice? | cell), "slice | int | null | cell");
    __expect_type(0 as (int | (int | int)), "int");
    __expect_type(0 as (slice | (slice | (slice | int) | (cell | slice?))), "slice | int | cell | null");

    __expect_type(0 as (MInt | MNull), "MInt?");
    __expect_type(0 as (MInt | slice | int), "MInt | slice");
    __expect_type(0 as (MIntN), "MIntN");
    __expect_type(0 as (MIntN | null), "int?");
    __expect_type(0 as (MIntN | MNull | MSlice), "int | null | MSlice");
    __expect_type(0 as (MIntN | MNull? | MSlice), "int | null | MSlice");
    __expect_type(0 as (MInt | IntOrSliceOrBuilder | MSlice), "MInt | slice | builder");
    __expect_type((1, 2) as (Pair2 | Pair2Or3 | (slice, int?)), "Pair2 | (int, int, int) | (slice, int?)");

    __expect_type(0 as (int | int8), "int | int8");
    __expect_type(0 as (MInt | int8), "MInt | int8");
    __expect_type((0 as MInt) as (int | int8), "int | int8");
    __expect_type((0 as MInt) as (MInt | int8), "MInt | int8");

    __expect_type(0 as IntOrSlice, "IntOrSlice");
    __expect_type(0 as IntOrSlice?, "MInt | slice | null");
    __expect_type(0 as IntOrSliceN, "IntOrSliceN");
    __expect_type(0 as IntOrSliceN?, "null | slice | int");

    __expect_type(0 as (int?)?, "int?");
    __expect_type(0 as (int?)? | (int)?, "int?");

    __expect_type(0 is builder, "bool");
    __expect_type(0 !is builder, "bool");
    __expect_type(0!!is builder, "bool");

    __expect_type(match (0) { int => [] }, "[]");
    __expect_type(match (0 as int|slice) { int => 0, slice => 0 }, "int");
    __expect_type(match (0 as int|slice) { int => 0, slice => 0 as int8 } as int|int8, "int | int8");
    __expect_type(match (0 as int?) { int => "", null => null }, "slice?");
    __expect_type(match (0 as int?) {
        int => 10>3 ? match(0) { int => 0 } : match (10>3 ? 6 : null) { null => 0, int => { return; } },
        null => match(null) { null => 0 }
    }, "int");
}

@method_id(101)
fun test1() {
    var a = getIntOrSlice();
    var b: MInt | MSlice = getIntOrSlice();
    var c: IntOrSlice = getIntOrSlice();
    __expect_type(a, "int | slice");
    __expect_type(b, "MInt | MSlice");
    __expect_type(c, "IntOrSlice");
    return (a, b, c);
}

@method_id(102)
fun test2() {
    var a: int | slice = 4;
    __expect_type(a, "int");
    a = getIntOrSlice();
    __expect_type(a, "int | slice");
    a = getSlice32();
    __expect_type(a, "slice");
    return a.loadInt(32);
}

@method_id(103)
fun test3(case: int): int | slice | null {
    var a: int | slice | null = 4;
    if (case == 0) {
        a = getSlice32()
    } else if (case == 1) {
        a = 10
    } else {
        a = null
    }
    return a;
}

@method_id(104)
fun test4() {
    return (
        5 as int | int,
        (5 as int8) as int8 | int8,
        true as bool | bool,
        5 as int | int | (int | slice),
        (5 as int | int | (int | slice)) as int | slice | null,
    )
}

@method_id(105)
fun test5() {
    var a: int | int | IntOrSlice = getIntOrSlice();
    __expect_type(a, "int | slice");
    a = 5;
    return (a, a as IntOrSlice, a as IntOrSliceOrBuilder, a as MIntN);
}

@method_id(106)
fun test6(case: int): int | slice | builder | null {
    var a: int? | slice? | builder = null;
    __expect_type(a, "null");
    if (case == 1) {
        a = null as slice? | builder;
        __expect_type(a, "null | slice | builder");
        __expect_type(a!, "slice | builder");
        return a
    }
    if (case == 2) {
        __expect_type(a = 2 as MInt, "MInt");
        __expect_type(a, "int");
        a = 2;
        __expect_type(a, "int");
        return a + 0
    }
    if (case == 3) {
        a = null as slice?;
        __expect_type(a, "slice?");
        return a
    }
    a = null as MNull;
    __expect_type(a, "null");
    return a;
}

@method_id(107)
fun test7(case: int) {
    var a = (2, 3);
    var b: Pair2 = (4, 5);
    var c: Pair2Or3 = case == 1 ? a : case == 2 ? b : (6, 7, 8);
    __expect_type(c, "Pair2Or3");
    c as Pair2Or3;
    c as Pair2Or3 | Pair2Or3;
    c as (int, int, int) | Pair2;
    c as (int, int) | (int, int, int);
    return (c, c as (int, int)? | (int, int, int) | null);
}

@method_id(108)
fun test8() {
    return (
        (2, 3) as (int, int) | (int, int, int) | null,
        null as (int, int) | null | (int, int, int),
        (2, 3) as Pair2Or3 | null,
        (2, 3) as null | Pair2Or3,
        null as Pair2Or3?
    )
}

@method_id(109)
fun test9() {
    var c: null | Pair2Or3 = (6, 7, 8) as Pair2Or3?;
    __expect_type(c, "null | Pair2 | (int, int, int)");
    __expect_type(c!, "Pair2 | (int, int, int)");
    c! as Pair2Or3;
    c! as (int, int, int) | Pair2;
    (c! as (int, int) | (int, int, int)) as Pair2Or3;
    return (c, c == null, c = null, c == null, c = null as Pair2Or3?, c == null, c as Pair2? | (int, int, int)? | null);
}

@method_id(110)
fun test10() {
    var t = getSlice32() as (int, (int, int)?, (int?, int)?) | slice;
    if (10 > 3) {
        t = (1, null, null) as (int, (int, int)?, (int?, int)?)
    }
    return t;
}

@method_id(120)
fun test20() {
    var a = getIntOrSlice();
    if (a is int) {
        __expect_type(a, "int");
        return a + 0;
    }
    __expect_type(a, "slice");
    return a.loadInt(32);
}

@method_id(121)
fun test21() {
    var a = getIntOrSlice();
    if (a is int) {
        if (a !is int8) {
            __expect_type(a, "int");
        }
        if (a is slice) {
            __expect_type(a, "never");
        }
        if (a !is slice) {
            __expect_type(a, "int");
        }
        if (a !is builder) {
            __expect_type(a, "int");
        }
    }
    if (a !is int && a !is builder && a !is int && a !is null) {
        __expect_type(a, "slice");
    }
    if (a !is int || (a !is int && a !is builder)) {
        __expect_type(a, "slice");
    }
    if (a is int || a is slice) {
        __expect_type(a, "int | slice");
    } else {
        __expect_type(a, "never");
    }
    if (a is slice || a is int) {
        __expect_type(a, "slice | int")
    } else {
        __expect_type(a, "never")
    }
    if (a is int || a is slice || a is builder) {
        __expect_type(a, "int | slice");
    }
    if (a !is int && a !is slice && a !is builder) {
        __expect_type(a, "never");
    }
    return (a is int, a !is int, a is slice, a !is slice, a is int || a is slice || a is builder, a is builder);
}

@method_id(122)
fun test22(a: int, b: int | slice) {
    var result = 0;
    if (a is int) { result += 1 }
    if (a is int || a is slice) { result += 2 }
    if (a is builder || a is slice) { result += 3 }
    if (b is int) { result += 10 }
    if ((b is int) | (b is slice)) { result += 11 }
    if ((b is builder) | (b is int)) { result += 12 }
    return result
}

@method_id(123)
fun test23(p2: bool) {
    var p: Pair2Or3 = p2 ? (1, 2) : (3, 4, 5);
    __expect_type(p, "Pair2Or3");
    if (p is (int, int) && 10 < 0) {
        __expect_type(p, "(int, int)");
    }
    __expect_type(p, "(int, int) | (int, int, int)");
    if (p is int) {
        __expect_type(p, "never");
        return (0, 0);
    }
    __expect_type(p, "(int, int) | (int, int, int)");
    if (p !is (int, int)) {
        __expect_type(p, "(int, int, int)");
        p = (10, 20);
    }
    __expect_type(p, "Pair2");
    return p;
}

fun eqUnion2<T1, T2>(v: T1 | T2): T2 | T1 { return v }

fun detectUnionSide<T1, T2>(v: T1 | T2) {
    if (v is T1) {
        assert(v is T1 && v !is T2 && v !is (T1, T2), 101);
        return 0;
    }
    assert(v is T2 && v !is T1 && !(v is null || v is never), 102);
    return 1;
}

@method_id(124)
fun test24() {
    var bc = beginCell() as builder | cell;
    var mi1 = 5 as MInt | int8;
    var mi2 = 5 as int8 | MInt;
    var mi3 = (5 as int8) as int8 | MInt;
    __expect_type(eqUnion2(bc), "cell | builder");
    __expect_type(eqUnion2(mi1), "int8 | MInt");
    return (
        detectUnionSide<int, slice>(5),
        detectUnionSide<slice, int>(5),
        detectUnionSide(getIntOrSlice()),
        detectUnionSide<builder, slice>((getIntOrSlice() is int) ? getSlice32() : beginCell()),
        detectUnionSide(bc),
        detectUnionSide(mi1),
        detectUnionSide(mi2),
        detectUnionSide(mi3)
    );
}

@method_id(125)
fun test25(a: int? | slice) {
    var result = 0;
    if (a is null) { result |= 0x0001; }
    if (a is int || a == null) { result |= 0x0002; }
    if ((a is null) | a is slice) { result |= 0x0004; }
    if (a is int) { result |= 0x0008; }
    if ((a is builder) | a is null) { result |= 0x0010; }
    if (a is int || a is builder) { result |= 0x0020; }
    if (a is int8 || a is null || a is int16) { result |= 0x0040; }
    return result;
}

@method_id(126)
fun test26() {
    var a = 1 as int? | slice;
    var result = 0;
    if (a is null) { result |= 0x0001; }
    if (a is int || a == null) { result |= 0x0002; }
    if ((a is null) | a is slice) { result |= 0x0004; }
    if (a is int) { result |= 0x0008; }
    if ((a is builder) | a == null) { result |= 0x0010; }
    if ((a is int) | a is builder) { result |= 0x0020; }
    if ((a is int8) | (a is int16) | (a is null)) { result |= 0x0040; }
    return result;
}

@method_id(127)
fun test27() {
    var a: (MInt, int) | slice = (1, 2);
    __expect_type(a, "(MInt, int)");
    a = (1 as MInt, 2 as MInt);
    __expect_type(a, "(MInt, int)");
    var b: IntOrSliceOrBuilder = 5 as (MInt | slice);
    return (a, b);
}

@method_id(128)
fun test28() {
    var t = null as int | (int, int)?;
    return (t, 777, 2 as int | (int, int)?, 777, (3, 4) as int | (int, int)?);
}

@method_id(129)
fun test29() {
    var t1 = (null as int?) as (int, int) | int | null;
    var t2 = (5 as int?)    as (int, int) | int | null;
    return (t1, t2);
}

fun get5OrNull(getNull: bool) { return getNull ? null : 5; }

@method_id(130)
fun test30(getNull: bool) {
    var t1 = get5OrNull(getNull)     as (int, int) | int | null;
    var t2 = (null as (int, int)?)   as (int, int) | int | null;
    var t3 = ((1, 2) as (int, int)?) as (int, int) | int | null;
    var t4 = ([6] as [int]?)         as (int, int) | [int] | null;
    var t5 = (null as [int]?)        as (int, int) | [int] | null;
    return (t1, 777, t2, 777, t3, 777, t4, 777, t5);
}

@method_id(131)
fun test31() {
    // all of them are smart casted (narrowed from a wide union type to a subtype)
    var t1: (int, int) | int | null = null;
    var t2: (int, int) | int | null = 5;
    var t3: (int, int) | int | null = (1, 2);
    var t4: (int, int) | int | null = null as int?;
    var t5: (int, int) | int | null = 5 as int?;
    var t6: (int, int) | int | null = null as (int, int)?;
    var t7: (int, int) | int | null = (1, 2) as (int, int)?;
    {
        __expect_type(t1, "null");
        __expect_type(t2, "int");
        __expect_type(t3, "(int, int)");
        __expect_type(t4, "int?");
        __expect_type(t6, "(int, int)?")
    }
    return (t1, 777, t2, 777, t3, 777, t4, 777, t5, 777, t6, 777, t7);
}

@method_id(132)
fun test32() {
    var t1 = 5            as (int, slice?, int) | (int, int) | int | int8 | null;
    var t2 = (5 as int8)  as (int, slice?, int) | (int, int) | int | int8 | null;
    var t3 = (4, 5)       as (int, slice?, int) | (int, int) | int | int8 | null;
    var t4 = null         as (int, slice?, int) | (int, int) | int | int8 | null;
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(133)
fun test33() {
    var t1 = (5 as (int, MInt) | int)            as (int, slice?, int) | (int, int)  | MInt | int8 | null;
    var t2 = ((5 as int8) as null | int8 | int)  as (int, slice?, int) | (MInt, int) | int  | int8 | null;
    var t3 = ((4, 5) as (int, MInt) | MInt)      as (int, slice?, int) | (MInt, int) | int | int8  | null;
    var t4 = (null as MInt | int8 | null)        as (int, slice?, int) | (int, MInt) | int | int8  | null;
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(134)
fun test34() {
    // all of them are smart casted (narrowed from a wide union type to a subtype)
    var t1: (int, cell, slice?, int) | (int, int)   | int  | int8 | null = 5 as (int, int) | int;
    var t2: (int, cell, slice?, int) | (int, MInt)  | int  | int8 | null = 5 as (int, int) | MInt | int8;
    var t3: (int, cell, slice?, int) | (int, int)   | MInt | int8 | null = (1, 2) as (int, int) | MInt;
    var t4: (int, cell, slice?, int) | (MInt, MInt) | int  | int8 | null = (1, 2) as (int, int) | int | null;
    var t5: (int, cell, slice?, int) | (int, int)   | int  | int8 | null = (5 as int8) as (int, MInt) | int | int8;
    var t6: (int, cell, slice?, int) | (MInt, int)  | MInt | int8 | null = (5 as int8) as int8 | int;
    var t7: (int, cell, slice?, int) | (MInt, MInt) | int  | int8 | null = (5 as int8) as int8 | MInt | null;
    __expect_type(t1, "(int, int) | int");
    __expect_type(t7, "int | int8 | null");
    return (t1, 777, t2, 777, t3, 777, t4, 777, t5, 777, t6, 777, t7);
}

@method_id(135)
fun test35() {
    var t1 = 5 as int | (int, MInt) | null;
    var t2 = (1, 2) as int | (int, int) | (int, MInt, slice, slice);
    var t3 = (1, 2) as int | null | (MInt, int) | (MInt, int, slice, slice);
    var t4 = null as int? | (MInt, MInt)?;
    __expect_type(t1!, "int | (int, MInt)");
    __expect_type(t2!, "int | (int, int) | (int, MInt, slice, slice)");
    __expect_type(t3!, "int | (MInt, int) | (MInt, int, slice, slice)");
    __expect_type(t4!, "int | (MInt, MInt)");
    return (t1!, 777, t2!, 777, t3!, 777, t4!);
}

@method_id(136)
fun test36() {
    var t1 = (1 as int8, 2 as int16) as (int, int) | slice;
    var t2 = (1 as int8, 2 as int16) as (int, MInt) | (int8, int16) | slice;
    var t3 = (1 as int8, 2 as int16) as (int?, MInt?) | builder | null;
    var t4 = (1 as int8, 2 as int16) as (int8?, int16) | (int, int, int) | (cell, cell, cell);
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(137)
fun test37(one: int8, two: int16) {
    var t1: (int, MInt) | slice = (one, two);
    var t2: (int, int) | (int8, int16) | slice = (one, two);
    var t3: (int?, int?) | builder | null = (one, two);
    var t4: [int8?, int16] | (int, int, int) | (cell, cell, cell) = [one, two];
    __expect_type(t1, "(int, MInt)");
    __expect_type(t2, "(int8, int16)");
    __expect_type(t3, "(int?, int?)");
    __expect_type(t4, "[int8?, int16]");
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(138)
fun test38() {
    var t1 = (1, null, 2) as (int, (int, MInt, int, int)?, int) | slice;
    var t2 = (1, null, 2) as (int, MInt | slice | null, int) | slice;
    var t3 = (1, 5, 2)    as (int, int  | slice | null, int) | slice;
    var t4 = (1, 5, 2)    as (int, int  | slice | (int, int), int) | slice;
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(139)
fun test39() {
    var t1: (int, (MInt, int, int, MInt)?, int) | slice = (1, null, 2);
    var t2: (int, int | slice | null, int) | slice = (1, null, 2);
    var t3: (int, MInt | slice | null, int) | slice = (1, 5, 2);
    var t4: (int, int | slice | (int, int), int) | slice = (1, 5, 2);
    __expect_type(t1, "(int, (MInt, int, int, MInt)?, int)");
    __expect_type(t3, "(int, MInt | slice | null, int)");
    return (t1, 777, t2, 777, t3, 777, t4);
}

@method_id(140)
fun test40(x: int): SomeIntBits {
    if (x < 10) { return x as int8 }
    if (x < 20) { return x as int9 }
    if (x < 30) { return x as int10 }
    return x as uint64;
}

@method_id(141)
fun test41(x: int?, y: MInt | MNull) {
    var t1 = x as int | MNull;
    var t2 = x as int | slice | MNull;
    var t3 = y as int?;
    var t4 = y as int? | slice;
    return (t1, t2, t3, t4);
}

@method_id(142)
fun test42(x: int?, y: MInt | MNull) {
    var t1: int | MNull = x;
    var t2: int | slice | MNull = x;
    var t3: int? = y;
    var t4: int? | slice = y;
    __expect_type(t1, "int?");
    __expect_type(t2, "int?");
    __expect_type(t3, "int?");
    __expect_type(t4, "int?");
    return (t1, t2, t3, t4);
}

@method_id(143)
fun test43() {
    var a: MNull = null;
    __expect_type(a, "null");
    return (a as null, a as int?, a as MIntN, a as (int, int)?, a as (int, int) | slice | MNull);
}

@method_id(144)
fun test44(a: int) {
    return (a is slice, a is int, a is null, a != null, a is builder, a !is (int, int), a is int8);
}

@method_id(145)
fun test45(a: slice?) {
    return (a is slice, a is null, a != null, a is builder, a !is (int, int), a is int);
}

@method_id(146)
fun test46(a: (int, int) | int) {
    return (a is slice, a is null, a != null, a is builder, a !is (int, int), a is int && a == 0);
}

fun checkSimple(a: int | slice | builder) {
    return match (a) {
        int => 1,
        slice => 2,
        builder => 3,
    }
}

@method_id(147)
fun test47() {
    __expect_type(checkSimple, "(int | slice | builder) -> int");
    return (checkSimple(1), checkSimple(beginCell().endCell().beginParse()), checkSimple(100), checkSimple(beginCell()));
}

fun checkGeneric3<T1, T2, T3>(a: T1 | T2 | T3): (int, T1 | T2?) {
    var vv: T1? = match (a) { T1 => a, T2 => null, T3 => null };
    return match (a) {
        T1 => (1, a),
        T2 => (2, match(a) { T2 => a }),
        T3 => (3, null),
    }
}

type IntOrInt8OrInt16 = int | int8 | int16;

@method_id(148)
fun test48(a: IntOrInt8OrInt16) {
    var b = a;
    __expect_type(b, "IntOrInt8OrInt16");
    match (b) { MInt => {} int8 => {} int16 => {} }
    __expect_type(b, "int | int8 | int16");
    match (b) { int => {}, int8 => { return ((0, null), (0, null)); }, int16 => "" };
    __expect_type(b, "int | int16");
    return (checkGeneric3(a), checkGeneric3(5 as int | slice | builder));
}

@method_id(149)
fun test49(a: slice? | int) {
    if (a !is int) {
        return match (a) {
            slice => 1,
            null => 2,
        }
    }
    return a + 0
}

type VeryComplexType = int | (MInt, int)? | builder? | int;

@method_id(150)
fun test50() {
    var a = 5 as int | slice | null;
    var b = 5 as VeryComplexType;
    test49(a);
    test49(match (a) { int => a, slice => a, null => a, });
    __expect_type(b, "VeryComplexType");
    match (b) { int => {} Pair2 => {} builder => {} null => {} }
    __expect_type(b, "int | (int, int) | builder | null");
    __expect_type(b!, "int | (int, int) | builder");
    match (b) { int => 100, Pair2 => {} builder => return -2, null => "null" }
    __expect_type(b, "int | (int, int) | null");
    if (b !is null) {
        match (b) { int => {}, Pair2 => {} }
    } else {
        match (b) { null => {} }
    }
    match (b is null) {
        bool => {}
    }
    match (b = 5 as VeryComplexType) { int => b, (int, int) => { return -1; } null => {} builder => return -2 }
    __expect_type(b, "int?");
    b is int && b!is int && b!!is int;
    if (match(b) { int => 1, null => null } !is int) {
        return 100
    }
    return (b! + test49(match (a) { int => a, slice => a, null => a } as int|slice?));
}

@method_id(151)
fun test51(init: int) {
    var a = init as int | slice;
    match(a){}
    __expect_type(a, "int | slice");
    match (a) {
        int => {
            a = init;
            match (a) {};
            __expect_type(a, "int");
            a = a > 10 ? beginCell().storeInt(init, 32).endCell().beginParse() : 10;
            __expect_type(a, "int | slice");
            if (a is slice) {
                a = a.loadInt(32)
            }
        }
        slice => {
            if (a !is int) {
                a = a.loadInt(32) as int8;
            } 
        }
    }
    __expect_type(a, "int");
    return a;
}

@method_id(152)
fun test52(init: int): int | builder {
    var m = get5OrNull(false);
    if (init > 10 && init < 15) {
        return init > 20 ? init : beginCell();
    }
    return match (init as int?) {
        int => init,
        null => beginCell()
    }
}

fun inc53(mutate x: int) {
    x += 1;
    return x;
}

@method_id(153)
fun test53(a: int | builder) {
    match (var a = 5 as int | slice) {
        int => {}
        slice => { __expect_type(a, "slice"); var b: slice? = a; return b.loadInt(32); }
    }
    match (val a = getIntOrSlice()) {
        int => a + 0,
        slice => {}
    }
    __expect_type(a, "int | builder");
    var cc = match (var a = getIntOrSlice()) {
        int => inc53(mutate a),
        slice => { return 777; }
    };
    __expect_type(cc, "int");
    match (var (a, (b, c)) = (10, (20, 30))) {
        (int, Pair2) => {}
    }
    return cc
}

@method_id(154)
fun test54(): int|slice {
    match (var a = getIntOrSlice()) {
        int => {
            __expect_type(a, "int");
            a = beginCell().storeInt(100, 32).endCell().beginParse();
            __expect_type(a, "slice");
            if (a.remainingBitsCount() < 100) { a = a.loadInt(32); }
            else { a = 0; }
            __expect_type(a, "int");
            return a;
        }
        slice => throw 123
    }
    return 777;
}

@method_id(155)
fun test55() {
    var a = getIntOrSlice() as int | slice | null;
    var cc = match (a) {
        int => a = getIntOrSlice(),
        slice => return (-10, 0),
        null => throw 123
    };
    __expect_type(a, "int | slice");
    return (a, cc);
}

@method_id(156)
fun test56(a: int8, b: int16, useSecond: bool) {
    var r1: int8 | int16 = useSecond ? b: a;
    var r2: int          = useSecond ? b: a;
    var r3: int8 | (int, int) | int16? = useSecond ? b : a;
    var r4               = (useSecond ? b : a) as int;
    var r5               = (useSecond ? b : a) as int8? | int16;
    var r6: int8 | int16 = match (val c: int16 | int8 | int8 = useSecond ? b : a) { int8 => c, int16 => c };
    var r7               = match (val c: int16 | int8 = useSecond ? b : a) { int8 => c, int16 => c } as int;
    var r8               = match (val c: int16 | int8 = useSecond ? b : a) { int8 => c as int, int16 => c as int };
    var r9: int          = match (val c = (useSecond ? b : a) as IntOrInt8OrInt16) { int8 => c, int16 => c, int => c };
    return (r1, r2, r3, r4, r5, r6, r7, r8, r9);
}

@method_id(157)
fun test57(r3v: int) {
    var r1 = null as int | () | null;
    var r2 = ()   as int | () | null;
    var r3 = ()   as (int, int) | () | null;
    var r4 = null as (int, int) | () | null;
    if (r3v == 1) {
        r3 = (1, 2);
    }
    return (r1, r2, 777, r3, 777, r4 = r3v == 1 ? () : null, r4, 777, r1 is int, r1 is (), r1 is null, r3 is int, r3 is (int,int), r3 is (), r3 == null, 777, r4 is (), r4 is null);
}

@method_id(158)
fun test58() {
    var tn = () as ()?;
    var aln: null = (tn = null);
    __expect_type(tn, "null");
    return ((()) as () | (int, int) | null, (() as () | null) as () | (int, int) | null, tn, tn as ()?, aln);
}

fun alwaysThrows123(): never { throw 123; }

@method_id(159)
fun test59(a: int | slice | builder | null) {
    try {
        var b = 0;
        var c = match (a) {
            int => b + a,
            slice => {
                b = 10;
                throw 456;
            }
            builder => alwaysThrows123(),
            null => {
                alwaysThrows123();
            }
        };
        __expect_type(c, "int");
        return b + c;
    } catch (excCode) {
        return excCode;
    }
}

fun takeIntReturnVoid(a: int) { }

fun returnVoidOrNull(willBeVoid: bool) {
    var (i: int?, _: builder) = (willBeVoid ? 10 : null, beginCell());

    var res = i != null ? takeIntReturnVoid(i) : null;
    __expect_type(res, "void?");
    return res;
}

@method_id(160)
fun test60() {
    return (returnVoidOrNull(true), returnVoidOrNull(false));
}

@method_id(161)
fun test61(i: int32 | int64 | int) {
    match (i) {
        int32 => { return 10; }
        int64 => { return 20; }
        int => { return 30; }
    }
}



fun main() {
    return 0;
}

/**
@testcase | 0   |           | 0
@testcase | 101 |           | 5 1 5 1 5 1
@testcase | 102 |           | 32
@testcase | 103 | 1         | 10 1
@testcase | 103 | 2         | (null) 0
@testcase | 104 |           | 5 5 -1 5 1 5 1
@testcase | 105 |           | 5 5 1 5 1 5
@testcase | 106 | 1         | (null) 0
@testcase | 106 | 2         | 2 1
@testcase | 107 | 1         | (null) 2 3 129 (null) 2 3 129
@testcase | 107 | 3         | 6 7 8 130 6 7 8 130
@testcase | 108 |           | (null) 2 3 129 (null) (null) (null) 0 (null) 2 3 129 (null) 2 3 129 (null) (null) (null) 0
@testcase | 109 |           | 6 7 8 130 0 (null) -1 (null) (null) (null) 0 -1 (null) (null) (null) 0
@testcase | 110 |           | 1 (null) (null) 0 (null) (null) 0 135
@testcase | 120 |           | 5
@testcase | 121 |           | -1 0 0 -1 -1 0
@testcase | 122 | 0 0 1     | 36
@testcase | 122 | 0 0 4     | 14
@testcase | 123 | 1         | 1 2
@testcase | 123 | 0         | 10 20
@testcase | 124 |           | 0 1 0 1 0 0 1 0
@testcase | 125 | null 0    | 87
@testcase | 125 | 1 1       | 42
@testcase | 126 |           | 42
@testcase | 127 |           | 1 2 5 1
@testcase | 128 |           | (null) (null) 0 777 (null) 2 1 777 3 4 129
@testcase | 129 |           | (null) (null) 0 (null) 5 1
@testcase | 130 | 0         | (null) 5 1 777 (null) (null) 0 777 1 2 129 777 (null) [ 6 ] 136 777 (null) (null) 0
@testcase | 130 | -1        | (null) (null) 0 777 (null) (null) 0 777 1 2 129 777 (null) [ 6 ] 136 777 (null) (null) 0
@testcase | 131 |           | (null) 777 5 777 1 2 777 (null) 777 5 777 (null) (null) 0 777 1 2 129
@testcase | 132 |           | (null) (null) 5 1 777 (null) (null) 5 42 777 (null) 4 5 129 777 (null) (null) (null) 0
@testcase | 133 |           | (null) (null) 5 1 777 (null) (null) 5 42 777 (null) 4 5 129 777 (null) (null) (null) 0
@testcase | 134 |           | (null) 5 1 777 (null) 5 1 777 1 2 129 777 1 2 129 777 (null) 5 42 777 5 42 777 5 42
@testcase | 135 |           | (null) 5 1 777 (null) (null) 1 2 129 777 (null) (null) 1 2 129 777 (null) (null) 0
@testcase | 136 |           | 1 2 129 777 1 2 140 777 1 2 141 777 (null) 1 2 142
@testcase | 137 | 1 2       | 1 2 777 1 2 777 1 2 777 [ 1 2 ]
@testcase | 138 |           | 1 (null) (null) (null) (null) 0 2 146 777 1 (null) 0 2 147 777 1 5 1 2 147 777 1 (null) 5 1 2 148
@testcase | 139 |           | 1 (null) (null) (null) (null) 0 2 777 1 (null) 0 2 777 1 5 1 2 777 1 (null) 5 1 2
@testcase | 140 | 5         | 5 42
@testcase | 140 | 15        | 15 131
@testcase | 140 | 90        | 90 49
@testcase | 141 | 7 8       | 7 7 1 8 8 1
@testcase | 141 | null null | (null) (null) 0 (null) (null) 0
@testcase | 142 | 7 8       | 7 7 8 8
@testcase | 142 | null null | (null) (null) (null) (null)
@testcase | 143 |           | (null) (null) (null) (null) (null) 0 (null) (null) 0
@testcase | 144 | 0         | 0 -1 0 -1 0 -1 0
@testcase | 145 | null      | 0 -1 0 0 -1 0
@testcase | 146 | null 0 1  | 0 0 -1 0 -1 -1
@testcase | 147 |           | 1 2 1 3
@testcase | 148 | 9 1       | 1 9 1 1 5 1
@testcase | 148 | 9 44      | 3 (null) 0 1 5 1
@testcase | 149 | null 0    | 2
@testcase | 150 |           | 10
@testcase | 151 | 88        | 88
@testcase | 152 | 52        | 52 1
@testcase | 153 | 1 1       | 6
@testcase | 154 |           | 100 1
@testcase | 155 |           | 5 1 5 1
@testcase | 156 | 1 2 -1    | 2 44 2 2 44 2 2 44 2 44 2 2 2
@testcase | 157 | 1         | (null) 0 (null) 149 777 1 2 129 777 149 149 777 0 0 -1 0 -1 0 0 777 -1 0
@testcase | 157 | 0         | (null) 0 (null) 149 777 (null) (null) 149 777 0 0 777 0 0 -1 0 0 -1 0 777 0 -1
@testcase | 158 |           | (null) (null) 149 (null) (null) 149 (null) 0 (null)
@testcase | 159 | 0 4       | 456
@testcase | 159 | null 0    | 123
@testcase | 160 |           | 10 0
@testcase | 161 | 100 48    | 20


@fif_codegen
"""
  test26 PROC:<{
    42 PUSHINT  // result
  }>
"""

@fif_codegen
"""
  checkSimple PROC:<{   //  a.USlot1 a.UTag
    NIP	    //  a.UTag
    DUP	    //  a.UTag a.UTag
    1 EQINT	//  a.UTag '3
    IF:<{	//  a.UTag
      DROP	//
      1 PUSHINT	//  '2=1
    }>ELSE<{	//  a.UTag
      4 EQINT	//  '6
      IF:<{	//
        2 PUSHINT	//  '2=2
      }>ELSE<{	//
        3 PUSHINT	//  '2=3
      }>
    }>
  }>
"""

@fif_codegen
"""
  test61 PROC:<{
    NIP
    DUP
    46 EQINT
    IFJMP:<{
      DROP
      10 PUSHINT
    }>
    48 EQINT
    IFJMP:<{
      20 PUSHINT
    }>
    30 PUSHINT
  }>
"""

@fif_codegen DECLPROC checkGeneric3<int,int8,int16>

 */
