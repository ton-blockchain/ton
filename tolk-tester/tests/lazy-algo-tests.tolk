struct A { a1: int7; a2: int8; a3: int8; }
struct B { b1: int7; b2: int8; b3: int8; }
type AOrB = A | B;

@noinline
fun A.getA1(self) {
    return self.a1;
}

fun AOrB.doMatch(self) {
    __expect_inline(true);
    match (self) {
        A => {}
        B => {}
    }
}

struct Fields9 {
    f1: uint8; f2: uint8; f3: uint8;
    f4: uint8; f5: uint8; f6: uint8;
    f7: uint8; f8: uint8; f9: uint8;
}

fun uint8.doNothing(self) {}

@noinline
fun Fields9.method(self) {}

fun Fields9.useAll(self) {
    __expect_inline(true);
    if (true) { self.method() }
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
    f1: int32;
    f2: int32;
    f3: int32;
    f4: int32;
    f5: int32;
    ab: A | B;
}

fun WithEitherABEnd.checkF3(self, against: int) {
    assert(self.f3 == against, 100);
}

fun WithEitherABEnd.doMatch(self) {
    __expect_inline(true);
    match (self.ab) {
        A => {}
        B => {}
    }
}

fun rand() { return random.uint256() }

struct Point {
    x: int8;
    y: int8;
}

fun Point.assignX(mutate self, x: int) {
    self.x = x;
}

fun Point.getX(self) {
    return self.x;
}

fun Point.getXNoInline(self) {
    __expect_inline(false);    // due to returns in the middle
    if (10>3) {
        return self.x;
    }
    throw 123;
}

fun Point.getCoord(self, getX: bool) {
    return getX ? self.x : self.y;
}

fun Point.getCoordWrapper(self, getX: bool) {
    if (10<3) { throw 123 }
    return self.getCoord(getX)
}

fun getXOfPoint(p: Point) {
    return p.x
}

struct Point3d {
    x: int32;
    y: int32;
    z: int32;
}

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

@overflow1023_policy("suppress")
struct NotFixedWidthStorage {
    f1: int8;
    f2: int8?;
    f3: int8;
    f4: bool;
    f5: cell;
    f6: int8 | bool;
    f7: address?;
    f8: int8;
    f9: bits200;
    f10: (bits400, ());
    f11: coins;
}

struct WithTensor {
    a1: int8;
    t: (int8, int8);
    a3: int8;
}

struct WithMaybeTensor {
    a1: int8;
    a2: int8;
    t: (int8, int8)?;
}

struct MixedDataAndRefs {
    i1: int8;
    i2: bool;
    a1: address;
    e: ();
    r1: cell;
    r2: ((), cell);
    i4: Point;
    rm: cell?;
    r4: Cell<MixedDataAndRefs>?;
}

struct (0x01) CounterIncrement { byValue: int8 }
struct (0x02) CounterDecrement { byValue: int8 }
struct (0x03) CounterDecrementBy1 { }
struct (0x04) CounterReset { initialValue: int32 }
type MsgFullCounter = CounterIncrement | CounterDecrement | CounterDecrementBy1 | CounterReset;

struct ItemState {
    ownerAddress: address;
    content: cell;
    auction: cell?;
    royaltyParams: cell;
}

struct HasIncrementEnd {
    a: int8;
    inc: CounterIncrement;
}

struct HasUnusedAddressInTensor {
    f1: (int8, bits267, int8)           // can be safely skipped as bits283
    f2: (int8, address, int8)           // can NOT (even fixed-size) since `address` needs validation
    f3: int8
}


fun loadWalletStorage() {
    return WalletStorage.fromSlice(stringHexToSlice("8000003d800000e4000000000000000000000000000000000000000000000000000000000000000020_"));
}

fun WalletStorage.load() {
    return WalletStorage.fromCell(createEmptyCell());
}



fun algo1() {
    val o = lazy Fields9.fromSlice("");
    if (rand()) {
        __expect_lazy("[o] load f1");
        o.f1;
    }
}

fun algo2() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1");
    if (rand()) {
        o.f1;
    } else {
        o.f1 + o.f1;
    }
}

fun algo3() {
    val o = lazy Fields9.fromSlice("");
    if (rand()) {
        return;
    }
    __expect_lazy("[o] load f1");
    o.f1.doNothing();
}

