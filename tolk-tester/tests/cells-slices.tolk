fun builder.store_u32(mutate self, value: int): self {
    return self.storeUint(value, 32);
}
fun builder.storeMessageOp(mutate self, op: int): self
    asm(op self) "32 STU";
fun builder.storeMessageQueryId(mutate self, queryId: int): self
    asm(queryId self) "64 STU";

fun slice.load_u32(mutate self): int {
    return self.loadUint(32);
}

fun slice.myLoadInt(mutate self, len: int): int
    asm(-> 1 0) "LDIX";
fun builder.myStoreInt(mutate self, x: int, len: int): self
    asm(x self len) "STIX";
@pure
fun endCell(b: builder): cell
    asm "ENDC";
@pure
fun beginParse(c: cell): slice
    asm "CTOS";

@method_id(101)
fun test1(): [int,int,int,int,int] {
    var b: builder = beginCell().storeUint(1, 32);
    b = b.storeUint(2, 32);
    b.storeUint(3, 32);
    b = b.store_u32(4);
    b.store_u32(5);

    var cs: slice = b.endCell().beginParse();
    var one: int = cs.loadUint(32);
    var (two: int, three: int) = (cs.loadUint(32), cs.load_u32());
    var four: int = cs.load_u32();
    var five: int = cs.load_u32();

    return [one,two,three,four,five];
}

@method_id(102)
fun test2(): [int,int,int] {
    var b: builder = beginCell().myStoreInt(1, 32);
    b = b.myStoreInt(2, 32);
    // operator ! here and below is used just for testing purposes, it doesn't affect the result
    b!.myStoreInt(3, 32);

    var cs: slice = b.endCell().beginParse();
    var one: int = cs.myLoadInt(32);
    var (two: int, three: int) = (cs.myLoadInt(32), cs.myLoadInt(32));

    return [one,two,three];
}

@method_id(103)
fun test3(ret: int): int {
    val same: int = beginCell()!.storeUint(ret,32).endCell().beginParse().loadUint(32);
    return same;
}

@method_id(104)
fun test4(): [int,int] {
    var b: builder = (beginCell() as builder).myStoreInt(1, 32);
    b = b!.storeInt(2, 32)!.storeInt(3, 32);

    var cs: slice = b.endCell().beginParse();
    var (one, _, three) = (cs.getFirstBits(32).loadUint(32), cs.skipBits(64), cs.load_u32());

    return [one,three];
}

@method_id(105)
fun test5(): [int,int] {
    var cref: cell = endCell(beginCell().store_u32(105));
    var c: cell = beginCell().storeRef(cref).storeRef(cref).store_u32(1).endCell();

    var cs: slice = beginParse(c);
    var sto5x2: int = cs.loadRef().beginParse().load_u32() + cs.loadRef().beginParse().loadUint(32);
    return [sto5x2, cs.load_u32()];
}

@method_id(106)
fun test6() {
    var st = beginCell().storeUint(1, 32).storeUint(2, 32).storeUint(3, 32).endCell().beginParse();
    return st.loadUint(96) == ((1 << 64) + (2 << 32) + 3);
}

@method_id(107)
fun test7() {
    // since .store() methods now mutate, this piece of code works not as earlier (mutates uri_builder)
    var uri_builder = beginCell();
    var uri_slice = uri_builder.storeSlice(".json").endCell().beginParse();
    var image_slice = uri_builder.storeSlice(".png").endCell().beginParse();
    return (uri_builder.bitsCount(), uri_slice.remainingBitsCount(), image_slice.remainingBitsCount());
}

@method_id(108)
fun test8() {
    var uri_builder = beginCell();
    var fresh = uri_builder;
    var uri_slice = fresh.storeSlice(".json").endCell().beginParse();
    var fresh redef = uri_builder;
    var image_slice = fresh.storeSlice(".png").endCell().beginParse();
    return (uri_builder.bitsCount(), uri_slice.remainingBitsCount(), image_slice.remainingBitsCount());
}


fun slice.sumNumbersInSlice(mutate self): int {
    var result = 0;
    while (!self.isEndOfBits()) {
        result += self.loadUint(32);
    }
    return result;
}

@method_id(110)
fun test10() {
    var ref = beginCell().storeInt(100, 32).endCell();
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeRef(ref).endCell().beginParse();
    var result = (s.remainingBitsCount(), s.sumNumbersInSlice(), s.remainingBitsCount(), s.isEnd(), s.isEndOfBits(), s.isEndOfRefs());
    var ref2: cell = s.loadRef();
    var s2: slice = ref2.beginParse();
    s.assertEnd();
    return (result, s2.loadInt(32), s2.isEnd());
}

