// encoded as TL/B `len: (## 8) data: (bits (len*8))`
type TelegramString = slice;

fun TelegramString.packToBuilder(self, mutate b: builder) {
    val bytes = self.remainingBitsCount() / 8;
    b.storeUint(bytes, 8);
    b.storeSlice(self);
}

fun TelegramString.unpackFromSlice(mutate s: slice) {
    val bytes = s.loadUint(8);
    return s.loadBits(bytes * 8);
}

type Custom8 = int;

fun Custom8.packToBuilder(self, mutate b: builder) {
    b.storeUint(self, 8)
}

fun Custom8.unpackFromSlice(mutate s: slice) {
    return s.loadUint(8)
}

struct StorWithStr {
    a: int32;
    str: TelegramString;
    b: int32;
}

struct PointWithCustomInt {
    a: Custom8;
    b: int8;
}


type MyBorderedInt = int;

fun MyBorderedInt.packToBuilder(self, mutate b: builder) {
    if (self > 10) { b.storeUint(1, 4) }
    else if (self > 0) { b.storeUint(2, 4) }
    else { b.storeUint(3, 4) }
}

fun MyBorderedInt.unpackFromSlice(mutate s: slice) {
    return match (s.loadUint(4)) {
        1 => 10,
        2 => 0,
        3 => -1,
        else => throw 123
    }
}

struct WithMyBorder {
    a: int8;
    b: MyBorderedInt;
}

type MyCustomNothing = ();

struct WithFakeWriter {
    a: int8;
    fake: MyCustomNothing = ();
    b: int8;
}

fun MyCustomNothing.packToBuilder(self, mutate b: builder) {
    b.storeUint(123, 32);
    b.storeRef(createEmptyCell());
}

global gModByCustom: int;

type MagicGlobalModifier = ();

fun MagicGlobalModifier.packToBuilder(self, mutate b: builder) {
    b.storeUint(gModByCustom, 8);
}

fun MagicGlobalModifier.unpackFromSlice(mutate s: slice) {
    gModByCustom = s.loadUint(8);
    return ();
}

struct WithGlobalModifier {
    a: int8;
    g: MagicGlobalModifier = ();
    n: int8;
}

type Tensor3Skipping1 = (int, int, int);

fun Tensor3Skipping1.unpackFromSlice(mutate s: slice): Tensor3Skipping1 {
    val e0 = s.loadUint(8);
    val e2 = s.loadUint(8);
    return (e0, 0, e2);
}

fun Tensor3Skipping1.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.0, 8).storeUint(self.2, 8);
}

enum Color {
    Red,
    Green,
    Blue,
}

fun Color.unpackFromSlice(mutate s: slice) {
    return s.loadUint(2) as Color       // don't check for correctness
}

fun Color.packToBuilder(self, mutate b: builder) {
    b.storeUint(self as int, 2)
}

type OnlyWithPack = int8

fun OnlyWithPack.packToBuilder(self, mutate b: builder) {
    b.storeUint(0xFF, 8);
    b.storeUint(self, 8);
}

type OnlyWithUnpack = int8

fun OnlyWithUnpack.unpackFromSlice(mutate s: slice): int8 {
    assert (s.loadUint(8) == 0xFF) throw 123;
    return s.loadUint(8)
}

struct HasOnlyWithUnpack {
    wu: OnlyWithUnpack
}

struct CustomPoint {
    x: int
    y: int
}

fun CustomPoint.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.x, 8);
    b.storeUint(self.y, 8);
}

fun CustomPoint.unpackFromSlice(mutate s: slice): CustomPoint {
    return {
        x: s.loadUint(8),
        y: s.loadUint(8),
    }
}

type CustomPointAlias = CustomPoint

struct GenericTensor<T1, T2> {
    v: (T1, T2)
}

fun GenericTensor<X, Y>.packToBuilder(self, mutate b: builder) {
    b.storeAny<X>(self.v.0);
    b.storeAny<Y>(self.v.1);
}