fun algo4() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1 f2");
    if (rand()) {
        o.f1;
    } else {
        o.f2;
    }
}

fun algo5() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] skip (bits8), load f2, skip (bits8), load f4");
    o.f4;
    o.f2;
}

fun algo6() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits40), load f8");
    if (rand()) {
        o.f8;
    }
    o.f2;
    o.f1 * o.f1 + o.f1 + o.f2 * o.f2;
}

fun algo7() {
    val o = lazy Fields9.fromSlice("");
    if (rand()) {
        __expect_lazy("[o] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
        o;
    } else {
    }
}

fun algo8() {
    if (rand()) {
        val o: Fields9 = lazy Fields9.fromSlice("");
        __expect_lazy("[o] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
        o.method();
    } else {
    }
}

fun algo9() {
    val p = lazy Point3d.fromCell(createEmptyCell());
    __expect_lazy("[p] load x y z");
    if (p.x && p.y) {
        p.x;
        p.y;
    } else {
        p.z;
    }
}

fun algo10() {
    val p = lazy Point3d.fromSlice("");
    __expect_lazy("[p] load x, skip (bits32), load z");
    p.z;
    if (rand()) {
        p.x;
        return;
    }
}

fun algo11() {
    val p = lazy Point3d.fromSlice("");
    var num = 0;
    if (10 > 2) {
        num += 1;
    }
    __expect_lazy("[p] load x");
    rand()
        ? p.x as int
        : p.x + num;
}

fun algo12() {
    try {
        val p = lazy Point3d.fromSlice("");
        try {}
        catch {}
        assert(10>3, 1);
        __expect_lazy("[p] skip (bits32), load y");
        p.y * p.y;
    } catch {
    }
}

fun algo13() {
    val o = lazy Fields9.fromSlice("");
    if (10 > 3) {
        __expect_lazy("[o] load f1");
        assert(o.f1 > 0) throw 400;
    }
}

fun algo14() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x");
    while (rand()) {
        p.x;
    }
}

fun algo15() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] skip (bits8), load y");
    do {
        p.y;
    } while (rand());
}

fun algo16() {
    val p = lazy Point3d.fromSlice("");
    __expect_lazy("[p] load x, skip (bits32), load z");
    do {
        p.x;
    } while (p.z);
}

fun algo17() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");
    repeat(p.x) {
        p.y;
    }
}

fun algo18() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits16), load f5 f6");
    if (o.f1) {
        if (o.f2) {
            throw o.f6
        }
    }
    else { o.f5 }
}

fun algo19() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");
    var p2 = p;
    return (p2.x, p2.y);
}

fun algo20() {
    val c = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[c] skip (bits8), load p");
    return c.p.x;
}

fun algo21() {
    val c = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[c] load f1 p");
    if (c.f1 > 0) {
        return c.p.y;
    }
    throw 0;
}

fun algo22() {
    var w = lazy WithTensor.fromSlice("");
    __expect_lazy("[w] skip (bits8), load t");
    w.t.0 = 10;
}

fun algo23() {
    var w = lazy WithMaybeTensor.fromSlice("");
    __expect_lazy("[w] skip (bits8), load a2 t");
    if (w.t == null) {
        return w.a2;
    }
    return w.t.0;
}

fun algo24() {
    val w = lazy WalletStorage.fromSlice("");
    try {
        __expect_lazy("[w] skip (bits1), load seqno");
        return w.seqno as int;
    } catch (excno) {
        return excno;
    }
}

fun algo25() {
    val w = lazy WalletStorage.fromSlice("");
    __expect_lazy("[w] skip (bits1), load seqno");
    try {
        return w.seqno as int;
    } catch (excno) {
        if (excno > 0) {
            return w.seqno as int;
        }
        return excno;
    }
}

fun algo26() {
    val w = lazy WalletStorage.fromSlice("");
    __expect_lazy("[w] skip (bits1), load seqno");
    try {
        return w.seqno + 1;
    } catch (excno) {
    }
    return w.seqno - 1;
}

fun algo27() {
    val w = lazy WalletStorage.fromSlice("");
    __expect_lazy("[w] skip (bits1), load seqno walletId");
    try {
        return w.seqno + 1;
    } catch (excno) {
    }
    if (10 > 3) {
        return w.walletId as int;
    }
    return -1;
}