@method_id(111)
fun test11() {
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeInt(3, 32).storeInt(4, 32).storeInt(5, 32).storeInt(6, 32).storeInt(7, 32).endCell().beginParse();
    var size1 = s.remainingBitsCount();
    s!.skipBits(32);
    var s1: slice = s.getFirstBits(64);
    var n1 = s1.loadInt(32);
    var size2 = s.remainingBitsCount();
    s.loadInt(32);
    var size3 = s.remainingBitsCount();
    s.removeLastBits(32);
    var size4 = s.remainingBitsCount();
    var n2 = s.loadInt(32);
    var size5 = s.remainingBitsCount();
    return (n1, n2, size1, size2, size3, size4, size5);
}

@method_id(112)
fun test12() {
    var (result1, result2) = (0, 0);
    try {
        beginCell().storeRef(beginCell().endCell()).endCell().beginParse().assertEnd();
        result1 = 100;
    } catch (code) {
        result1 = code;
    }
    try {
        beginCell().endCell().beginParse().assertEnd();
        result2 = 100;
    } catch (code) {
        result2 = code;
    }
    return (result1, result2);
}

@method_id(113)
fun test13() {
    var ref2 = beginCell().storeInt(1, 32).endCell();
    var ref1 = beginCell().storeInt(1, 32).storeRef(ref2).endCell();
    var c = beginCell().storeInt(444, 32).storeRef(ref1).storeRef(ref1).storeRef(ref1).storeRef(ref2).storeInt(4, 32).endCell();
    var (n_cells1, n_bits1, n_refs1) = c.calculateSizeStrict(10);
    var s = c.beginParse();
    s.loadRef();
    s.loadRef();
    var n = s.loadInt(32);
    var (n_cells2, n_bits2, n_refs2) = s.calculateSizeStrict(10);
    return ([n_cells1, n_bits1, n_refs1], [n_cells2, n_bits2, n_refs2], n);
}

@method_id(114)
fun test110(x: bool) {
    var s = beginCell().storeBool(x == true).storeBool(false).storeBool(x).endCell().beginParse();
    return (s.loadBool(), s.loadBool(), s.loadBool());
}

@method_id(115)
fun test111() {
    var s = beginCell().storeMessageOp(123).storeMessageQueryId(456)
        .storeAddressNone().storeAddressNone()
        .storeUint(0, 32)
        .storeUint(123, 32).storeUint(456, 64).storeUint(789, 64)
        .endCell().beginParse();
    var op1 = s.loadUint(32);
    var q1 = s.loadUint(64);
    if ((s as address).isNone()) {
        s.skipBits(2);
    }
    if (s.loadBool() == false) {
        assert(!s.loadBool()) throw 444;
        s.skipBouncedPrefix();
    }
    var op2 = s.loadMessageOp();
    var q2 = s.loadMessageQueryId();
    s.skipBits(64);
    s.assertEnd();
    return (op1, q1, op2, q2);
}

global g8: int;
global g10: int;
global g16: int;
global g32: int;

@inline_ref
fun initGlobals() {
    g8 = 8;
    g10 = 10;
    g16 = 16;
    g32 = 32;
}

@method_id(116)
fun test16() {
    initGlobals();
    var b1 = beginCell().storeUint(g8, g16).storeUint(0xFF, g32).storeUint(g8, g16 * 2);
    var b2 = beginCell().storeUint(8, 16).storeUint(0xFF, 32).storeUint(8, 16 * 2);
    assert(b1.bitsCount() == b2.bitsCount(), 400);
    var c1 = b1.endCell().beginParse();
    var c2 = b2.endCell().beginParse();
    assert(c1.bitsEqual(c2), 400);
    assert(c1.loadUint(g16) == g8, 400);
    assert(c1.loadUint(g32) == 0xFF, 400);
    assert(c1.loadUint(2 * g16) == 8, 400);
    return b1;
    // 00140008000000ff00000008
    // 00140008000000ff00000008
}


@method_id(117)
fun test17() {
    var b = beginCell().storeUint(1, 4).storeCoins(0).storeInt(123, 8);
    var s = b.endCell().beginParse();
    return (s.loadUint(4), s.loadCoins(), s.loadUint(8));
}

@method_id(118)
fun test18() {
  var x = 0;
  var b = beginCell();
  b = b.storeUint(x, 14);
  x += 12;
  if (10 > 3) { x += x; }
  if (true) {
      b.storeInt(x + 2, 8).storeUint(x = match (x) { 24 => 5, else => 0 }, 4);
  }
  var s = b.endCell().beginParse();
  return (s.loadUint(14), s.loadInt(8), s.loadUint(4));
}

fun test19() {
    // numbers with potential overflow for STU are not joined, check via codegen
    var b = beginCell();
    b.storeInt(123, 4).storeUint(0xFF, 8).storeUint(0xFF, 8).storeInt(-1, 8);
    return b;
}

@method_id(120)
fun test20() {
  var x = false;
  var n = 4;
  var b = true ? beginCell() : null;
  b.storeBool(true).storeBool(x);
  b = b.storeBool(true).storeUint(0, n *= 2).storeBool(!!true).storeCoins(0);
  var s = b.endCell().beginParse();
  return (s.loadBool(), s.loadBool(), s.loadBool(), s.loadUint(8), s.loadBool(), s.loadCoins());
}

