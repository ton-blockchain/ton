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


@method_id(101)
fun test1() {
    var t: StorWithStr = { a: 10, str: "abc", b: 20 };
    var c = t.toCell();
    var back = c.load();
    return ((t.b == back.b) & (t.str.bitsEqual(back.str)), back.str.remainingBitsCount(), c.hash() & 0xFFFF);
}

@method_id(102)
fun test2() {
    var s = ("" as TelegramString, "" as TelegramString);
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

fun main() {
    __expect_type("" as TelegramString, "TelegramString");
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

@fif_codegen
"""
  test3() PROC:<{
    x{0102} PUSHSLICE
    8 LDU
    8 LDI
    DROP
  }>
"""
 */