fun algo28() {
    val w = lazy WalletStorage.fromSlice("");
    try {
        try {
            return 1;
        } catch {
            __expect_lazy("[w] skip (bits1), load seqno walletId");
            return w.walletId as int;
            w.seqno;
        }
    } catch (excno) {
        return -1;
    }
}

fun algo29() {
    val w = lazy WalletStorage.fromSlice("");
    try {
        __expect_lazy("[w] skip (bits1), load seqno walletId");
        try {
            return 1;
        } catch {
            return w.walletId as int;
        }
        w.seqno;
    } catch (excno) {
        return -1;
    }
}

fun algo30() {
    val w = lazy WalletStorage.fromSlice("");
    if (true) {
        try {
            __expect_lazy("[w] skip (bits1), load seqno walletId");
            try {
                return 1;
            } catch {
                return w.walletId as int;
            }
            w.seqno;
        } catch (excno) {
            return -1;
        }
    }
}

fun algo31() {
    val w = lazy WalletStorage.fromSlice("");
    if (false) {}
    else {
        __expect_lazy("[w] skip (bits65), load publicKey");
        while (true) {
            w.publicKey
        }
    }
}

fun algo32() {
    val w = lazy WalletStorage.fromSlice("");
    __expect_lazy("[w] load isSignatureAllowed");
    match (true) {
        true => { w.isSignatureAllowed }
        false => {}
    }
}

fun algo33() {
    val p = lazy ComplexWithPoint.fromSlice("");
    try { if(true) {} else {
        __expect_lazy("[p] skip (bits8), load p");
        p.p.x;
    }} catch{}
}

fun algo34() {
    val p = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[p] skip (bits8), load p f3");
    if (p.p.x) { }
    else { p.f3 }
}

fun algo35() {
    var p = lazy ComplexWithPoint.fromSlice("");
    try {
        __expect_lazy("[p] load f1 p");
        if (p.p.x) { }
        else { p.p = {x:1, y:2} }
        if (true) {
            try {} catch (excno) { match (excno + p.f1) { } }
        }
    } catch {}
}

fun algo36() {
    var p1 = lazy ComplexWithPoint.fromSlice("");
    var p2 = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[p1] skip (bits8), load p");
    try {
        __expect_lazy("[p2] load f1 p f3");
        match (true) {
            true => { p2.f3 }
            else => p2.p.x ? p2.f1 : p1.p.x
        }
    } catch { p1.p.y -= 1 }
}

fun algo37() {
    var p1 = lazy ComplexWithPoint.fromSlice("");
    var p2 = lazy ComplexWithPoint.fromSlice("");
    try {
        if (10>3) {
            __expect_lazy("[p2] load f1 p f3");
            var p1 = p2;
        }
    } catch {
        __expect_lazy("[p1] skip (bits8), load p");
        p1.p.y -= 1
    }
}

fun algo38() {
    var p1 = lazy ComplexWithPoint.fromSlice("");
    var p2 = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[p1] skip (bits8), load p f3");
    try {
        if (10>3) {
            __expect_lazy("[p2] skip (bits24), load f3");
            p1.f3 = p2.f3;
        }
    } catch {
        p1.p.y -= 1
    }
}

fun algo39() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x");
    return p.getX();
}

@method_id(140)
fun algo40() {
    val p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    return p.getCoord(false);
}

@method_id(141)
fun algo41(getX: bool) {
    val p = lazy Point.fromSlice(stringHexToSlice("0102"));
    __expect_lazy("[p] load x y");
    return p.getCoordWrapper(getX);
}

fun algo42() {
    var p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");      // calling a mutating method — not available for lazy
    p.assignX(10);
}

fun algo43() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");      // not a method — not available for lazy (not implemented)
    return getXOfPoint(p);
}

fun algo44() {
    val p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");      // not an inlined method
    return p.getXNoInline();
}

fun algo46() {
    var w = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[w] skip (bits64), load f3, skip (bits64), load ab");
    w.checkF3(0);
    w.doMatch();
}

