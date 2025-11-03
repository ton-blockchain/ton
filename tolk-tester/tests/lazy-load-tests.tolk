import "@stdlib/tvm-dicts.tolk"

struct A { a1: int7; a2: uint8; a3: uint8; }
struct B { b1: int7; b2: uint8; b3: uint8; }
type AOrB = A | B;

@noinline
fun A.getA1(self) {
    return self.a1;
}

struct Fields9 {
    f1: uint8; f2: uint8; f3: uint8;
    f4: uint8; f5: uint8; f6: uint8;
    f7: uint8; f8: uint8; f9: uint8;
}

@noinline
fun Fields9.method(self) {}

fun Fields9.getF2(self) { return self.f2 }
fun Fields9.getF5(self) { return self.f5 }

struct Small { s1: int7; }
struct Big { b1: int7; b2: int8; b3: int8; }
type SmallOrBig = Small | Big;

struct StructF1F2AB {
    f1: int8;
    f2: int8;
    ab: A | B;
}

struct StructF1SBF3 {
    f1: int8;
    sb: Small | Big;
    f3: int8;
}

struct WithEitherABMiddle {
    f1: int32;
    f2: int32;
    ab: A | B;
    f4: int32;
    f5: int32;
    f6: int32;
}

struct WithEitherABEnd {
    f1: int8;
    f2: int8;
    f3: int8;
    f4: int8;
    f5: int8;
    ab: A | B;
}

struct Point {
    x: int8;
    y: int8;
}

fun Point.getX(self) {
    return self.x
}

fun Point.makeCell(self) {
    return self.toCell()
}

fun Point.makeCellWrapped(self) {
    val cc = self.makeCell();
    self.x;
    return cc
}

fun Point.incXAndMakeCell(mutate self) {
    __expect_inline(true);
    self.x += 1;
    return self.toCell()
}

struct Point3d {
    x: int32;
    y: int32;
    z: int32;
}

type PointOrPoint3d = Point | Point3d;

struct Point78ForMaybe {
    x: int7;
    y: int8;
}

type OptionalPoint78ForMaybe = Point78ForMaybe?;

struct ComplexWithPoint {
    f1: int8;
    p: Point;
    f3: int8;
}

struct ComplexWithMaybePoint {
    f1: int8;
    p: Point78ForMaybe?;
    f3: int8;
}

struct Counter7Increment { byValue: int7 }
struct Counter7Decrement { byValue: int7 }
type MsgEitherCounter = Counter7Increment | Counter7Decrement;

struct FieldsAndEitherCounter {
    f1: int8;
    f2: int8;
    c: MsgEitherCounter;
}

struct WalletStorage {
    isSignatureAllowed: bool;
    seqno: uint32;
    walletId: uint32;
    publicKey: uint256;
    extensions: dict;
}

fun WalletStorage.save(self) {
    contract.setData(self.toCell())
}

@overflow1023_policy("suppress")
struct NotFixedWidthStorage {
    f1: int8;
    f2: int8?;
    f3: int8;
    f4: bool;
    f5: cell;
    f6: int8 | bool;
    f7: any_address;
    f8: int8;
    f9: bits200;
    f10: (bits400, ());
    f11: coins;
}

struct Inner1 { i1: int8; i2: int8; }
struct Inner2 { i1: int8; i2: int8; }
type Inner2Alias = Inner2;
struct Outer {
    inner1: Inner1;
    inner2: Inner2Alias;
    wa: StructF1F2AB;
}

struct WithMaybe {
    f1: int8;
    big: Big?;
    f3: int2;
}

struct WithRefs {
    r1: cell?;
    f2: int8;
    r3: Cell<Point>;
    f4: int8;
}

struct (0x01) CounterIncrement { byValue: int8 }
struct (0x02) CounterDecrement { byValue: int8 }
struct (0x03) CounterDecrementBy1 { }
struct (0x04) CounterReset { initialValue: int32 }
type MsgFullCounter = CounterIncrement | CounterDecrement | CounterDecrementBy1 | CounterReset;

struct(0x12345678) Counter32Increment { byValue: int32; }
struct(0x23456789) Counter32DecrementBy1 {}
struct(0x34567890) Counter32Reset { initial: uint32; f2: int8; f3: int8; f4: int8; f5: int8; }
type MyMsg32 = Counter32Increment | Counter32DecrementBy1 | Counter32Reset;

struct AutoP1 { f1: int8; f2: int8; }
struct AutoP2 { f1: int8; f2: int8; f3: int8; }
struct AutoP3;
type AutoPrefixedBin2 = AutoP1 | AutoP2 | AutoP3; // 00 / 01 / 10

struct(0b0111) ShortP1 { f1: int8; }
struct(0b0100) ShortP2 { f1: int8; f2: int8; }
type ShortPrefixed = ShortP1 | ShortP2;

struct(0b01) NotEqPrefixedS1 { f: uint6; }
struct(0b0001) NotEqPrefixedS2 { f: uint4; }
struct(0b101) NotEqPrefixedS3 { f: int5; }
type NotEqPrefixed = NotEqPrefixedS1 | NotEqPrefixedS2 | NotEqPrefixedS3;

struct WithBitsField {
    bin16: bits16;
    bin8: bits8;
}

struct WithOneRestField {
    i32: int32;
    rest: RemainingBitsAndRefs;
}

struct VestingStorage {
    vestingParameters: Cell<(uint64, int32)>;
    seqno: uint32;
    subwalletId: uint32;
    publicKey: uint256;
    whitelist: dict;
}

struct OneIntOneRef {
    z: cell? = null;
    o: cell;
    e: () = ();
    i: int32 = 0;
    r: cell;
}

struct WithVariadicInts {
    i1: varuint32;
    i2: varuint32;
}

struct WithInternalAddress {
    f1: int8;
    f2: address;
    f3: int8;
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

struct WithN5 { n: uint5 }
type U111 = int8 | int16 | WithN5 | bits100
struct WithU111 {
    a: U111
    b: U111?
    bit: bool
}
fun WithU111.getSlice_a8_bnull(): slice asm "b{000000000101} PUSHSLICE"
fun WithU111.getSlice_a16_b8(): slice asm "b{010000000000000001100000000011} PUSHSLICE"
fun WithU111.getSlice_a8_b5(): slice asm "b{0000000001110111111} PUSHSLICE"
fun WithU111.getSlice_a8_b5_nobit(): slice asm "b{000000000111011111} PUSHSLICE"


@noinline
fun contract.getFakeData(seqno: int): Cell<WalletStorage> {
    return WalletStorage {
        isSignatureAllowed: true,
        seqno: seqno,
        walletId: 999,
        publicKey: 1 << 100,
        extensions: null,
    }.toCell()
}

fun getPointCell(x: int, y: int): Cell<Point> {
    val p: Point = {x,y};
    return p.toCell();
}

fun loadWalletStorage() {
    return WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
}

fun WalletStorage.load() {
    return WalletStorage.fromCell(contract.getFakeData(777));
}

@method_id(101)
fun demo101() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x");
    return p.x;
}

@method_id(102)
fun demo102(takeX: bool) {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    var result: int;
    __expect_lazy("[p] load x y");
    if (takeX) {
        result = p.x;
    } else {
        result = p.y;
    }
    return result;
}

@method_id(103)
fun demo103() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] skip (bits8), load y");
    return p.y * p.y * p.y;
}