fun GenericTensor<W, Z>.unpackFromSlice(mutate s: slice) {
    return GenericTensor<W, Z> {
        v: (s.loadAny(), s.loadAny())
    }
}

type GenericStrangeShape<T1, T2> = [T1, T2]

fun GenericStrangeShape<X, Y>.packToBuilder(self, mutate b: builder) {
    var refB = beginCell();
    refB.storeAny<X>(self.0);
    refB.storeAny<Y>(self.1);
    b.storeRef(refB.endCell())
}

fun GenericStrangeShape<W, Z>.unpackFromSlice(mutate s: slice): GenericStrangeShape<W, Z> {
    var refS = s.loadRef().beginParse();
    return [refS.loadAny<W>(), refS.loadAny<Z>()]
}

fun GenericStrangeShape<int, int>.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.0, 16);
    b.storeUint(self.1, 16);
}

fun GenericStrangeShape<int, int>.unpackFromSlice(mutate s: slice) {
    return [s.loadUint(8), s.loadUint(8)] as [int, int]
}

struct WillBeManuallyInvoked<T> {
    item: T
}

fun WillBeManuallyInvoked<T>.packToBuilder(self, mutate b: builder) {
    b.storeUint(0x123, 32).storeAny(self.item);
}

fun WillBeManuallyInvoked<T>.unpackFromSlice(mutate s: slice) {
    assert (s.loadUint(32) == 0x123) throw 123;
    return WillBeManuallyInvoked<T> { item: s.loadAny() };
}


@method_id(101)
fun test1() {
    var t: StorWithStr = { a: 10, str: "abc".literalSlice(), b: 20 };
    var c = t.toCell();
    var back = c.load();
    return ((t.b == back.b) & (t.str.bitsEqual(back.str)), back.str.remainingBitsCount(), c.hash() & 0xFFFF);
}

@method_id(102)
fun test2() {
    var s = (createEmptySlice() as TelegramString, "".literalSlice() as TelegramString);
    return beginCell().storeAny(s).endCell().beginParse().remainingBitsCount();
}

@method_id(103)
fun test3() {
    return PointWithCustomInt.fromSlice(stringHexToSlice("0102"));
}

@method_id(104)
fun test4(initialInt: int) {
    var c = WithMyBorder { a: 0, b: initialInt }.toCell();
    return c.load().b;
}

@method_id(105)
fun test5() {
    var f: WithFakeWriter = { a: 10, b: 20 };
    var s = beginCell().storeAny(f).endCell().beginParse();
    val size = s.remainingBitsAndRefsCount();
    return (s.skipBits(8).loadUint(32), size.1);
}

@method_id(106)
fun test6() {
    gModByCustom = 6;
    val m1: WithGlobalModifier = { a: 8, n: 16 };
    val c1 = m1.toCell();
    val r2 = WithGlobalModifier.fromSlice(stringHexToSlice("01FF02"));
    val gAfter2 = gModByCustom;
    WithGlobalModifier.fromSlice(stringHexToSlice("010002"));   // not deleted, sets 0
    val gAfter3 = gModByCustom;
    var s = stringHexToSlice("010902FF");
    s.skipAny<WithGlobalModifier>();        // custom loader also called
    return (c1.beginParse().skipBits(8).loadUint(8), r2.n, gAfter2, gAfter3, s.remainingBitsCount(), gModByCustom);
}

@method_id(107)
fun test7() {
    val t = (1, 2, 3) as Tensor3Skipping1;
    __expect_type(t.toCell(), "Cell<Tensor3Skipping1>");
    return t.toCell().load();
}

@method_id(108)
fun test8(c: Color) {
    var way1 = beginCell().storeAny(c).endCell();
    var way2 = c.toCell();
    __expect_type(way2, "Cell<Color>");
    assert (way1.hash() == way2.hash()) throw 123;
    return way2.load();
}