fun algo47() {
    var f = lazy Fields9.fromSlice("");
    __expect_lazy("[f] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
    f.useAll();
}

fun algo48() {
    var p = lazy Point.fromSlice("");
    contract.getData();
    __expect_lazy("[p] load x y");
    p.forceLoadLazyObject();
}

fun algo49(forceLoad: bool) {
    val l = lazy Fields9.fromSlice("");
    if (forceLoad) {
        __expect_lazy("[l] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
        try { l.forceLoadLazyObject() }
        catch { l.f2 }
    }
}

fun algo50() {
    var st = lazy NotFixedWidthStorage.fromSlice("");
    __expect_lazy("[st] skip (bits8) (int8?) (bits9) (int8 | bool) (address?) (bits608), load f11");
    st.f11;
}

fun algo51() {
    var w1 = lazy MixedDataAndRefs.fromSlice("");
    var w2 = lazy MixedDataAndRefs.fromSlice("");
    var w3 = lazy MixedDataAndRefs.fromSlice("");
    var w4 = lazy MixedDataAndRefs.fromSlice("");
    var w5 = lazy MixedDataAndRefs.fromSlice("");
    var w6 = lazy MixedDataAndRefs.fromSlice("");

    __expect_lazy("[w1] load r1");
    w1.r1;
    __expect_lazy("[w2] skip (cell), load r2");
    w2.r2;
    __expect_lazy("[w3] skip (bits9) (address) (cell) (((), cell)) (bits16), load rm");
    w3.rm;
    __expect_lazy("[w4] skip (bits9) (address), load i4");
    w4.i4;
    __expect_lazy("[w5] skip (bits8), load i2, skip (address), load e r1 i4");
    w5.i2; w5.e; w5.r1; w5.i4;
}

fun algo52(cc: Cell<ItemState>?) {
    val itemState = lazy cc!.load();
    __expect_lazy("[itemState] skip (address) (cell) (cell?), load royaltyParams");
    return itemState.royaltyParams;
}

fun algo53() {
    val i = lazy ItemState.fromSlice("");
    __expect_lazy("[i] load content");
    return i.content.beginParse();
}


fun algo100() {
    var o = lazy AOrB.fromSlice("");
    __expect_lazy("[o] lazy match");
    match (o) {
        A => {
            __expect_lazy("[o] load a1, skip (bits8), load a3");
            o.a1;
            __expect_lazy("");
            o.a3
        }
        B => {
            __expect_lazy("[o] load b1 b2");
            o.b2;
            __expect_lazy("");
            o.b1
        }
    }
}

fun algo101() {
    val o = lazy WithEitherABMiddle.fromSlice("");
    __expect_lazy("[o] skip (bits64), load ab, skip (bits32), load f5");
    o.f5;
    o.ab;
}

fun algo102() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits128), load f5 ab");
    o.f5;
    o.ab;
}

fun algo103() {
    val o = lazy WithEitherABMiddle.fromSlice("");
    __expect_lazy("[o] skip (bits64), load ab");
    match (o.ab) {
        A => {}
        B => {}
    }
}

fun algo104() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits160), lazy match ab");
    match (o.ab) {
        A => {}
        B => {}
    }
}

fun algo105() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] load f1 f2 f3, skip (bits64), lazy match ab");
    match (o.ab) {
        A => {
            assert(o.f1 > o.f2, o.f3);
            // __expect_lazy("[o.ab] load a1 a2");
            return o.ab.a1 + o.ab.a2;
        }
        B => {
            // __expect_lazy("[o.ab] load b1 b2 b3");
            val cp = o.ab;
            return cp.b1 + cp.b2;
        }
    }
}

fun algo106() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits96), load f4, skip (bits32), lazy match ab");
    if (rand()) {
        o.f4;
    } else {
        match (o.ab) { A => {} B => {
            o.ab.b2;
        } }
        o.f4;
    }
}

fun algo107() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits160), load ab");
    if (o.ab is B) {}
    match (o.ab) {}
}

fun algo108(): int7 | int8 {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits160), lazy match ab");
    return match (o.ab) {
        A => {
            // __expect_lazy("[o.ab] load a1");
            return o.ab.a1;
        }
        B => {
            // __expect_lazy("[o.ab] skip b1, load b2");
            return o.ab.b2;
        }
    }
}

fun algo109(): int {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits32), load f4, skip (bits32), lazy match ab");
    match (o.ab) {
        A => {
            assert(o.f1 > o.f2, 100);
            // __expect_lazy("[o.ab] load a1");
            return o.f1 + o.ab.a1;
        }
        B => {
            return o.f2 + o.f4;
        }
    }
}