@method_id(104)
fun demo104() {
    var p2 = lazy Point.fromSlice(stringHexToSlice("0A14"));
    __expect_lazy("[p2] load x y");
    assert(p2.x < 100) throw 100;
    assert(p2.y < 100) throw 100;

    var c = lazy ComplexWithPoint.fromSlice(stringHexToSlice("01020304"));
    __expect_lazy("[c] load f1, skip (bits16), load f3");
    return (c.f1, 0 + c.f3);
}

@method_id(105)
fun demo105(s: slice) {
    return (fun(s: slice) {
        var c = lazy ComplexWithMaybePoint.fromSlice(s);
        __expect_lazy("[c] load f1, skip (Point78ForMaybe?), load f3");
        return (c.f1, c.f3);
    })(s);
}

@method_id(106)
fun demo106(s: slice) {
    var c = lazy ComplexWithMaybePoint.fromSlice(s);
    var x: int? = null;
    __expect_lazy("[c] skip (bits8), load p");
    if (c.p != null) {
        x = c.p.x;
    }
    return (c.p == null, x);
}

@method_id(107)
fun demo107() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    return p;
}

@method_id(108)
fun demo108() {
    var o = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    __expect_lazy("[o] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
    {
        assert(o.f3 == 3) throw 100;
        assert(o.f9 == 9) throw 100;
    }

    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    p.y = 15;
    return (p, o);
}

@method_id(109)
fun demo109() {
    var st = lazy WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
    __expect_lazy("[st] skip (bits65), load publicKey");
    return st.publicKey;
}

@method_id(110)
fun demo110(s: slice) {
    var msg = lazy MsgEitherCounter.fromSlice(s);
    var t = createEmptyTuple();
    __expect_lazy("[msg] lazy match");
    match (msg) {
        Counter7Increment => {
            __expect_lazy("[msg] load byValue");
            t.push(+msg.byValue)
        }
        Counter7Decrement => {
            __expect_lazy("[msg] load byValue");
            t.push(-msg.byValue)
        }
    }
    return t;
}

@method_id(111)
fun demo111(s: slice, x: int) {
    var msg = lazy MsgEitherCounter.fromSlice(s);
    var t = createEmptyTuple();
    match (msg) {
        Counter7Increment => {
            assert(x >= 0, 123);
            __expect_lazy("[msg] load byValue");
            t.push(+msg.byValue)
        }
        Counter7Decrement => {
            assert(x >= 1, 456);
            assert(t.size() == 0, 789);
            __expect_lazy("[msg] load byValue");
            t.push(-msg.byValue)
        }
    }
    return t;
}

@method_id(112)
fun demo112(s: slice) {
    var msg = lazy MsgFullCounter.fromSlice(s);
    var curX = 0;
    __expect_lazy("[msg] lazy match");
    match (msg) {
        CounterIncrement => curX += msg.byValue,
        CounterDecrement => curX -= msg.byValue,
        CounterDecrementBy1 => curX = -1,
        CounterReset => curX = msg.initialValue,
    }
    return curX;
}

@method_id(113)
fun demo113(s: slice) {
    var msg = lazy MsgFullCounter.fromSlice(s);
    var curX = 0;
    var newX: int = match (msg) {
        CounterIncrement => curX + msg.byValue,
        CounterDecrement => curX - msg.byValue,
        CounterDecrementBy1 => -1,
        CounterReset => msg.initialValue,
    };
    return newX;
}

@method_id(114)
fun demo114(s: slice) {
    var msg = lazy MsgFullCounter.fromSlice(s);
    var curX = 0;
    var newX: int | [int] = match (msg) {
        CounterDecrementBy1 => -1,
        CounterReset => [msg.initialValue as int],
        CounterDecrement => [curX - msg.byValue],
        CounterIncrement => curX + msg.byValue,
    };
    return newX;
}

@method_id(115)
fun demo115(s: slice): int8 | bool {
    var msg = lazy MsgEitherCounter.fromSlice(s);
    return match (msg) {
        Counter7Decrement => msg.byValue < 5,
        Counter7Increment => msg.byValue as int8,
    }
}

@method_id(118)
fun demo118(s: slice) {
    match (var msg = lazy MsgEitherCounter.fromSlice(s)) {
        Counter7Increment => return +msg.byValue,
        Counter7Decrement => {
            assert(s.remainingRefsCount() == 0, 456);
            __expect_lazy("[msg] load byValue");
            return -msg.byValue
        }
    }
}

@method_id(119)
fun demo119(s: slice) {
    var o = lazy SmallOrBig.fromSlice(s);
    var s1 = -1;
    var b1 = -1;
    match (o) {
        Small => {
            __expect_lazy("[o] load s1");
            s1 = o.s1;
        }
        Big => {
            __expect_lazy("[o] skip (bits7), load b2");
            b1 = o.b2;
        }
    }
    return (s1, b1);
}

@method_id(120)
fun demo120(s: slice) {
    var msg = lazy FieldsAndEitherCounter.fromSlice(s);
    var t = createEmptyTuple();
    __expect_lazy("[msg] load f1, skip (bits8), lazy match c");
    match (msg.c) {
        Counter7Increment => { t.push(+msg.c.byValue) }
        Counter7Decrement => { t.push(-msg.c.byValue) }
    }
    t.push(msg.f1);
    return t;
}

@method_id(121)
fun demo121() {
    var c = FieldsAndEitherCounter{f1:1, f2:2, c:Counter7Decrement{byValue:20}};
    var o = lazy FieldsAndEitherCounter.fromCell(c.toCell());
    __expect_lazy("[o] load f1 f2 c");      // load c, not lazy match
    return (o.c is Counter7Increment) ? o.f1 + o.c.byValue : o.f2 + o.c.byValue;
}

@method_id(122)
fun demo122(s: slice): int | slice {
    var o = lazy FieldsAndEitherCounter.fromSlice(s);
    __expect_lazy("[o] load f1, skip (bits8), lazy match c");
    return match(o.c) {
        Counter7Decrement => -o.c.byValue,
        Counter7Increment => o.f1 + o.c.byValue,
    }
}

@method_id(123)
fun demo123(s: slice) {
    var o = lazy FieldsAndEitherCounter.fromSlice(s);
    __expect_lazy("[o] load f1 f2 c");        // not lazy match, used in a complex expression
    return o.f1 + match (o.c) {
        Counter7Increment => o.f1 as int,
        Counter7Decrement => o.f2 * o.f2,
    }
}

@method_id(124)
fun demo124(s: slice) {
    var msg = lazy FieldsAndEitherCounter.fromSlice(s);
    var t = createEmptyTuple();
    __expect_lazy("[msg] skip (bits8), load f2, lazy match c");
    match (msg.c) {
        Counter7Increment => {
            var mm = msg.c;
            t.push(+mm.byValue)
        }
        Counter7Decrement => {
            try {}
            catch {}
            var mm = msg.c;
            t.push(-mm.byValue);
            t.push(msg.f2);
        }
    }
    return t;
}

@method_id(125)
fun demo125() {
    var o = lazy StructF1F2AB.fromSlice(stringHexToSlice("7F200F0203"));
    var (a1, a3, b2) = (-1,-1,-1);
    __expect_lazy("[o] load f1 f2 ab");
    match (o.ab) {
        A => {
            a1 = o.ab.a1;
            a3 = o.ab.a3;
        }
        B => {
            b2 = o.ab.b2;
        }
    }
    return (o, a1, a3, b2);
}

@method_id(126)
fun demo126(s: slice) {
    var o = lazy StructF1SBF3.fromSlice(s);
    var (s1, b2) = (-1, -1);
    __expect_lazy("[o] load f1 sb f3");
    match (o.sb) {
        Small => {
            s1 = o.sb.s1;
        }
        Big => {
            b2 = o.sb.b2;
        }
    }
    return (o, s1, b2);
}

@method_id(127)
fun demo127() {
    val o = lazy Outer.fromSlice(stringHexToSlice("0102030408FF7FFFFF"));
    __expect_lazy("[o] load inner1, skip (bits16), load wa");
    var inner1_i1 = o.inner1.i1;
    var inner1_i2 = o.inner1.i2;

    var wa_is_A: bool;
    var wa_last: int;
    match (o.wa.ab) {
        A => { wa_is_A = true;  __expect_lazy(""); wa_last = o.wa.ab.a3; }
        B => { wa_is_A = false; __expect_lazy(""); wa_last = o.wa.ab.b3; }
    }
    return (inner1_i1, inner1_i2, wa_is_A, wa_last);
}

@method_id(128)
fun demo128(s: slice) {
    var o = lazy WithMaybe.fromSlice(s);
    __expect_lazy("[o] load f1 big");
    assert (o.f1 > 0) throw 123;
    var isnull = true;
    var b3 = -1;
    match (o.big) {
        null => {
            debug.printString("big is null");
        }
        Big => {
            isnull = false;
            assert(o.big.b1 != 0, 123);
            b3 = o.big.b3;
        }
    }
    return (isnull, b3);
}

@method_id(129)
fun demo129() {
    var o = lazy AOrB.fromSlice(stringHexToSlice("0F0203"));
    var (a1, wasA) = (-1, false);
    __expect_lazy("[o] lazy match");
    match (o) {
        A => {
            __expect_lazy("[o] load a1 a2 a3");
            a1 = o.getA1();
            wasA = true;
        }
        B => {
            __expect_lazy("[o] skip (bits7), load b2");
            a1 = o.b2;
        }
    }
    return (wasA, a1);
}

@method_id(130)
fun demo130() {
    var st = lazy loadWalletStorage();
    __expect_lazy("[st] skip (bits33), load walletId");
    return st.walletId;
}

@method_id(131)
fun demo131() {
    var st = lazy WalletStorage.load();
    __expect_lazy("[st] skip (bits1), load seqno, skip (bits32), load publicKey");
    return (st.seqno, st.publicKey >> 100);
}

@method_id(132)
fun demo132(incSeqno: bool) {
    var st = lazy WalletStorage.fromCell(contract.getFakeData(888));
    __expect_lazy("[st] skip (bits1), load seqno, skip (bits288), load extensions");
    if (incSeqno) {
        return (st.seqno + 1, st.extensions == null);
    } else {
        return (st.seqno as int, st.extensions == null);
    }
}

@method_id(133)
fun demo133() {
    var extensions = createEmptyDict();
    extensions.iDictSet(32, 8, stringHexToSlice("12"));
    var wCell = WalletStorage {
        isSignatureAllowed: true,
        seqno: 0,
        walletId: 123,
        publicKey: 1 << 88,
        extensions,
    }.toCell();
    if (10 > 3) {
        var st = lazy wCell.load();
        __expect_lazy("[st] skip (bits321), load extensions");
        while (!false) {
            assert(st.extensions != null, 123);
            return st.extensions.iDictGet(32, 8).0!.loadUint(8);
        }
    } else {
        throw 0;
    }
}

@method_id(134)
fun demo134() {
    var cc = lazy CounterIncrement.fromSlice(stringHexToSlice("010f"), {throwIfOpcodeDoesNotMatch: 134});
    return cc.byValue;
}

@method_id(135)
fun demo135(skip: bool) {
    var cc = lazy WithBitsField.fromSlice(stringHexToSlice("123456"));
    __expect_lazy("[cc] load bin16, save immutable (tail)");
    cc.bin16 = stringHexToSlice("00000000") as bits16;
    try {
        return cc.toCell({
            skipBitsNValidation: skip
        }).beginParse().remainingBitsCount();
    } catch (excno) {
        return excno;
    }
}

@method_id(136)
fun demo136(skip: bool): int {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    if (skip) {
        return -1;
    } else {
        __expect_lazy("[p] skip (bits8), load y");
        return p.y;
    }
}

@method_id(137)
fun demo137(s: slice): int {
    val p = lazy Point.fromSlice(s);
    try {
        __expect_lazy("[p] load x");
        return p.x;     // will be ok even if s doesn't contain y
    } catch (excno) {
        return 100 + excno;
    }
}

@method_id(138)
fun demo138(s: slice): int {
    val p = lazy Point.fromSlice(s);
    try {
        __expect_lazy("[p] skip (bits8), load y");
        return p.y;
    } catch (excno) {
        return 100 + excno;
    }
}

@method_id(139)
fun demo139(s: slice, skip: bool): int {
    var msg = lazy MsgFullCounter.fromSlice(s);
    match (msg) {
        CounterIncrement => {
            try {
                if (s.isEmpty()) { return 100; }
                __expect_lazy("[msg] load byValue");
                return msg.byValue;
            } catch (excno) { return excno }
        }
        CounterReset => throw 123,
        CounterDecrementBy1 => throw 456,
        CounterDecrement => {
            if (!skip) {
                __expect_lazy("[msg] load byValue");
                return msg.byValue;
            }
            return 100;
        }
    }
}

@method_id(140)
fun demo140(getF4: bool) {
    val o = lazy WithEitherABEnd.fromSlice(stringHexToSlice("010203040580FF00"));
    var result: int;
    __expect_lazy("[o] skip (bits24), load f4, skip (bits8), lazy match ab");
    if (getF4) {
        result = o.f4;
    } else {
        match (o.ab) { A => throw 123, B => {
            result = o.ab.b2;
        } }
        if (o.f4 < 0) { result *= 100; }
    }
    return result;
}

@method_id(141)
fun demo141(eval: bool): int {
    val o = lazy WithEitherABEnd.fromSlice(stringHexToSlice("010203040500FF00"));
    if (eval) {
        try {
            __expect_lazy("[o] load f1 f2, skip (bits24), lazy match ab");
            match (o.ab) {
                A => { return o.f1 }
                B => { throw o.f2 }
            }
        } catch(excno) { return excno * 100 }
    }
    throw 123;
}

@method_id(142)
fun demo142(s: slice, ignore: bool) {
    val o = lazy AOrB.fromSlice(s);
    match (o) {
        A => {
            try {
                __expect_lazy("[o] skip (bits15), load a3");
                return o.a3 as int;
            } catch (excno) { return excno }
        }
        B => {
            if (ignore) { return -1; }
            else {
                if (ignore) { return -1; }
                else {
                    __expect_lazy("[o] load b1");
                    return o.b1 * 1;
                }
            }
        }
    }
}

@method_id(143)
fun demo143(): int {
    val o = lazy WithEitherABEnd.fromSlice(stringHexToSlice("0809000000"));   // ab missing
    try { try {
        __expect_lazy("[o] load f1 f2, skip (bits24), lazy match ab");
        try {
            match (o.ab) {
                A => { return 1 }
                B => { return 2 }
            }
        } catch { throw o.f1 + o.f2 }
    } catch(excno) { return excno } } catch { return -1 }
}

@method_id(144)
fun demo144(dummy: bool) {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    var result = -1;
    if (!dummy) {
        __expect_lazy("[p] load x");
        result = p.getX();
    }
    return result;
}

@method_id(145)
fun demo145(getF5: bool): int {
    var p = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    try {
        __expect_lazy("[p] skip (bits8), load f2, skip (bits16), load f5");
        if (getF5) {
            return p.getF5();
        }
        return 1 * p.getF2();
    } catch { throw 123 }
}

@method_id(146)
fun demo146() {
    val p = lazy WithRefs.fromSlice(stringHexToSlice("0000000"));     // no mandatory ref exists, but since we don't access it, it doesn't fail
    __expect_lazy("[p] load r1");
    return p.r1 == null;
}

@method_id(147)
fun demo147() {
    val p = lazy WithRefs.fromSlice(stringHexToSlice("0000000"));
    if (false) {} else {
    try {
        __expect_lazy("[p] skip (cell?) (bits8), load f4");
        return (true, p.f4 as int);        // even though `r3` (Cell<Point>) doesn't exist, it's ignored to reach `f4`
    } catch (excno) {
        return (false, excno);
    }}
}

@method_id(148)
fun demo148() {
    var o = lazy AOrB.fromSlice(stringHexToSlice("0F0203FF"));
    match (o) {
        A => {
            var bits: int;
            __expect_lazy("[o] load a1 a2 a3");
            bits = o.forceLoadLazyObject().remainingBitsCount();
            return bits;
        }
        B => throw 123
    }
}

@method_id(149)
fun demo149() {
    var o = lazy AOrB.fromSlice(stringHexToSlice("8F02"));  // corrupted, too small
    match (o) {
        A => throw 123,
        B => {
            try {
                __expect_lazy("[o] load b1 b2 b3");
                o.forceLoadLazyObject().assertEnd();
                return o.b1 << 8
            } catch (excno) {
                return excno
            }
        }
    }
}

@method_id(150)
fun demo150() {
    val o = lazy WithOneRestField.fromSlice(stringHexToSlice("00000001FFFF"));
    return (o.i32, o.rest.remainingBitsCount());
}

@method_id(151)
fun demo151() {
    val c = VestingStorage {
        whitelist: createEmptyDict(),
        subwalletId: 123,
        publicKey: 456,
        vestingParameters: (1 as uint64, 2 as int32).toCell(),
        seqno: 10,
    }.toCell();
    var st = lazy c.load();
    __expect_lazy("[st] skip (Cell<(uint64, int32)>) (bits320), load whitelist");
    return st.whitelist == null;    // need to skip prev bits, because dict requires 1 bit
}

@method_id(152)
fun demo152() {
    val c = OneIntOneRef { o: createEmptyCell(), r: (2 as int32).toCell() }.toCell();
    val st = lazy c.load();
    __expect_lazy("[st] skip (cell?) (cell), load r");
    return st.r.beginParse().remainingBitsCount();
}

@method_id(153)
fun demo153() {
    val c = OneIntOneRef { o: createEmptyCell(), r: (2 as int32).toCell() }.toCell();
    val st = lazy c.load();
    __expect_lazy("[st] skip (cell?), load i");
    return st.i;
}

global gTup: tuple;

@noinline
fun pushToGlobalTup<T>(v: T) { gTup.push(v.toTuple()); }

@method_id(154)
fun demo154() {
    gTup = createEmptyTuple();
    val p = lazy Point.fromSlice(stringHexToSlice("0102"));
    pushToGlobalTup(p);
    return gTup;
}

fun inlinedFunctionWithLazy(s: slice) {
    __expect_inline(true);
    val p = lazy Point.fromSlice(s);
    assert(p.y >= 0, 555);
}

@method_id(155)
fun demo155() {
    inlinedFunctionWithLazy(stringHexToSlice("0102"));
    inlinedFunctionWithLazy(stringHexToSlice("0304"));
    try {
        inlinedFunctionWithLazy(stringHexToSlice("03FF"));
    } catch (excno) { return excno; }
    return -1;
}

@method_id(156)
fun demo156(s: slice): int {
    val d = lazy CounterIncrement.fromSlice(s);
    __expect_lazy("[d] lazy match");
    match (d) {
        CounterIncrement => {
            __expect_lazy("[d] load byValue");
            return d.byValue;
        }
        else => {
            return -100;
        }
    }
}

@method_id(157)
fun demo157() {
    var c: Cell<WithVariadicInts> = WithVariadicInts { i1: 1 << 100, i2: 1 << 200 }.toCell();
    val d = lazy c.load();
    return d.i2 == (1 << 200);
}

@method_id(158)
fun demo158() {
    val m1 = lazy WithGlobalModifier.fromSlice(stringHexToSlice("01FF"));
    __expect_lazy("[m1] skip (bits8), load g");
    m1.g;       // modifies gModByCustom by a custom unpack function
    val after1 = gModByCustom;
    val m2 = lazy WithGlobalModifier.fromSlice(stringHexToSlice("010A02"));
    __expect_lazy("[m2] skip (bits8) (MagicGlobalModifier), load n");
    return (m2.n, gModByCustom, after1);  // also modifies by skipping (unpack called)
}

@method_id(159)
fun demo159() {
    val p1 = lazy WithU111.fromSlice(WithU111.getSlice_a8_bnull());
    val p2 = lazy WithU111.fromSlice(WithU111.getSlice_a16_b8());
    val p3 = lazy WithU111.fromSlice(WithU111.getSlice_a8_b5());
    val p4 = lazy WithU111.fromSlice(WithU111.getSlice_a8_b5_nobit());

    val p1_bit = p1.bit;
    val p2_bit = p2.bit;
    val p3_bit = p3.bit;
    val p4_b = p4.b;

    return (p1_bit, p2_bit, (p3.b is WithN5) ? p3.b.n as int : 777, p3_bit, (p4_b is WithN5) ? p4_b.n as int: 777);
}


@method_id(200)
fun demo200() {
    var st = lazy WalletStorage.fromCell(contract.getFakeData(888));
    var b = beginCell();
    if (10>3) {
        __expect_lazy("[st] load isSignatureAllowed seqno walletId publicKey extensions");
        assert(st.isSignatureAllowed, 123);
        b.storeAny(st);
        var st2 = lazy WalletStorage.fromCell(b.toCell());
        __expect_lazy("[st2] load isSignatureAllowed");
        if (st2.isSignatureAllowed) {
            return 1;
        }
    }
    throw 123;
}

@method_id(201)
fun demo201() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    var c1 = p.toCell();
    var x = p.x;
    var c2 = p.toCell();
    var y = p.y;
    var c3 = p.toCell();
    assert(c1.beginParse().bitsEqual(c2.beginParse()), 100);
    assert(c1.beginParse().bitsEqual(c3.beginParse()), 100);
    return (x, y, p is Point);
}