fun test21(s: slice) {
    // successive skipBits are also joined
    var x = 8;
    s.skipBits(x);
    x -= 4;
    s = s.skipBits(x).skipBits(2);
    x *= 0;
    s = s.skipBits(x);
    return s;
}

@method_id(122)
fun test22() {
    // different builders aren't mixed, store inside them are joined independently
    var (b1, b2) = (beginCell(), beginCell());
    b1.storeUint(8, 16).storeUint(8, 8);
    b2.storeUint(8, 32).storeUint(1<<88, 100);
    return (
        b1.endCell().beginParse().remainingBitsCount(),
        b2.endCell().beginParse().skipBits(32).loadUint(100),
    );
}

@method_id(123)
fun test23(uns: bool) {
    // corner values, signed/unsigned 255/256
    var b = beginCell();
    if (uns) {
        b.storeUint(1, 100).storeUint(2, 100).storeInt(3, 55).storeInt(0, 1);
        b.storeUint(8, 256);
    } else {
        b.storeInt(1, 10).storeUint(2, 190).storeInt(3, 54).storeUint(1, 1);
    }
    return b.bitsCount();
}

@method_id(124)
fun test24(uns: bool) {
    // doesn't fit into a single STI/STU instruction, is splitted
    var b = beginCell();
    if (uns) {
        b.storeUint(1, 100).storeUint(2, 100)
         .storeInt(3, 100).storeInt(8, 19);
        return b.endCell().beginParse().skipBits(200+100).loadInt(19);
    } else {
        b.storeInt(1, 20).storeUint(2, 200).storeInt(3, 35)
         .storeUint(1, 1).storeUint(5, 5).storeUint(10, 10);
        return b.endCell().beginParse().skipBits(255+6).loadUint(10);
    }
}

fun main(): int {
    return 0;
}

/**
@testcase | 101 |     | [ 1 2 3 4 5 ]
@testcase | 102 |     | [ 1 2 3 ]
@testcase | 103 | 103 | 103
@testcase | 104 |     | [ 1 3 ]
@testcase | 105 |     | [ 210 1 ]
@testcase | 106 |     | -1
@testcase | 107 |     | 72 40 72
@testcase | 108 |     | 0 40 32
@testcase | 110 |     | 64 3 0 0 -1 0 100 -1
@testcase | 111 |     | 2 3 224 192 160 128 96
@testcase | 112 |     | 9 100
@testcase | 113 |     | [ 3 128 5 ] [ 2 96 3 ] 444
@testcase | 114 | -1  | -1 0 -1
@testcase | 114 | 0   | 0 0 0
@testcase | 115 |     | 123 456 123 456
@testcase | 116 |     | BC{00140008000000ff00000008}
@testcase | 117 |     | 1 0 123
@testcase | 118 |     | 0 26 5
@testcase | 120 |     | -1 0 -1 0 -1 0
@testcase | 122 |     | 24 309485009821345068724781056
@testcase | 123 | -1  | 512
@testcase | 123 | 0   | 255
@testcase | 124 | -1  | 8
@testcase | 124 | 0   | 10

We test that consequtive storeInt/storeUint with constants are joined into a single number

@fif_codegen
"""
  test6 PROC:<{
    18446744082299486211 PUSHINT
    NEWC
    96 STU      //  '2
    ENDC        //  '11
"""

@fif_codegen
"""
  test17 PROC:<{
    4219 PUSHINT
    NEWC
    16 STU
"""

@fif_codegen
"""
  test18 PROC:<{
    421 PUSHINT
    NEWC
    26 STU
"""

@fif_codegen
"""
  test19 PROC:<{
    NEWC
    123 PUSHINT
    SWAP
    4 STI
    16 PUSHPOW2DEC
    16 STUR
    -1 PUSHINT
    SWAP
    8 STI
  }>
"""

@fif_codegen
"""
  test20 PROC:<{
    40976 PUSHINT
    NEWC
    16 STU
"""

@fif_codegen
"""
  test21 PROC:<{
    14 PUSHINT
    SDSKIPFIRST
  }>
"""

@fif_codegen
"""
  test22 PROC:<{
    NEWC                    	//  '2
    NEWC                    	//  b1 b2
    SWAP                    	//  b2 b1
    2056 PUSHINT
    24 STUR                 	//  b2 b1
    SWAP                    	//  b1 b2
    10141514286835656557042350424064 PUSHINTX
    132 STUR                	//  b1 b2
"""

@fif_codegen
"""
  test23 PROC:<{
    NEWC
    SWAP
    IF:<{
      91343852333181432387730302044911803916571639814 PUSHINT
      256 STUR
      8 PUSHINT
      256 STUR
    }>ELSE<{
      56539106072908298546665520023773392506479484700019806659963456035401760775 PUSHINT
      255 STIR
    }>
    BBITS
  }>
"""

 */