fun algo110() {
    val p = lazy Point3d.fromSlice("");
    __expect_lazy("[p] load x y z");
    match (p.x) {
        1 => p.y,
        2 => p.z,
    }
}

fun algo111() {
    val p = lazy Point3d.fromSlice("");
    __expect_lazy("[p] load x y z");
    match (p.x) {
        1 => 1,
        else => { var x = p.y + p.y }
    }
    p.z;
}

fun algo112() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits8), load f4 f5 f6");
    match (o.f2) {
        uint8 => {
            o.f1 + match(o.f4) { 1 => o.f4, else => o.f5 }
        }
    }
    o.f6;
}

fun algo113() {
    val o = lazy Fields9.fromSlice("");
    __expect_lazy("[o] load f1");
    match (o.f1) {
        uint8 => {
            debug.print(o.f1);
        }
    }
    o.f1;
}

type PointOrPoint3d = Point | Point3d;

fun algo114() {
    val o = lazy PointOrPoint3d.fromSlice("");
    __expect_lazy("[o] lazy match");
    match (o) {
        Point => {
            __expect_lazy("[o] load x y");
            return o.x + o.y;
        }
        Point3d => {
            __expect_lazy("[o] load x y z");
            var p = o;
            return p.x + p.y;
        }
    }
}

fun algo115() {
    val o = lazy WithEitherABMiddle.fromSlice("");
    assert(10>3, 1);
    __expect_lazy("[o] load f1 f2 ab f4");
    o.f1 + o.f2;
    match (o.ab) {
        A => { o.f1 + o.ab.a1 }
        B => { o.f1 + o.f2 + o.ab.b1 }
    }
    o.f4;
}

fun algo116() {
    val o = lazy WithEitherABMiddle.fromSlice("");
    __expect_lazy("[o] load f1 f2 ab f4 f5");
    o.f1 + o.f2;
    match (o.ab) {
        A => { return o.f4 }
        B => throw o.f5
    }
}

fun algo117() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits32), load f4 f5, lazy match ab");
    o.f1 + o.f2;
    match (o.ab) {
        A => return o.f4,
        B => throw o.f5
    }
}

fun algo118() {
    val o = lazy WithEitherABEnd.fromSlice("");
    if (10>3) {
        __expect_lazy("[o] load f1 f2, skip (bits96), lazy match ab");
        match (o.ab) {
            A => { return o.f1 }
            B => { throw o.f2 }
        }
    }
    throw 123;
}

fun algo119() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] load f1 f2, skip (bits96), lazy match ab");
    match (o.ab) {
        A => {
            return o.ab.a2 + o.f2;
        }
        B => {
            assert(o.ab.b1) throw o.f1;
        }
    }
    o.f1;
    return o.f2 as int;
}

fun algo120() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] load f1, skip (bits128), lazy match ab");
    match (o.ab) {
        A => return o.ab.a2 * 1,
        B => return o.ab.b1 << 0,
    }
    o.f1;
}

fun algo121() {
    val o1 = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o1] load f1 f2 f3 f4 f5, lazy match ab");
    match (o1.ab) {
        A => { (o1.f1, o1.f2, o1.f3, o1.f4, o1.f5) }
        B => {}
    }
    val o2 = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o2] load f1 f2 f3 f4 f5 ab");
    match (o2.ab) {
        A => { (o2) }
        B => {}
    }
}


type AOrWithEitherABEnd = A | WithEitherABEnd;

fun algo122() {
    val o = lazy AOrWithEitherABEnd.fromSlice("");
    match (o) {
        A => {
            __expect_lazy("[o] load a1");
            o.a1;
        }
        WithEitherABEnd => {
            __expect_lazy("[o] skip (bits160), load ab");  // not lazy match, nesting not supported
            match (o.ab) {
                A => { o.ab.a1 }
                B => {}
            }
        }
    }
}

fun algo123() {
    val o = lazy AOrB.fromSlice("");
    return match (o) {
        A => {
            __expect_lazy("[o] load a1");
            throw o.a1;
        }
        B => {
            __expect_lazy("[o] load b1 b2");
            assert(10>o.b2) throw 100;
            return o.b1;
        }
    }
}