@method_id(202)
fun demo202() {
    var o = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    __expect_lazy("[o] load f1 f2, save immutable (tail), skip (bits24), load f6");
    if (o.f1 == 1) {
        o.f2 = 127;
    }
    var f6 = o.f6;
    var o2 = lazy o.toCell().load();
    __expect_lazy("[o2] skip (bits8), load f2");
    return (o.f2, o2.f2, f6);
}

@method_id(203)
fun demo203() {
    var o = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    if (o.f1 == 1) {
        return o.toCell().load();
    }
    throw 123;
}

@method_id(204)
fun demo204() {
    var o = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    __expect_lazy("[o] load f1 f2 f3 f4, save immutable (tail)");
    if (o.f1 != 1) {
        return o.toCell().load();
    } else {
        o.f1 = 10;
    }
    if (o.f2 != 2) {
        return o.toCell().load();
    } else {
        o.f2 = 20;
    }
    if (o.f3 != 3) {
        return o.toCell().load();
    } else {
        o.f4 = 40;
    }
    return o.toCell().load();
}

@method_id(205)
fun demo205() {
    var o = lazy WithMaybe.fromSlice(stringHexToSlice("551_"));
    __expect_lazy("[o] save immutable (tail), skip (bits8), load big");
    match (o.big) { null => {} Big => {} }
    return o.toCell().beginParse().bitsEqual(stringHexToSlice("551_"));
}

