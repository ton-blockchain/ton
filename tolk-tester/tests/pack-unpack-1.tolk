struct JustInt32 {
    value: int32;
}

struct Point {
    x: int32;
    y: int32;
}

@method_id(101)
fun test1(value: int) {
    var t: JustInt32 = { value };
    var c = t.toCell();
    var t2 = c.load();
    __expect_type(c, "Cell<JustInt32>");
    __expect_type(t2, "JustInt32");
    if (0) {
        var rawCell: cell = c;           // Cell<Something> is assignable to raw cell
        contract.setData(t.toCell());    // (for this purpose, since we don't have overloads)
    }

    var t3 = Cell<JustInt32>.load(c);
    var t4 = JustInt32.fromCell(c);
    var t5 = JustInt32.fromSlice(c.beginParse());
    return (t2.value, t3.value, t4.value, t5.value);
}

@method_id(102)
fun test2() {
    var t: JustInt32 = { value: 10 };
    var c = t.toCell();
    var s = c.tvmCell.beginParse();
    __expect_type(s.skipAny<JustInt32>(), "slice");
    s.assertEnd();
    return true;
}

@method_id(103)
fun test3() {
    var yy = 20;
    var p: Point = { x: 10, y: yy };
    var c = p.toCell();         // p is constant, storing its fields are joined into a single STI
    var s = c.beginParse();
    s.skipAny<Point>();         // skipping is also merged to skip 64 bits
    return s.remainingBitsCount();
}

@method_id(104)
fun test4() {
    var s = stringHexToSlice("000000010000000200000003");
    var p = Point.fromSlice(s, {assertEndAfterReading: false});     // does not mutate s
    return (p, s.remainingBitsCount());
}

@method_id(105)
fun test5() {
    var s = stringHexToSlice("000000010000000200000003");
    var p: Point = s.loadAny();         // mutates s
    return (p, s.remainingBitsCount());
}

@method_id(106)
fun test6() {
    var b = beginCell().storeInt(1, 32);
    var p: Point = { x: 10, y: 20 };
    b.storeAny(p);
    var s = b.endCell().beginParse();
    return (s.remainingBitsCount(), b.bitsCount(), Point.fromSlice(s.getLastBits(64)), s.remainingBitsCount());
}

// this function is used in serialization codegen, it's not exposed to stdlib;
// it constructs "x{...}" slices for SDBEGINSQ, used for opcode matching;
// here we write some unit tests and fif codegen tests for it
fun slice.tryStripPrefix(mutate self, prefix: int, prefixLen: int): bool
    builtin;

@method_id(107)
fun test7(s: slice) {
    if (s.tryStripPrefix(0x10, 32)) { return (1, s.remainingBitsCount()); }
    if (s.tryStripPrefix(0x40, 31)) { return (2, s.remainingBitsCount()); }
    if (s.tryStripPrefix(0xFF18, 16)) { return (3, s.remainingBitsCount()); }
    if (s.tryStripPrefix(0b1001, 8)) { return (4, s.remainingBitsCount()); }
    if (s.tryStripPrefix(0b01, 3)) { return (5, s.remainingBitsCount()); }
    return (6, s.remainingBitsCount());
}

fun main(c: cell) {
    c as Cell<Point>;
    (c as Cell<Point>) as cell;
    (c as Cell<Point?>) as cell?;
    __expect_type((c as Cell<Point?>?)!, "Cell<Point?>");
    c = c as Cell<Point>;           // Cell<T> implicitly converts to cell
}

/**
@testcase | 101 | 10            | 10 10 10 10
@testcase | 102 |               | -1
@testcase | 103 |               | 0
@testcase | 104 |               | 1 2 96
@testcase | 105 |               | 1 2 32
@testcase | 106 |               | 96 96 10 20 96
@testcase | 107 | x{00000080}   | 2 1
@testcase | 107 | x{09332}      | 4 12
@testcase | 107 | x{2}          | 5 1
@testcase | 107 | x{0234}       | 6 16

@fif_codegen
"""
  test3 PROC:<{
    20 PUSHINT                  //  yy=20
    10 PUSHINT                  //  p.y=20 p.x=10
    NEWC                        //  p.y=20 p.x=10 b
    32 STI                      //  p.y=20 b
    32 STI                      //  b
    ENDC                        //  c
    CTOS                        //  s
    32 PUSHINT                  //  s '15=32
    SDSKIPFIRST                 //  s
    32 PUSHINT                  //  s '16=32
    SDSKIPFIRST                 //  s
    SBITS                       //  '17
  }>
"""

@fif_codegen x{00000010} SDBEGINSQ
@fif_codegen b{0000000000000000000000001000000} SDBEGINSQ
@fif_codegen x{ff18} SDBEGINSQ
@fif_codegen x{09} SDBEGINSQ
@fif_codegen b{001} SDBEGINSQ
 */