@method_id(109)
fun `test only pack and only unpack`() {
    var b1 = beginCell().storeAny(0x80 as OnlyWithPack);
    var (s1, s2, s3) = (b1.toSlice(), b1.toSlice(), b1.toSlice());
    var has: OnlyWithUnpack = s1.loadAny();
    s2.skipAny<OnlyWithUnpack>();
    s3.skipAny<HasOnlyWithUnpack>();
    return (
        b1.bitsCount(), b1.hash() & 0xFFFF,
        s1.remainingBitsCount(), s2.remainingBitsCount(), s2.remainingBitsCount(), has
    );
}

@method_id(110)
fun test10() {
    var p1: CustomPoint = { x: 10, y: 20 };
    var p2: CustomPointAlias = { x: 100, y: 200 };
    return (
        p1.toCell().beginParse().loadAny<CustomPoint>(),
        p2.toCell().beginParse().loadAny<CustomPointAlias>(),
    )
}

@method_id(111)
fun test11() {
    // `int` is not serializable, but since we don't serialize this object, `packToBuilder` is not instantiated
    val gtInt: GenericTensor<int, int?> = { v: (1, null) };

    var s = GenericTensor<uint8, uint8> { v: (0xFF, 0xFF) }.toCell().beginParse();
    return (gtInt, s.loadAny<GenericTensor<uint4, uint4>>(), s.loadUint(8))
}

@method_id(112)
fun test12() {
    var sh1 = [1, 2] as GenericStrangeShape<int, int>;
    var sh2 = [1, 2] as GenericStrangeShape<int8, int8>;
    var sh3 = [1, 2] as GenericStrangeShape<int16, int16>;
    var (s1, s2, s3) = (sh1.toCell().beginParse(), sh2.toCell().beginParse(), sh3.toCell().beginParse());
    var counts = (s1.remainingBitsAndRefsCount(), s2.remainingBitsAndRefsCount(), s3.remainingBitsAndRefsCount());
    [sh1, sh2, sh3] = [s1.loadAny(), s2.loadAny(), s3.loadAny()];
    return (
        counts,
        s1.isEmpty(), s2.isEmpty(), s3.isEmpty(),
        sh1, sh2, sh3,
    );
}

@method_id(113)
fun test13() {
    var w = WillBeManuallyInvoked<int8> { item: 100 };

    // test that if functions are (why ever) invoked manually, so they already exist in the pipeline,
    // the compiler won't fail trying to instantiate them once again, and will reuse them in serialization
    var b = beginCell();
    w.packToBuilder(mutate b);
    var s = b.endCell().beginParse();
    var w2 = WillBeManuallyInvoked<int8>.unpackFromSlice(mutate s);

    var c = w.toCell().beginParse();
    var w3 = WillBeManuallyInvoked<int8>.fromSlice(c);
    return (
        b.hash() == c.hash(),
        s.isEmpty(),
        c.isEmpty(),
        w2,
        w3,
    )
}


fun main() {
    __expect_type(createEmptySlice() as TelegramString, "TelegramString");
}

/**
@testcase | 101 |     | -1 24 5203
@testcase | 102 |     | 16
@testcase | 103 |     | 1 2
@testcase | 104 | 55  | 10
@testcase | 104 | 8   | 0
@testcase | 104 | -5  | -1
@testcase | 105 |     | 123 1
@testcase | 106 |     | 6 2 255 0 8 9
@testcase | 107 |     | 1 0 3
@testcase | 108 | 0   | 0
@testcase | 108 | 3   | 3
@testcase | 109 |     | 16 20571 0 0 0 128
@testcase | 110 |     | 10 20 100 200
@testcase | 111 |     | 1 (null) 15 15 255
@testcase | 112 |     | 32 0 0 1 0 1 0 -1 -1 [ 0 1 ] [ 1 2 ] [ 1 2 ]
@testcase | 113 |     | -1 -1 0 100 100

@fif_codegen
"""
  test3() PROC:<{
    x{0102} PUSHSLICE
    8 LDU
    8 LDI
    ENDS
  }>
"""
 */