@method_id(206)
fun demo206() {
    var o = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    o.method();
    return (o.toCell() is Cell<Fields9>, o.toCell().beginParse().remainingBitsCount());
}

@method_id(207)
fun demo207() {
    var o = lazy WithRefs.fromCell(WithRefs {
        r1: null,
        f2: 123,
        r3: getPointCell(10, 20),
        f4: 55,
    }.toCell());
    o.r1 = getPointCell(60, 70);
    o.f2 = 90;
    return o.toCell();
}

@method_id(208)
fun demo208(s: slice) {
    var o = lazy AOrB.fromSlice(s);
    __expect_lazy("[o] lazy match");
    match (o) {
        A => {
            __expect_lazy("[o] load a1 a2 a3");
            var bc = o.toCell().beginParse().remainingBitsCount();
            return (bc, (o as A|B).toCell().beginParse().remainingBitsCount());
        }
        B => {
            assert(10>3, 101);
            __expect_lazy("[o] load b1 b2 b3");     // loaded because of toCell
            return (o.b1 as int, o.toCell().hash() as int);
        }
    }
}

@method_id(209)
fun demo209() {
    var o = lazy StructF1F2AB.fromSlice(stringHexToSlice("7F200F0203"));
    __expect_lazy("[o] skip (bits16), lazy match ab");
    match (o.ab) {
        A => {
            return o.ab.toCell().beginParse().remainingBitsCount();
        }
        B => throw 0xFFFF
    }
}

