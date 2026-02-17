
struct JustInt32 {
    v: MInt32
}

struct JustUint64 {
    v: uint64
}

struct JustAddress {
    v: address
}

type MInt32 = int32
type MSlice = slice

struct Point {
    x: int8
    y: int8
}

struct Point3d {
    x: int8
    y: int8
    z: int8
}

struct DataWithCell {
    a: address?
    v: coins
    p: Cell<Point>?
    payload: cell? = null
}

enum HashMode: uint16 {
    NoHash = 0,
    PartialHash = 80,
    TotalHash = 255,
}

fun map<K, V>.calculateSize(self) {
    var size = 0;
    var r = self.findFirst();
    while (r.isFound) {
        size += 1;
        r = self.iterateNext(r);
    }
    return size;
}


@method_id(101)
fun test1() {
    var m: map<int8, int32> = createEmptyMap();
    m.set(1, 10);
    return m.exists(1);
}

@method_id(102)
fun test2(k1: int, v1: int, k2: int, v2: int) {
    var m: map<int8, int32> = createMapFromLowLevelDict(null);
    m.set(k1, v1);
    m.set(k2, v2);
    return (m.exists(1), m.exists(k1), m.mustGet(k2));
}

@method_id(103)
fun test3(v1: int) {
    var m = createEmptyMap<int8, (int32, int64)>();
    m.set(1, (1, 2));
    m.set(2, (3, v1));
    m.delete(1);
    return (m.exists(1), m.mustGet(2));
}

@method_id(104)
fun test4() {
    var m = createEmptyMap<int8, int8>();
    m.set(1, 10);
    m.set(2, 20);
    m.set(3, 30);
    val g1 = m.get(1);
    val g4 = m.get(4);
    return (g1.isFound, g4.isFound, g1.loadValue());
}

@method_id(105)
fun test5() {
    var m = createEmptyMap<int8, int8>();
    m.set(1, 10);
    m.set(2, 20);
    m.set(3, 30);
    var t = createEmptyTuple();
    var r = m.findFirst();
    while (r.isFound) {
        t.push(r.getKey());
        t.push(r.loadValue());
        r = m.iterateNext(r);
    }
    r = m.findLast();
    while (r.isFound) {
        t.push(r.loadValue());
        r = m.iterateNext(r);
    }
    r = m.findLast();
    while (r.isFound) {
        t.push(r.loadValue());
        r = m.iteratePrev(r);
    }
    return t;
}

@method_id(106)
fun test6() {
    var m = createEmptyMap<int8, int8>();
    m.set(1, 10);
    m.set(2, 20);
    m.set(3, 30);
    return (
        m.findKeyGreater(1).getKey(), m.findKeyGreaterOrEqual(1).getKey(), 
        m.findKeyLess(3).getKey(), m.findKeyLessOrEqual(3).getKey(),
        m.findKeyLess(3).loadValue(), m.findKeyLessOrEqual(3).loadValue(),
        m.findKeyLessOrEqual(3).isFound, m.findKeyGreater(4).isFound,
    )
}

@method_id(107)
fun test7() {
    var m1 = createEmptyMap<int8, bool>();
    m1.addIfNotExists(1, true);
    m1.addIfNotExists(1, false);
    m1.replaceIfExists(2, true);
    return (m1.mustGet(1), m1.replaceIfExists(2, false), m1.get(2).isFound);
}