fun algo124() {
    val o = lazy AOrB.fromSlice("");
    return match (o) {
        A => {
            assert(10>3, 100);
            assert(10>3, 100);
            __expect_lazy("[o] load a1 a2 a3");
            throw (100, o.getA1());
        }
        B => {
            assert(10>3, 100);
            __expect_lazy("[o] load b1 b2 b3");
            return o is B;
        }
    }
}

fun algo127() {
    val w = lazy WithMaybeTensor.fromSlice("");
    __expect_lazy("[w] skip (bits16), load t");
    match (w.t) {
        null => {}
        (int8, int8) => {}
    }
}

fun algo128() {
    val o = lazy AOrB.fromSlice("");
    __expect_lazy("[o] lazy match");
    match (o) {
        A => {
            match (o) { A => {} }
        }
        B => {}
    }
}

fun algo129() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits128), load f5, lazy match ab");
    match (o.ab) {
        A => {}
        B => {
            match (o.ab) { B => { o.f5 } }
        }
    }
}

fun algo130() {
    var o = lazy AOrB.fromSlice("");
    try {
        return 1;
    } catch (excno) {
        __expect_lazy("[o] lazy match");
        match (o) {
            A => {
                __expect_lazy("[o] load a1");
                return o.a1 + excno;
            }
            B => {
                __expect_lazy("[o] load b1 b2 b3");
                if (o.b1 > 0) { o = B { b1: 1, b2: 2, b3: 3 } }
                return o.b2 as int;
            }
        }
    }
}

fun algo131() {
    val o = lazy AOrB.fromSlice("");
    match (o) {
        A => {
            try {
                __expect_lazy("[o] skip (bits15), load a3");
                return o.a3 as int;
            } catch (excno) { return excno }
        }
        B => {
            if (10 > 3) { return -1; }
            else {
                if (10 > 3) { return -1; }
                else {
                    __expect_lazy("[o] load b1");
                    return o.b1 * 0;
                }
            }
        }
    }
}

fun algo132(): int {
    val o = lazy AOrB.fromSlice("");
    match (o) {
        B => {
            __expect_lazy("[o] load b1 b2");
            try {
                return o.b1;
            } catch {
                return o.b2;
            }
        }
        A => {
            __expect_lazy("[o] skip (bits7), load a2 a3");
            try {
                return o.a3;
            } catch {
                throw 123;
            }
            return o.a2;
        }
    }
}

fun algo133(): void {
    val o = lazy WithEitherABEnd.fromSlice("");
    try {}
    catch {
        try {
            __expect_lazy("[o] load f1, skip (bits128), lazy match ab");
            match (o.ab) {
                A => { }
                B => { o.f1 }
            }
        } catch { throw 123 }
    }
}

fun algo134(): void {
    val o = lazy WithEitherABEnd.fromSlice("");
    try { try {
        __expect_lazy("[o] load f1 f2, skip (bits96), lazy match ab");
        try {
            match (o.ab) {
                A => { }
                B => { }
            }
        } catch { throw o.f1 + o.f2 }
    } catch {} } catch {}
}

fun algo135() {
    val o = lazy AOrB.fromSlice("");
    match (o) {
        A => {
            __expect_lazy("[o] load a1 a2 a3");
            o.a1;
            o.forceLoadLazyObject().assertEnd()
        }
        B => {
            __expect_lazy("[o] load b1 b2");
            if (o.b1) {
                o.b2;
            }
        }
    }
}

fun algo136() {
    while (true) {
        val obj = lazy AOrB.fromSlice("");
        __expect_lazy("[obj] lazy match");
        match (obj) {
            A => { __expect_lazy("[obj] load a1"); obj.a1 }
            B => { __expect_lazy("[obj] skip (bits7), load b2"); obj.b2 }
        }
    }
}


fun algo150() {
    val o = lazy WithEitherABEnd.fromSlice("");
    var found = false;
    __expect_lazy("[o] skip (bits160), load ab");
    while (!found) {
        match (o.ab) {
            A => {}
            B => { found = true }
        }
    }
}

fun algo151() {
    val o = lazy WithEitherABEnd.fromSlice("");
    var found = false;
    if (!found) {
        __expect_lazy("[o] skip (bits160), lazy match ab");
        match (o.ab) {
            A => {}
            B => { found = true }
        }
    }
}