@method_id(210)
fun demo210() {
    var o = lazy StructF1F2AB.fromSlice(stringHexToSlice("7F200F0203"));
    __expect_lazy("[o] save immutable (tail), skip (bits16), load ab");    // not lazy match because of nested o.toCell()
    match (o.ab) {
        A => {
            return o.toCell().beginParse().remainingBitsCount();
        }
        B => throw 0xFFFF
    }
}

@method_id(211)
fun demo211() {
    var o = lazy Counter32Reset.fromSlice(stringHexToSlice("34567890000000FF02030405"));
    var f3 = -1;
    __expect_lazy("[o] save immutable (tail), load initial, skip (bits8), load f3");
    if (o.initial > 100) {
        f3 = o.f3;
    }
    assert(o.toCell().beginParse().bitsEqual(stringHexToSlice("34567890000000FF02030405")), 100);
    return o.toCell();
}

@method_id(212)
fun demo212() {
    var o = lazy Counter32Reset.fromSlice(stringHexToSlice("34567890000000FF02030405"));
    __expect_lazy("[o] load initial (bits8) f3, save immutable (tail), load f4");
    if (o.initial > 100 && o.initial < 10000) {
        o.f3 = o.f4;
    }
    return o.toCell().beginParse().bitsEqual(stringHexToSlice("34567890000000FF02040405"));
}

@method_id(213)
fun demo213(eval: bool) {
    var c = lazy ComplexWithPoint.fromSlice(stringHexToSlice("01080903FFFF"));  // more than needed
    if (eval && 10>3) {
        __expect_lazy("[c] load (bits8) p, save immutable (tail)");
        c.p.y += 2;
        var s = c.toCell().beginParse();
        return (s.remainingBitsCount(), s.skipBits(8+8).preloadUint(8));
    } else {
        throw 123;
    }
}

@method_id(214)
fun demo214() {
    val p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] save immutable (tail), load x");
    var s = p.makeCellWrapped().beginParse();
    return (s.loadUint(16), s.remainingBitsCount());
}

@method_id(215)
fun demo215() {
    val p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    var s = Point.makeCell(p).beginParse();
    return (s.loadUint(16), s.remainingBitsCount());
}

@method_id(216)
fun demo216() {
    var st = lazy WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
    __expect_lazy("[st] load (bits1) seqno, save immutable (tail), load walletId");
    st.seqno += 1;
    val w = st.walletId;
    st.save();
    return (contract.getData().hash() & 0xFFFF, w);
}

@method_id(217)
fun demo217() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");  // because call mutating method
    return p.incXAndMakeCell().beginParse().loadUint(16);
}

@method_id(218)
fun demo218() {
    var o = lazy WithRefs.fromCell(WithRefs {
        r1: (1 as int32).toCell(),
        f2: 2,
        r3: Point{x: 10, y: 20}.toCell(),
        f4: 4,
    }.toCell());
    __expect_lazy("[o] load (cell?) f2, save immutable (tail), load r3");
    val (f2, pointY) = (o.f2, o.r3.load().y);
    o.f2 *= 10;
    return (f2, pointY, o.toCell().hash() & 0xFFFF, o.f2);
}


@method_id(300)
fun demo300() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] save immutable (tail)");
    return p.toCell().beginParse().bitsEqual(stringHexToSlice("0102"));
}

@method_id(301)
fun demo301() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] save immutable (tail), load x");
    var x = p.x;
    return (x, p.toCell().beginParse().bitsEqual(stringHexToSlice("0102")));
}

@method_id(302)
fun demo302() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    p.x = 15;
    return p.toCell().beginParse().bitsEqual(stringHexToSlice("0F02"));
}

@method_id(303)
fun demo303() {
    var st = lazy WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
    __expect_lazy("[st] load isSignatureAllowed seqno, save immutable (tail)");
    st.isSignatureAllowed = false;
    st.seqno += 1;
    return st.toCell().hash();
}

@method_id(304)
fun demo304() {
    var st = lazy WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
    __expect_lazy("[st] load isSignatureAllowed seqno, save immutable (tail), load walletId publicKey extensions");
    var st2 = st;
    (st.isSignatureAllowed, st.seqno) = (false, st.seqno + 1);
    return (st2, st.toCell().hash());
}

@method_id(305)
fun demo305(setX: int?) {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x, save immutable (tail)");
    if (setX != null) {
        p.x = setX;
    }
    return p.toCell().beginParse().loadUint(8);
}

@method_id(306)
fun demo306(setX: int?) {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    if (setX != null) {
        p.x = setX;
    }
    return p;
}

@method_id(307)
fun demo307() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    var cc = (p as PointOrPoint3d).toCell();
    assert(cc.beginParse().remainingBitsCount() == 1+8+8, 100);
    return cc.hash();
}

@method_id(308)
fun demo308() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    var cc = (p as PointOrPoint3d).toCell();
    var ll = lazy cc.load();
    assert(cc.beginParse().remainingBitsCount() == 1+8+8, 100);
    __expect_lazy("[ll] lazy match");
    match (ll) {
        Point => {
            assert(p.x == 1, 100);
            p.y += 1;
            __expect_lazy("[ll] load x y");
            ll.y += 1;
            assert(p.y == ll.y, 101);
            ll.y -= 1;
            return (p, (ll as PointOrPoint3d).toCell().hash());
        }
        Point3d => throw 0
    }
}

@method_id(309)
fun demo309() {
    var st = lazy WalletStorage.load();
    __expect_lazy("[st] load (bits65) publicKey, save immutable (tail)");
    st.publicKey += 1;
    return st.toCell();
}