@method_id(108)
fun test8() {
    var (a, m: map<int8, address?>, m2: map<int8, any_address>) = (1, createEmptyMap(), createEmptyMap());
    val prev1 = m.setAndGetPrevious(a, null);
    val prev2 = m.setAndGetPrevious(a, address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5"));
    val prev3 = m2.setAndGetPrevious(a, createAddressNone());
    val prev4 = m2.setAndGetPrevious(a, address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5") as any_address);
    return (
        prev1.isFound, prev2.isFound, prev2.loadValue() == null, prev2.loadValue() == null, m.get(a).loadValue()!.getWorkchain(),
        prev3.isFound, prev4.isFound, prev4.loadValue().isNone(), prev4.loadValue().isNone(), m2.get(a).loadValue()!.getWorkchain(),
    );
}

@method_id(109)
fun test9() {
    var m = createEmptyMap<int8, (coins, ())>();
    m.set(1, (ton("1"), ()));
    val replaced = m.replaceIfExists(1, (ton("0.05"), ()));
    val prev = m.replaceAndGetPrevious(1, (ton("0.01"), ()));
    return (replaced, prev.loadValue(), m.replaceAndGetPrevious(2, (0,())), m.mustGet(1).0);
}

@method_id(110)
fun test10() {
    var m = createEmptyMap<uint128, int32>();
    val added1 = m.addIfNotExists(1<<100, 1<<10);
    val added2 = m.addIfNotExists(1<<100, 2<<10);
    val b1 = m.addOrGetExisting(2<<100, 20);
    val b2 = m.addOrGetExisting(2<<100, 30);
    return (added1, added2, m.mustGet(1<<100), b1.isFound, b2.isFound, b2.loadValue());
}

@method_id(111)
fun test11() {
    var m: map<uint2, int8?>;
    m = createEmptyMap();
    m.set(0, null);
    m.set(1, 1);
    m.set(2, null);
    return (m.exists(0), m.exists(1), m.mustGet(0), m.mustGet(1), m.get(0).isFound, m.get(0).loadValue(), m.get(3));
}

@method_id(112)
fun test12() {
    var m1: map<int8, int32> = createEmptyMap();
    val s1 = m1.calculateSize();
    m1.set(1, 10);
    m1.addIfNotExists(1, -100);
    val s2 = m1.calculateSize();
    var m2 = m1;
    m2.addIfNotExists(2, 20);
    m2.addIfNotExists(3, 30);
    var m3 = m2;
    val wasDeleted = m2.delete(2);
    var del1 = m1.deleteAndGetDeleted(-100);
    var del2 = m2.deleteAndGetDeleted(2);
    var del3 = m2.deleteAndGetDeleted(3);
    return (
        s1, s2, m3.calculateSize(),
        wasDeleted, del1.isFound, del2.isFound, del3.isFound,
        del3.loadValue(),
    );
}

@method_id(113)
fun test13(m: map<JustInt32, int8>) {
    // key remains just int at the TVM level, no runtime packing, checked via codegen
    return m.get({v: 123})
}

@method_id(114)
fun test14(fromV: int) {
    var m = createEmptyMap<JustUint64, int8>();
    m.set({v: 1}, 10);
    m.set({v: 2}, 20);
    m.set({v: 3}, 30);
    return m.findKeyGreater({v: fromV}).getKey()
}

@method_id(115)
fun test15() {
    var m = createEmptyMap<JustAddress, int8>();
    m.set({v: address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA")}, 10);
    m.set({v: address("Ef9LynHHKgBxY6-l-W_dWN-CtGT2_ji5rN3EzOI-p9zWEfq6")}, 20);
    m.set({v: address("Ef9hMd78gzSiVsK0zz0AHtEja8x1UoB_NDZMjn-l86NQK_2Y")}, 30);
    return m.mustGet({v:address("Ef9LynHHKgBxY6-l-W_dWN-CtGT2_ji5rN3EzOI-p9zWEfq6")});
}

@method_id(116)
fun test16() {
    var m = createEmptyMap<MInt32, Point>();
    m.set(100, {x: 10, y: 20});
    return (m.deleteAndGetDeleted(100).loadValue(), m.calculateSize(), m.isEmpty());
}

@method_id(117)
fun test17() {
    var m = createEmptyMap<bool, MInt32>();
    m.set(true, 123);
    return m.findFirst().getKey();
}

@method_id(118)
fun test18() {
    var m: map<Point, Point> = createEmptyMap();
    m.set({x: 1, y: 1}, {x: 10, y: 10});
    m.addIfNotExists({x: 1, y: 1}, {x: -100, y: -100});
    val f1 = m.get({x: 1, y: 1});
    m.replaceIfExists({x: 1, y: 1}, {x: 20, y: 20});
    val f2 = m.get({x: 1, y: 1});
    val deleted1 = m.delete({x: 1, y: 1});
    val deleted2 = m.delete({x: 1, y: 1});
    val f3 = m.get({x: 1, y: 1});
    return (m, f1.isFound, f2.isFound, f3.isFound, f1.loadValue(), f2.loadValue(), deleted1, deleted2, m.isEmpty());
}

@method_id(119)
fun test19() {
    var m: map<address, Cell<Point>> = createEmptyMap();
    m.addIfNotExists(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"), Point{x:10,y:10}.toCell());
    return m.get(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA")).loadValue().load();
}

@method_id(120)
fun test20() {
    var m: map<address, Cell<Point>> = createEmptyMap();
    m.addIfNotExists(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"), Point{x:10,y:10}.toCell());
    return m.mustGet(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA")).load();
}

@method_id(121)
fun test21() {
    var m: map<address, Cell<Point>> = createEmptyMap();
    val r1 = m.addOrGetExisting(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"), Point{x:10,y:10}.toCell());
    val r2 = m.addOrGetExisting(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"), Point{x:20,y:20}.toCell());
    val d1 = m.deleteAndGetDeleted(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"));
    val d2 = m.deleteAndGetDeleted(address("Ef9rU-_AAnBkHB71TIC3QvUf5LcAsvj0B4IoYzAXLpEFd5CA"));
    return (r1.isFound, r2.isFound, r2.loadValue().load(), d1.isFound, d2.isFound);
}

@method_id(122)
fun test22() {
    var m: map<int8, Cell<Point>> = createEmptyMap();
    m.set(3, Point{x:30,y:30}.toCell());
    m.set(2, Point{x:20,y:20}.toCell());
    m.set(1, Point{x:10,y:10}.toCell());
    return (m.findFirst().getKey(), m.findLast().getKey(), m.findKeyGreater(2).getKey(), m.findKeyGreaterOrEqual(2).loadValue().load(), m.mustGet(2).load().x);
}

@method_id(123)
fun test23() {
    var m: map<Point, DataWithCell> = createEmptyMap();
    val p1 = m.setAndGetPrevious({x:1,y:1}, {a: null, v: ton("0.05"), p: Point{x:10,y:10}.toCell(), payload: null});
    val p2 = m.setAndGetPrevious({x:1,y:1}, {a: null, v: ton("0.1"), p: null, payload: createEmptyCell()});
    return (p1.isFound, p2.isFound, p2.loadValue().p!.load(), m.mustGet({x:1,y:1}).a == null, m.get({x:1,y:1}).loadValue().payload.depth());
}

@method_id(124)
fun test24() {
    var m = createEmptyMap<Point, DataWithCell>();
    val p1 = m.deleteAndGetDeleted({x: 1, y: 1});
    m.set({x: 3, y: 3}, {a: null, v: ton("0.05"), p: Point{x:30,y:30}.toCell(), payload: createEmptyCell()});
    m.set({x: 1, y: 1}, {a: null, v: ton("0.05"), p: Point{x:10,y:10}.toCell(), payload: null});
    m.set({x: 2, y: 2}, {a: null, v: ton("0.05"), p: null, payload: null});
    val k2 = m.findKeyGreaterOrEqual({x: 2, y: 2}).getKey();
    var r = m.findFirst();
    var t = createEmptyTuple();
    while (r.isFound) {
        t.push(r.getKey().x);
        t.push(r.getKey().y);
        t.push(r.loadValue().payload == null);
        r = m.iterateNext(r);
    }
    return (p1, k2, t);
}

@method_id(125)
fun test25() {
    var m = createEmptyMap<int8, slice>();
    m.set(1, stringHexToSlice("0101"));
    return m;
}

@method_id(126)
fun test26() {
    var m = createEmptyMap<int8, slice>();
    m.set(1, stringHexToSlice("0101"));
    val f1 = m.setAndGetPrevious(1, stringHexToSlice("010101"));
    val f2 = m.deleteAndGetDeleted(1);
    val f3 = m.setAndGetPrevious(1, stringHexToSlice("01010101"));
    return (
        f1.isFound, f2.isFound, f3.isFound, 
        f1.loadValue().remainingBitsCount(), f2.loadValue().remainingBitsCount(),
        m.mustGet(1).remainingBitsCount(),
    );
}

@method_id(127)
fun test27() {
    var m = createEmptyMap<int8, slice>();
    m.set(1, "");
    return (m.findFirst().getKey(), m.findFirst().loadValue().remainingBitsCount());
}

@method_id(128)
fun test28() {
    var m = createEmptyMap<Point, MSlice>();
    m.set({x: 1, y: 1}, stringHexToSlice("01"));
    m.set({x: 3, y: 3}, stringHexToSlice("03"));
    m.set({x: 2, y: 2}, stringHexToSlice("02"));
    var r = m.findKeyLessOrEqual({x: 4, y: 0});
    var sum = 0;
    while (r.isFound) {
        sum += r.loadValue().loadUint(8);
        r = m.iteratePrev(r);
    }
    return (sum, m.calculateSize());
}

@method_id(129)
fun test29() {
    var m = createEmptyMap<int32, slice>();
    val a1 = m.addIfNotExists(1, DataWithCell{a: null, v: 0, p: null, payload: null}.toCell().beginParse());
    val a2 = m.addIfNotExists(2, DataWithCell{a: null, v: 0, p: Point{x:20,y:20}.toCell(), payload: null}.toCell().beginParse());
    val a3 = m.addIfNotExists(3, DataWithCell{a: null, v: 0, p: Point{x:30,y:30}.toCell(), payload: createEmptyCell()}.toCell().beginParse());
    val r4 = m.replaceAndGetPrevious(4, "");
    return (
        a1, a2, a3, r4, 
        DataWithCell.fromSlice(m.mustGet(2)).payload, 
        m.get(3).loadValue().loadAny<DataWithCell>().p!.load(),
        m.get(3).loadValue().remainingRefsCount(),
        m.findLast().getKey(),
        m.findLast().loadValue().remainingRefsCount(),
    );
}

@method_id(130)
fun test30() {
    var m = createEmptyMap<int8, cell>();
    m.set(1, createEmptyCell());
    m.set(2, Point{x:20,y:20}.toCell());
    m.set(3, Point{x:30,y:30}.toCell());
    val c3 = m.get(3);
    var r = m.findKeyLess(3);
    var t = createEmptyTuple();
    while (r.isFound) {
        t.push(r.loadValue().beginParse().remainingBitsCount());
        r = m.iteratePrev(r);
    }
    val p2 = lazy Point.fromCell(m.mustGet(2));
    return (c3.isFound, c3.loadValue().beginParse().loadAny<Point>(), t, p2.y);
}

@method_id(131)
fun test31() {
    var m = createEmptyMap<uint4, cell?>();
    m.set(1, createEmptyCell());
    m.set(2, Point{x:20,y:20}.toCell());
    m.set(3, Point{x:30,y:30}.toCell());
    m.set(4, null);
    val c4 = m.get(4);
    val c5 = m.get(5);
    var r = m.findLast();
    var t = createEmptyTuple();
    while (r.isFound) {
        t.push(r.loadValue() == null);
        r = m.iteratePrev(r);
    }
    return (m.mustGet(4), c4.loadValue(), c5.isFound, t);
}

@method_id(132)
fun test32() {
    var m = createEmptyMap<bits16, bits16>();
    m.set(stringHexToSlice("0102") as bits16, stringHexToSlice("aaaa") as bits16);
    m.set(stringHexToSlice("0304") as bits16, stringHexToSlice("bbbb") as bits16);
    val repl = m.replaceAndGetPrevious(stringHexToSlice("0304") as bits16, stringHexToSlice("0000") as bits16);
    var r = m.iteratePrev(m.findLast());
    return ((r.getKey() as slice).remainingBitsCount(), (r.getKey() as slice).loadAny<Point>(), (r.loadValue() as slice).remainingBitsCount(), repl.isFound, (repl.loadValue() as slice).loadUint(8));
}

@method_id(133)
fun test33() {
    var m = createEmptyMap<uint8, Point>();
    m.set(123, {x: 10, y: 10});
    return m.mustGet(123);
}

@method_id(134)
fun test34(z: int) {
    var m = createEmptyMap<uint32, Point3d>();
    m.set(123, {x: 10, y: 10, z});
    return [m.mustGet(123).z, m.addIfNotExists(456, {x: 10, y: z, z}), m.mustGet(456).y];
}

@method_id(135)
fun test35() {
    var m: map<int8, map<int8, int32>> = createEmptyMap();
    m.set(1, createEmptyMap());
    val f1 = m.mustGet(1).findFirst();
    assert (!f1.isFound) throw 123;
    m.get(1).loadValue().set(10, 100);
    assert (m.mustGet(1).isEmpty()) throw 123;      // the previous set() changed a copy
    
    var nested = createEmptyMap<int8, int32>();
    nested.set(1,10).set(2, 20);
    m.replaceIfExists(1, nested);
    nested.set(3, 30);
    assert (nested.calculateSize() == 3) throw 123;
    assert (m.mustGet(1).calculateSize() == 2) throw 123;
    
    val (k1, v2) = (m.mustGet(1).findFirst().getKey(), m.get(1).loadValue().findLast().loadValue());
    val s1 = m.findFirst().loadValue().calculateSize();
    m.mustGet(1).delete(10);
    val d2 = m.mustGet(1).deleteAndGetDeleted(2);
    return (f1.isFound, k1, v2, s1, d2.loadValue(), m.mustGet(1).isEmpty(), m.get(1).loadValue().calculateSize());
}

@method_id(136)
fun test36() {
    val k = address("Ef_vA6yRfmt2P4UHnxlrQUZFcBnKux8mL2eMqBgpeMFPorr4");
    var m = createEmptyMap<address, int32>();
    m.set(k, 10);
    return m.mustGet(k);
}

@method_id(137)
fun test37() {
    try {
        var m = createEmptyMap<int32, int64>();
        m.set(1, 10);
        return m.mustGet(555) as int;
    } catch (ex) {
        return ex;
    }
}

@method_id(138)
fun test38(k: int) {
    try {
        var m = createEmptyMap<int32, int64>();
        m.set(1, 10);
        return m.mustGet(k, 55);
    } catch (ex) {
        return ex as int64;
    }
}

type Set<T> = map<T, ()>

fun createEmptySet<T>(): Set<T> { return createEmptyMap() }
fun Set<T>.add(mutate self, k: T) { self.set(k, ()) }
fun Set<T>.remove(mutate self, k: T) { self.delete(k) }

@method_id(139)
fun test39() {
    var s: Set<int8> = createEmptySet();
    s.add(10); s.add(10);
    s.add(20);
    s.add(30);
    s.remove(20);
    return (s.exists(20), s.exists(10), s.calculateSize());
}

@method_id(140)
fun test40() {
    var `m!empty` = createEmptyMap() as map<int32, int64>?;
    var `m filled` = createEmptyMap() as map<int32, int64>?;
    var `m_null` = null as map<int32, int64>?;

    `m filled`!.set(10, 20);
    return (
        `m!empty` == null, `m filled` == null, `m_null` == null, 777,
        `m!empty`!.isEmpty(), `m filled`!.isEmpty(), 777,
        `m!empty`, `m_null`,
    );
}

@method_id(141)
fun test41() {
    var m = createEmptyMap<int8, HashMode>();
    m.set(-7, HashMode.NoHash);
    m.addIfNotExists(111, HashMode.TotalHash);
    m.replaceIfExists(-7, HashMode.PartialHash);

    val k7 = m.get(-7);
    return (
        m.exists(8), m.mustGet(111) == HashMode.TotalHash, 
        k7.isFound, k7.loadValue() == HashMode.NoHash, k7.loadValue() == HashMode.PartialHash
    );
}

@method_id(142)
fun test42(dyn: int) {
    var m = createEmptyMap<int8, int8>();
    var t = createEmptyTuple();

    try { m.mustGet(1, 123); }
    catch (ex) { t.push(ex) }
    try { m.mustGet(1, 12345); }
    catch (ex) { t.push(ex) }
    try { m.mustGet(1, dyn); }
    catch (ex) { t.push(ex) }
    try { m.mustGet(1, dyn * 10); }
    catch (ex) { t.push(ex) }

    return t;
}


fun main() {
    __expect_type(createEmptyMap<int8, (int32, int64)>, "() -> map<int8, (int32, int64)>");
}

/**
@testcase | 101 |            | -1
@testcase | 102 | 1 10 2 20  | -1 -1 20
@testcase | 102 | 2 20 3 30  | 0 -1 30
@testcase | 103 | 100        | 0 3 100
@testcase | 104 |            | -1 0 10
@testcase | 105 |            | [ 1 10 2 20 3 30 30 30 20 10 ]
@testcase | 106 |            | 2 1 2 3 20 30 -1 0
@testcase | 107 |            | -1 0 0
@testcase | 108 |            | 0 -1 -1 -1 0 0 -1 -1 -1 0
@testcase | 109 |            | -1 50000000 (null) 0 10000000
@testcase | 110 |            | -1 0 1024 0 -1 20
@testcase | 111 |            | -1 -1 (null) 1 -1 (null) (null) 0
@testcase | 112 |            | 0 1 3 -1 0 0 -1 30
@testcase | 114 | 2          | 3
@testcase | 114 | 3          | (null)
@testcase | 115 |            | 20
@testcase | 116 |            | 10 20 0 -1
@testcase | 117 |            | -1
@testcase | 118 |            | (null) -1 -1 0 10 10 20 20 -1 0 -1
@testcase | 119 |            | 10 10
@testcase | 120 |            | 10 10
@testcase | 121 |            | 0 -1 10 10 -1 0
@testcase | 122 |            | 1 3 3 20 20 20
@testcase | 123 |            | 0 -1 10 10 -1 0
@testcase | 124 |            | (null) 0 2 2 [ 1 1 -1 2 2 -1 3 3 0 ]
@testcase | 126 |            | -1 -1 0 16 24 32
@testcase | 127 |            | 1 0
@testcase | 128 |            | 6 3
@testcase | 129 |            | -1 -1 -1 (null) 0 (null) 30 30 2 3 2
@testcase | 130 |            | -1 30 30 [ 16 0 ] 20
@testcase | 131 |            | (null) (null) 0 [ -1 0 0 0 ]
@testcase | 132 |            | 16 1 2 16 -1 187
@testcase | 133 |            | 10 10
@testcase | 134 | 5          | [ 5 -1 5 ]
@testcase | 135 |            | 0 1 20 2 20 0 2
@testcase | 136 |            | 10
@testcase | 137 |            | 9
@testcase | 138 | 1          | 10
@testcase | 138 | 4          | 55
@testcase | 139 |            | 0 -1 2
@testcase | 140 |            | 0 0 -1 777 -1 0 777 (null) typeid-1 (null) 0
@testcase | 141 |            | 0 -1 -1 0 -1
@testcase | 142 | 80         | [ 123 12345 80 800 ]

@fif_codegen
"""
  test1() PROC:<{               // 
    NEWDICT                     //  m
    1 PUSHINT                   //  m '2=1
    x{0000000a} PUSHSLICE
    s0 s2 XCHG
    8 PUSHINT
    DICTISET                    //  m
    1 PUSHINT                   //  m '11=1
    SWAP
    8 PUSHINT                   //  '11=1 m '16=8
    DICTIGET NULLSWAPIFNOT      //  '12 '13
    NIP                         //  '13
  }>
"""

@fif_codegen
"""
  test13() PROC:<{              //  m
    123 PUSHINT                 //  m '1=123
    SWAP
    32 PUSHINT                  //  '1=123 m '6=32
    DICTIGET NULLSWAPIFNOT      //  '2 '3
  }>
"""

@fif_codegen
"""
  test14() PROC:<{
    ...
    64 PUSHINT
    DICTUGETNEXT NULLSWAPIFNOT2
    DROP
    NIP
  }>
"""

@fif_codegen
"""
  test15() PROC:<{
    ...
    x{9FE9794E38E5400E2C75F4BF2DFBAB1BF0568C9EDFC717359BB8999C47D4FB9AC23_} PUSHSLICE
    SWAP
    267 PUSHINT
    DICTGET
    9 THROWIFNOT 
    8 LDI
    ENDS
  }>
"""

@fif_codegen
"""
  test16() PROC:<{ 
    NEWDICT
    100 PUSHINT
    x{0a14} PUSHSLICE
    s0 s2 XCHG
    32 PUSHINT
    DICTISET
"""

@fif_codegen
"""
  test25() PROC:<{              // 
    NEWDICT                     //  m
    1 PUSHINT                   //  m '2=1
    x{0101} PUSHSLICE           //  m '2=1 '3
    s0 s2 XCHG
    8 PUSHINT                   //  '3 '2=1 m '6=8
    DICTISET                    //  m
  }>
"""

@fif_codegen
"""
  test30() PROC:<{ 
    NEWDICT
    1 PUSHINT
    <b b> PUSHREF
    s0 s2 XCHG
    8 PUSHINT
    DICTISETREF
"""

@fif_codegen
"""
  test33() PROC:<{ 
    NEWDICT
    123 PUSHINT
    x{0a0a} PUSHSLICE
    s0 s2 XCHG
    8 PUSHINT
    DICTUSET
    123 PUSHINT
    SWAP
    8 PUSHINT
    DICTUGET
    9 THROWIFNOT
    8 LDI
    8 LDI
    ENDS
  }>
"""

@fif_codegen
"""
  test34() PROC:<{
    NEWDICT
    123 PUSHINT
    NEWC
    x{0a0a} STSLICECONST
    s3 PUSH
    8 STIR
    s0 s2 XCHG
    32 PUSHINT
    DICTUSETB
"""

@fif_codegen
"""
  test36() PROC:<{              // 
    x{9FFDE075922FCD6EC7F0A0F3E32D6828C8AE03395763E4C5ECF19503052F1829F45_} PUSHSLICE   //  k
    NEWDICT                     //  k m
    x{0000000a} PUSHSLICE
    s2 s1 PUXC
    267 PUSHINT
    DICTSET                     //  k m
"""

*/