fun algo152() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits160), load ab");
    1 + match (o.ab) {
        A => 1,
        B => -1,
    }
}

fun algo153() {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits160), load ab");
    1 && match (o.ab) {
        A => 1,
        B => -1,
    }
}

@method_id(154)
fun algo154() {
    val f1 = lazy Fields9.fromSlice(stringHexToSlice("010203040506070809"));
    val f2 = lazy Fields9.fromSlice(stringHexToSlice("0102"));
    val cb1 = fun(f: Fields9) { return f.f1 + f.f2 };
    val cb2 = fun(f1: int, f2: int) { return f1 + f2 };
    __expect_lazy("[f1] load f1 f2 f3 f4 f5 f6 f7 f8 f9");
    val r1 = cb1(f1);
    __expect_lazy("[f2] load f1 f2");
    val r2 = cb2(f2.f1, f2.f2);
    return (r1, r2);
}

fun algo224(): void {
    val o = lazy WithEitherABEnd.fromSlice("");
    __expect_lazy("[o] skip (bits32), load f2, skip (bits32), load f4 f5 ab");
    match (o.ab) {
        A => {}
        B => { o.f2; }
    }
    match (o.ab) {
        A => {}
        B => { if (rand()) { o.f4; } return; }
    }
    if (o.ab is A) {
        if (rand()) {
            o.f5;
        }
    }
}

fun algo225() {
    __expect_lazy("[c] lazy match");
    match (val c = lazy MsgFullCounter.fromSlice("")) {
        CounterIncrement => {
            var d = lazy WalletStorage.fromSlice("");
            __expect_lazy("[d] load (bits1) seqno, save immutable (tail)");
            d.seqno += 1;
            d.toCell();
            __expect_lazy("[c] load byValue");
            1 + c.byValue;
        }
        CounterDecrement => {
            val c = lazy MsgFullCounter.fromSlice("");
            __expect_lazy("[c] lazy match");
            match (c) {
                CounterIncrement => {}
                CounterDecrement => {}
                CounterDecrementBy1 => {}
                CounterReset => {
                    assert(10>3, 101);
                    __expect_lazy("[c] load initialValue");
                    while (10>3) {
                        if (10>3) { c.initialValue }
                    }
                }
            }
        }
        CounterDecrementBy1 => {
        }
        CounterReset => {
            if (10 > 3) {
                __expect_lazy("[c] load initialValue");
                c.initialValue
            }
        }
        else => throw 123,      // `else` is allowed in a lazy `match`
    }
}

fun algo226() {
    var st = lazy loadWalletStorage();
    __expect_lazy("[st] skip (bits1), load seqno");
    return st.seqno;
}

fun algo250() {
    var f = lazy Fields9.fromSlice("");
    __expect_lazy("[f] skip (bits8), load f2");
    match (f.f2) {
        uint8 => {}
    }
}

fun algo251() {
    var f = lazy Fields9.fromSlice("");
    __expect_lazy("[f] skip (bits8), load f2");
    match (f.f2) {
        0 => 1,
        1 => 0,
        else => throw 0,
    }
}

fun algo252() {
    var p = lazy Point.fromSlice("");
    __expect_lazy("[p] skip (bits8), load y");
    match (p.y) {
        int8 => {}
    }
}

fun algo253() {
    var p = lazy Point.fromSlice("");
    __expect_lazy("[p] load x y");
    match (p.y) {
        0 => p.x,
        1 => 0,
        else => throw 0,
    }
}

fun algo254() {
    var p = lazy Point.fromSlice("");
    __expect_lazy("[p] lazy match");
    match (p) {
        Point => {
            return 1;
        }
    }
}

fun algo255() {
    var p = lazy Point.fromSlice("");
    __expect_lazy("[p] lazy match");
    match (p) {
        Point => {
            __expect_lazy("[p] load x");
            return p.x;
        }
        else => throw 123
    }
}

fun algo256() {
    var p = lazy Point.fromSlice("");
    match (p) {
        Point => {
            __expect_lazy("[p] load x");
            match (p.x) { int8 => {} }
        }
    }
}

fun algo257(): int {
    val k = lazy HasIncrementEnd.fromSlice("");
    __expect_lazy("[k] skip (bits8), load inc");
    match (k.inc) {
        CounterIncrement => {
            __expect_lazy("");
            return k.inc.byValue;
        }
    }
}