@method_id(310)
fun demo310() {
    val cell = NotFixedWidthStorage {
        f1: 10,
        f2: null,
        f3: 20,
        f4: true,
        f5: createEmptyCell(),
        f6: 10,
        f7: createAddressNone(),
        f8: 9,
        f9: "" as bits200,
        f10: ("" as bits400, ()),
        f11: 0,
    }.toCell({skipBitsNValidation: true});
    var st = lazy cell.load();
    var sm = lazy cell.load();
    var sk = lazy cell.load();
    __expect_lazy("[st] load (bits8) (int8?) (bits9) (cell) f6, save immutable (tail), skip (any_address), load f8");
    match (st.f6) { int8 => st.f6 += 90, bool => throw 123 };
    __expect_lazy("[sm] load f5");
    sm.f5;  // previous (data-only) fields are not needed to reach a ref
    __expect_lazy("[sk] skip (bits8) (int8?) (bits9) (int8 | bool), load f7");
    sk.f7;  // ref (f5) is not needed to be skipped to reach data
    return (st.toCell().hash() & 0xFFFFFFFF, st.f8);
}

@method_id(311)
fun demo311() {
    var st = lazy WalletStorage.load();
    __expect_lazy("[st] load isSignatureAllowed (bits320) extensions");
    st.isSignatureAllowed = false;
    st.extensions = null;
    return st.toCell();
}

@method_id(312)
fun demo312() {
    val addr = address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF");
    val correct = beginCell().storeInt(0, 8).storeAddress(addr).storeInt(-1, 8).endCell();
    var st = lazy WithInternalAddress.fromCell(correct);
    __expect_lazy("[st] skip (bits8) (address), load f3");
    val f3 = st.f3;
    
    val wrong = beginCell().storeInt(0, 8).storeAddressNone().storeAddress(addr).storeInt(-1, 8).endCell();
    var st2 = lazy WithInternalAddress.fromCell(wrong);
    // check that `address` (st2.f2) is validated even though unused
    try {
        return (f3, 0, st2.f3);
    } catch (ex) {
        return (f3, ex, 0 as int8);
    }
}


@method_id(400)
fun demo400() {
    var t = createEmptyTuple();
    t.push(stringHexToSlice("0102"));
    t.push(stringHexToSlice("0304"));
    t.push(stringHexToSlice("0506"));
    var corrX: int = -1;
    while (t.size() && corrX == -1) {
        var p = lazy Point.fromSlice(t.pop());
        __expect_lazy("[p] load x y");
        if (p.y == 6) {
            corrX = p.x;
        }
    }
    if (corrX == -1) {
        throw 123;
    }
    return corrX;
}

fun helper401(s: slice, fakeHandle: bool): never {
    val msg = lazy MsgFullCounter.fromCell(beginCell().storeSlice(s).endCell());
    match (msg) {
        CounterDecrementBy1 => throw 0,
        CounterReset => throw 0,
        CounterDecrement => {
            var newX = 100;
            __expect_lazy("[msg] load byValue");
            if (!fakeHandle) {
                newX -= msg.byValue;
            } else {
                newX += msg.byValue;
            }
            throw newX;
        }
        CounterIncrement => {
            var newX = 50;
            if (!fakeHandle) {
                __expect_lazy("[msg] load byValue");
                newX += msg.byValue;
            }
            throw newX;
        }
        else => throw 0xFFFF
    }
}

@method_id(401)
fun demo401(s: slice, fakeHandle: bool) {
    try {
        helper401(s, fakeHandle);
    } catch (excNo) {
        return excNo;
    }
}

@method_id(402)
fun demo402(s: slice) {
    try {
        val msg = lazy MsgFullCounter.fromSlice(s, {throwIfOpcodeDoesNotMatch: 0xFFFF});
        __expect_lazy("[msg] lazy match");
        match (msg) {
            CounterDecrementBy1 => {}
            CounterReset => {}
            CounterDecrement => {}
            CounterIncrement => {}
            // without else, 0xFFFF is thrown by default
        }
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(403)
fun demo403(s: slice) {
    val msg = lazy MsgFullCounter.fromSlice(s);
    var st = lazy loadWalletStorage();
    var t = createEmptyTuple();
    __expect_lazy("[st] skip (bits1), load seqno walletId");
    match (msg) {
        CounterIncrement => t.push(st.seqno),
        CounterDecrementBy1 => throw 0,
        CounterReset => throw 0,
        CounterDecrement => {
            t.push(st.walletId);
            if (t.size() == 1) {
                __expect_lazy("[msg] load byValue");
                t.push(msg.byValue);
            }
        }
    }
    return t;
}

@method_id(404)
fun demo404(s: slice) {
    var msg = lazy MyMsg32.fromSlice(s, {throwIfOpcodeDoesNotMatch: 0xFFFF});
    var curCounter = 10;
    __expect_lazy("[msg] lazy match");
    match (msg) {
        Counter32Increment => {
            __expect_lazy("[msg] load byValue");
            curCounter += msg.byValue;
        }
        Counter32DecrementBy1 => {
            curCounter -= 1;
        }
        Counter32Reset => {
            __expect_lazy("[msg] load initial, skip (bits8), load f3");
            assert (msg.initial >= 0) throw 0xFF;
            if (msg.initial > 100) {
                msg.initial = 100;
            }
            curCounter = msg.initial;
            if (msg.f3 == 100) {
                curCounter *= 2;
            }
        }
    }
    return curCounter;
}


@method_id(405)
fun demo405(s: slice) {
    var sum = 0;
    try { var msg = lazy AutoPrefixedBin2.fromSlice(s); match (msg) {
        AutoP3 => {
            sum = -1;
        }
        AutoP1 => {
            __expect_lazy("[msg] load f1 f2");
            sum = msg.f1 + msg.f2;
        }
        AutoP2 => {
            __expect_lazy("[msg] load f1 f2 f3");
            sum = msg.f1;
            sum += msg.f2;
            if (sum > 0) {
                sum += msg.f3;
            }
        }
    } } catch { sum = -2 }
    return sum;
}


@method_id(406)
fun demo406(s: slice) {
    try {
    match (val msg = lazy ShortPrefixed.fromSlice(s)) {
        ShortP1 => {
            return 1;
        }
        ShortP2 => {
            __expect_lazy("[msg] skip (bits8), load f2");
            var sum = 2 + msg.f2;
            return sum;
        }
        else => { throw 9999; }
    }
    } catch(excno) { return excno }
}

@method_id(407)
fun demo407(s: slice): int {
    match (val msg = lazy NotEqPrefixed.fromSlice(s)) {
        NotEqPrefixedS1 => return msg.f,
        NotEqPrefixedS2 => return msg.f,
        NotEqPrefixedS3 => return msg.f,
    }
    throw 0xFF;
}

@method_id(408)
fun demo408() {
    var o = lazy Outer.fromSlice(stringHexToSlice("0102030408FF7FFFFF"));
    __expect_lazy("[o] skip (bits16), load inner2 wa");
    if (10>3) {
        o.inner2.i2 = 10;
    }
    return o.wa.ab.toCell().beginParse().remainingBitsCount();
}

@method_id(409)
fun demo409() {
    var o = lazy Counter32Reset.fromSlice(stringHexToSlice("34567890000000FF02030405"));
    var f3 = -1;
    __expect_lazy("[o] load initial, skip (bits8), load f3");
    if (o.initial > 100) {
        f3 = o.f3;
    }
    return (o.initial, f3);
}

@method_id(410)
fun demo410(s: slice) {
    val msg = lazy MsgFullCounter.fromSlice(s);
    // via codegen, check that "return" is implicitly added, and IFJMP is generated
    match (msg) {
        CounterIncrement => { debug.print(1) }
        CounterDecrement => { debug.dumpStack() }
        CounterReset => { random.setSeed(10) }
        CounterDecrementBy1 => { reserveToncoinsOnBalance(10, 1) }
        else => { /* do nothing, "just accept tons" */ }
    }
}

@method_id(411)
fun demo411(s: slice) {
    val msg = lazy AOrB.fromSlice(s);
    // the same, but for either
    match (msg) {
        A => { debug.print(1) }
        B => { debug.dumpStack() }
    }
}

@method_id(412)
fun demo412() {
    val msg = lazy AOrB.fromSlice("");
    match (msg) {
    }
    return 123;
}


fun main() {
    val s = stringHexToSlice("0102");
    var p = lazy Point.fromSlice(s);
    __expect_lazy("[p] skip (bits8), load y");
    return (p.y, s.remainingBitsCount());   // s not changed
}


/**
@testcase | 0   |                 | 2 16
@testcase | 101 |                 | 1
@testcase | 102 | 0               | 2
@testcase | 103 |                 | 8
@testcase | 104 |                 | 1 4
@testcase | 105 | x{01041_}       | 1 8
@testcase | 105 | x{01820304}     | 1 4
@testcase | 106 | x{01041_}       | -1 (null)
@testcase | 106 | x{01820304}     | 0 2
@testcase | 107 |                 | 1 2
@testcase | 108 |                 | 1 15 1 2 3 4 5 6 7 8 9
@testcase | 109 |                 | 0
@testcase | 110 | x{08}           | [ 8 ]
@testcase | 110 | x{88}           | [ -8 ]
@testcase | 111 | x{08} 50        | [ 8 ]
@testcase | 111 | x{88} 50        | [ -8 ]
@testcase | 112 | x{0110}         | 16
@testcase | 112 | x{020f}         | -15
@testcase | 112 | x{03}           | -1
@testcase | 112 | x{03ffff}       | -1
@testcase | 112 | x{04000000FF}   | 255
@testcase | 113 | x{0110}         | 16
@testcase | 113 | x{020f}         | -15
@testcase | 113 | x{03}           | -1
@testcase | 113 | x{03ffff}       | -1
@testcase | 113 | x{04000000FF}   | 255
@testcase | 114 | x{0110}         | 16 1
@testcase | 114 | x{020f}         | [ -15 ] typeid-2
@testcase | 114 | x{03}           | -1 1
@testcase | 114 | x{03ffff}       | -1 1
@testcase | 114 | x{04000000FF}   | [ 255 ] typeid-2
@testcase | 115 | x{08}           | 8 42
@testcase | 115 | x{88}           | 0 2
@testcase | 115 | x{83}           | -1 2
@testcase | 118 | x{08}           | 8
@testcase | 118 | x{88}           | -8
@testcase | 119 | x{0F}           | 15 -1
@testcase | 119 | x{8F0203}       | -1 2
@testcase | 120 | x{010208}       | [ 8 1 ]
@testcase | 120 | x{010288}       | [ -8 1 ]
@testcase | 121 |                 | 22
@testcase | 122 | x{010208}       | 9 1
@testcase | 122 | x{010288}       | -8 1
@testcase | 123 | x{010280}       | 5
@testcase | 124 | x{010283}       | [ -3 2 ]
@testcase | 125 |                 | 127 32 15 2 3 typeid-6 15 3 -1
@testcase | 126 | x{7F0F20}       | 127 (null) (null) 15 typeid-8 32 15 -1
@testcase | 126 | x{7F8F020320}   | 127 15 2 3 typeid-7 32 -1 2
@testcase | 127 |                 | 1 2 -1 255
@testcase | 128 | x{5500}         | -1 -1
@testcase | 128 | x{558F01024}    | 0 2
@testcase | 129 |                 | -1 15
@testcase | 130 |                 | 456
@testcase | 131 |                 | 777 1
@testcase | 132 | -1              | 889 -1
@testcase | 132 | 0               | 888 -1
@testcase | 133 |                 | 18
@testcase | 134 |                 | 15
@testcase | 135 | -1              | 40
@testcase | 135 | 0               | 9
@testcase | 136 | -1              | -1
@testcase | 136 | 0               | 2
@testcase | 137 | x{0102}         | 1
@testcase | 137 | x{03}           | 3
@testcase | 137 | x{1}            | 109
@testcase | 138 | x{0102}         | 2
@testcase | 138 | x{03}           | 109
@testcase | 139 | x{0105} 0       | 5
@testcase | 139 | x{01}   0       | 9
@testcase | 139 | x{0230} 0       | 48
@testcase | 139 | x{0230} -1      | 100
@testcase | 140 | -1              | 4
@testcase | 140 | 0               | 255
@testcase | 141 | -1              | 1
@testcase | 142 | x{010203} 0     | 3
@testcase | 142 | x{880203} 0     | 8
@testcase | 142 | x{0102}   0     | 9
@testcase | 143 |                 | 17
@testcase | 144 | 0               | 1
@testcase | 145 | 0               | 2
@testcase | 145 | -1              | 5
@testcase | 146 |                 | -1
@testcase | 147 |                 | -1 0
@testcase | 148 |                 | 8
@testcase | 149 |                 | 9
@testcase | 150 |                 | 1 16
@testcase | 151 |                 | -1
@testcase | 152 |                 | 32
@testcase | 153 |                 | 0
@testcase | 154 |                 | [ [ 1 2 ] ]
@testcase | 155 |                 | 555
@testcase | 156 | x{0110}         | 16
@testcase | 156 | x{FF10}         | -100
@testcase | 157 |                 | -1
@testcase | 158 |                 | 2 10 255
@testcase | 159 |                 | -1 -1 31 -1 31

@testcase | 200 |                 | 1
@testcase | 201 |                 | 1 2 -1
@testcase | 202 |                 | 127 127 6
@testcase | 203 |                 | 1 2 3 4 5 6 7 8 9
@testcase | 204 |                 | 10 20 3 40 5 6 7 8 9
@testcase | 205 |                 | -1
@testcase | 206 |                 | -1 72
@testcase | 207 |                 | C{11A18FA6FB377116ABE1827A4469944C29484C8B00B33BDC4E560D405D5BFDAD}
@testcase | 208 | x{0F0203}       | 23 24
@testcase | 209 |                 | 23
@testcase | 210 |                 | 40
@testcase | 211 |                 | C{32B95044B7193DA671CFD5822D3CAA23DF5055A7E75622EBCB16D31C23C50AA8}
@testcase | 212 |                 | -1
@testcase | 213 | -1              | 48 11
@testcase | 214 |                 | 258 0
@testcase | 215 |                 | 258 0
@testcase | 216 |                 | 12826 456
@testcase | 217 |                 | 514
@testcase | 218 |                 | 2 20 11636 20

@testcase | 300 |                 | -1
@testcase | 301 |                 | 1 -1
@testcase | 302 |                 | -1
@testcase | 303 |                 | 31704026347210867689619328142399241654002690318751313036781135835273797831937
@testcase | 304 |                 | -1 123 456 0 (null) 31704026347210867689619328142399241654002690318751313036781135835273797831937
@testcase | 305 | null            | 1
@testcase | 305 | 88              | 88
@testcase | 306 | null            | 1 2
@testcase | 306 | 88              | 88 2
@testcase | 307 |                 | 79635787059049818071080046066850883641967849619803682012164346688156057263776
@testcase | 308 |                 | 1 3 79635787059049818071080046066850883641967849619803682012164346688156057263776
@testcase | 309 |                 | C{EE7C06B69DDCEFEBED507FE35F603A9019D4AE79A13F670FE4E08928AF484838}
@testcase | 310 |                 | 3332695883 9
@testcase | 311 |                 | C{16A4757FA412AD5E89418F1263904CD57FA4DF0E73EE8A53DC60F862C72BCD9D}
@testcase | 312 |                 | -1 9 0

@testcase | 400 |                             | 5
@testcase | 401 | x{0110} -1                  | 50
@testcase | 401 | x{020f} 0                   | 85
@testcase | 402 | x{020f}                     | 0
@testcase | 402 | x{ffffffff}                 | 65535
@testcase | 403 | x{0110}                     | [ 123 ]
@testcase | 403 | x{020f}                     | [ 456 15 ]
@testcase | 404 | x{1234567800000010}         | 26
@testcase | 404 | x{23456789}                 | 9
@testcase | 404 | x{345678900000000002030405} | 0
@testcase | 405 | x{01c220_}                  | 15
@testcase | 405 | x{41c20260_}                | 24
@testcase | 405 | x{a0_}                      | -1
@testcase | 405 | x{ffff}                     | -2
@testcase | 406 | x{7}                        | 1
@testcase | 406 | x{40102}                    | 4
@testcase | 406 | x{99}                       | 9999
@testcase | 407 | x{7F}                       | 63
@testcase | 407 | x{1F}                       | 15
@testcase | 407 | x{BF}                       | -1
@testcase | 408 |                             | 24
@testcase | 409 |                             | 255 3
@testcase | 412 |                             | 123

@fif_codegen
"""
  demo101() PROC:<{             //
    x{0102} PUSHSLICE           //  lazyS
    8 PLDI                      //  p.x
  }>
"""

@fif_codegen
"""
  demo103() PROC:<{             //
    x{0102} PUSHSLICE           //  lazyS
    8 LDU
    NIP                         //  lazyS
    8 PLDI                      //  p.y
"""

@fif_codegen
"""
  demo106() PROC:<{             //  s
                                //  lazyS
    PUSHNULL                    //  lazyS x
    SWAP                        //  x lazyS
    8 LDU
    NIP                         //  x lazyS
    1 LDU                       //  x '18 lazyS
"""

@fif_codegen
"""
  demo109() PROC:<{             //
    x{8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_} PUSHSLICE    //  lazyS
    65 LDU
    NIP                         //  lazyS
    256 PLDU                    //  st.publicKey
  }>
"""

@fif_codegen
"""
  demo113() PROC:<{
  //  lazyS
    x{01} SDBEGINSQ
    IF:<{
      8 PLDI
    }>ELSE<{
      x{02} SDBEGINSQ
      IF:<{
        8 PLDI
        NEGATE
      }>ELSE<{
        x{03} SDBEGINSQ
        IF:<{
          DROP
          -1 PUSHINT
        }>ELSE<{
          x{04} SDBEGINSQ
          IFNOTJMP:<{
            63 THROW
          }>
          32 PLDI
        }>
      }>
    }>
  }>
"""

@fif_codegen
"""
  demo134() PROC:<{             //
    x{010f} PUSHSLICE           //  lazyS
    x{01} SDBEGINSQ             //  lazyS '6
    134 THROWIFNOT              //  lazyS
    8 PLDI                      //  cc
  }>
"""

@fif_codegen
"""
  demo136() PROC:<{
    x{0102} PUSHSLICE
    SWAP
    IFJMP:<{
      DROP
      -1 PUSHINT
    }>
    8 LDU
    NIP
    8 PLDI
  }>
"""

@fif_codegen
"""
  demo146() PROC:<{
    x{0000000} PUSHSLICE
    PLDOPTREF
    ISNULL
  }>
"""

@fif_codegen
"""
  demo201() PROC:<{
    x{0102} PUSHSLICE
    DUP
    8 LDI
    8 PLDI
    s2 PUSH
    NEWC
    STSLICE
    ENDC
    s3 PUSH
    NEWC
    STSLICE
    ENDC
    s0 s4 XCHG
    NEWC
    STSLICE
    ENDC
"""


@fif_codegen
"""
  demo203() PROC:<{             //
    x{010203040506070809} PUSHSLICE     //  lazyS
    DUP                         //  '14 lazyS
    8 PLDU                      //  '14 o.f1
    1 EQINT                     //  '14 '18
    IFJMP:<{                    //  '14
      NEWC                      //  '14 b
      STSLICE                   //  b
      BTOS                      //  s
"""

@fif_codegen
"""
  demo206() PROC:<{             //
    x{010203040506070809} PUSHSLICE     //  lazyS
    DUP                         //  '14 lazyS
    8 LDU                       //  '14 o.f1 lazyS
    8 LDU                       //  '14 o.f1 o.f2 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 o.f4 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 o.f4 o.f5 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 o.f4 o.f5 o.f6 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 o.f4 o.f5 o.f6 o.f7 lazyS
    8 LDU                       //  '14 o.f1 o.f2 o.f3 o.f4 o.f5 o.f6 o.f7 o.f8 lazyS
    8 PLDU                      //  '14 o.f1 o.f2 o.f3 o.f4 o.f5 o.f6 o.f7 o.f8 o.f9
"""

@fif_codegen
"""
  demo215() PROC:<{
    x{0102} PUSHSLICE
    8 LDI
    8 PLDI
    SWAP
    NEWC
    8 STI
    8 STI
    BTOS
"""

@fif_codegen
"""
  demo302() PROC:<{
    x{0102} PUSHSLICE
    8 LDI
    NIP
    NEWC
    x{0f} STSLICECONST
    STSLICE
"""

@fif_codegen
"""
  demo311() PROC:<{             //
    777 PUSHINT                 //  '6=777
    contract.getFakeData() CALLDICT       //  '7
    CTOS                        //  lazyS
    1 LDI                       //  '13 lazyS
    NIP                         //  lazyS
    320 PUSHINT                 //  lazyS '14=320
    PLDSLICEX                   //  '15
    PUSHNULL                    //  '15 st.extensions
    NEWC
    b{0} STSLICECONST           //  '15 st.extensions b
    s1 s2 XCHG                  //  st.extensions '15 b
    STSLICE                     //  st.extensions b
    STOPTREF                    //  b
    ENDC                        //  '21
  }>
"""

@fif_codegen
"""
  demo404() PROC:<{
    //  lazyS
    x{12345678} SDBEGINSQ
    IF:<{
      32 PLDI
      10 ADDCONST
    }>ELSE<{
      x{23456789} SDBEGINSQ
      IF:<{
        DROP
        9 PUSHINT
      }>ELSE<{
        x{34567890} SDBEGINSQ
        IFNOTJMP:<{
          16 PUSHPOW2DEC
          THROWANY
        }>
        32 LDU
        8 LDU
        NIP
        8 PLDI
        OVER
        100 GTINT
        IF:<{
          100 PUSHINT
          s2 POP
        }>
        100 EQINT
        IF:<{
          1 LSHIFT#
        }>
      }>
    }>
  }>
"""

@fif_codegen
"""
  demo410() PROC:<{
    // lazyS
    x{01} SDBEGINSQ
    IFJMP:<{
      ...
    }>
    x{02} SDBEGINSQ
    IFJMP:<{
      DROP
      DUMPSTK
    }>
    x{04} SDBEGINSQ
    IFJMP:<{
      ...
    }>
    x{03} SDBEGINSQ
    NIP
    IFJMP:<{
      ...
      RAWRESERVE
    }>
  }>
"""

@fif_codegen
"""
  demo411() PROC:<{             //  s
                                //  lazyS
    1 PLDU                      //  '10
    IFJMP:<{                    //
      DUMPSTK                   //
    }>                          //
    1 PUSHINT                   //  '11=1
    s0 DUMP DROP                //
  }>
"""

@fif_codegen DECLPROC pushToGlobalTup<Point>()
 */