fun algo300() {
    val p = lazy AOrB.fromSlice(stringHexToSlice("8F020304"));
    __expect_lazy("[p] lazy match");
    match (p) {
        A => {
            __expect_lazy("[p] skip (bits7), load a2");
            debug.print(p.a2);
        }
        B => {
            __expect_lazy("[p] load b1 b2");
            debug.print(p.b1 + p.b2);
        }
    }
}

@method_id(301)
fun algo301() {
    __expect_lazy("[p] lazy match");
    match (val p = lazy AOrB.fromSlice(stringHexToSlice("8F020304"))) {
        A => {
            __expect_lazy("[p] skip (bits7), load a2");
            return p.a2 as int;
        }
        B => {
            __expect_lazy("[p] load b1 b2");
            return p.b1 + p.b2;
        }
    }
}

fun algo302() {
    var c = Point{x:1,y:2}.toCell();
    var o = lazy Point.fromCell(c);
    __expect_lazy("[o] load x y");
    o.x;
    o.y;
    return o;
}

@method_id(303)
fun algo303() {
    var c = Point{x:1,y:2}.toCell();
    var o = lazy c.load();
    __expect_lazy("[o] load x y");
    return o;
}

@method_id(304)
fun algo304() {
    var o = lazy Point{x:1,y:2}.toCell().load();
    __expect_lazy("[o] load x y");
    return o;
}

@method_id(305)
fun algo305() {
    var p = lazy Point.fromSlice(stringHexToSlice("0102"));
    assert(10>3, 100);
    __expect_lazy("[p] load x y");
    {
        assert(p.x == 1, 100);
        assert(p.y == 2, 100);
    }
    return p;
}

fun algo306() {
    var st = lazy WalletStorage.load();
    __expect_lazy("[st] load (bits1) seqno, save immutable (tail)");
    st.seqno += 1;
    return st.toCell();
}

fun algo307() {
    var c = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[c] load (bits8) p, save immutable (tail)");
    c.p.assignX(10);
    c.toCell();
}

fun algo308() {
    var c = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[c] load (bits8) p, save immutable (tail)");
    c.p.x = 10;
    c.toCell();
}

fun algo309() {
    var c = lazy ComplexWithPoint.fromSlice("");
    __expect_lazy("[c] load f1 p f3");  // object used for writing and toCell, load all for safety
    c = { f1: 10, p: {x:0,y:0}, f3: c.f3 };
    c.toCell();
}

fun algo310() {
    var c = lazy ComplexWithPoint.fromSlice("");
    if (true) {
        __expect_lazy("[c] load (bits8) p, save immutable (tail)");
        debug.print(c.p.y += 2);
        c.toCell();
    }
}

fun algo311() {
    var st = lazy WalletStorage.load();
    __expect_lazy("[st] load isSignatureAllowed (bits320) extensions");
    st.isSignatureAllowed = true;
    st.extensions = null;
    return st.toCell();
}

fun algo312() {
    var st = lazy NotFixedWidthStorage.fromSlice("");
    __expect_lazy("[st] load (bits8) (int8?) (bits9) (cell) (int8 | bool) (address?) f8, save immutable (tail)");
    if (rand()) {
        st.f8 += 1;
    }
    st.toCell();
}

fun algo313() {
    var st = lazy NotFixedWidthStorage.fromSlice("");
    if (rand()) {
        __expect_lazy("[st] load (bits8) (int8?) (bits9) f5, save immutable (tail), skip (int8 | bool) (address?) (bits8), load f9");
        st.f5 = createEmptyCell();
        st.f9;
        st.toCell();
    }
}

fun algo314() {
    var l = lazy HasUnusedAddressInTensor.fromSlice("");
    __expect_lazy("[l] skip (bits283) ((int8, address, int8)), load f3");
    l.f3;
}


fun main() {
}

/**
@testcase | 140 |       | 2
@testcase | 141 | -1    | 1
@testcase | 154 |       | 3 3
@testcase | 301 |       | 17
@testcase | 303 |       | 1 2
@testcase | 304 |       | 1 2
@testcase | 305 |       | 1 2

@fif_codegen
"""
  algo40() PROC:<{
    x{0102} PUSHSLICE
    8 LDI
    NIP
    8 PLDI
  }>
"""
 */
