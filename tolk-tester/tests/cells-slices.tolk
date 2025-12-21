import "@stdlib/exotic-cells"

fun builder.store_u32(mutate self, value: int): self {
    return self.storeUint(value, 32);
}
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

@noinline
fun triggerOverflowIntConst() {
    return beginCell().storeInt(10, 4)
}

@noinline
fun triggerOverflowUintConst() {
    return beginCell().storeUint(123, 6)
}

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

    var cs: slice = b.toSlice();
    var one: int = cs.myLoadInt(32);
    var (two: int, three: int) = (cs.myLoadInt(32), cs.myLoadInt(32));

    return [one,two,three];
}

@method_id(103)
fun test3(ret: int): int {
    val same: int = beginCell()!.storeUint(ret,32).toSlice().loadUint(32);
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
    var result = (s.remainingBitsCount(), s.sumNumbersInSlice(), s.remainingBitsCount(), s.isEmpty(), s.isEndOfBits(), s.isEndOfRefs());
    var ref2: cell = s.loadRef();
    var s2: slice = ref2.beginParse();
    s.assertEnd();
    return (result, s2.loadInt(32), s2.isEmpty());
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
    if ((s as any_address).isNone()) {
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
  var s = [b.endCell()].0.beginParse();
  return (s.loadUint(14), s.loadInt(8), s.loadUint(4));
}

@method_id(119)
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
  var s = b.toSlice();
  return (s.loadBool(), s.loadBool(), s.loadBool(), s.loadUint(8), s.loadBool(), s.loadCoins());
}

@method_id(121)
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

fun builder.my_storeRef(mutate self, value: cell): self
    asm(self value) "STREFR";

fun builder.my_storeAddress(mutate self, value: any_address): self
    asm(value self) "STSLICE";

fun builder.my_storeBuilder(mutate self, value: builder): self
    asm(value self) "STB";

fun my_PUSHINT_SWAP_STU(mutate b: builder): void
    asm "5 PUSHINT" "SWAP" "8 STU";


@noinline
fun demo30(value: cell, dest: builder) {
    // SWAP + STREFR => STREF
    return dest.my_storeRef(value);
}

@noinline
fun demo31(dest: builder, value: cell) {
    // SWAP + STREF => STREFR
    return dest.storeRef(value);
}

@noinline
fun demo32(dest: builder, value: builder) {
    // SWAP + STB => STBR
    return dest.my_storeBuilder(value);
}

@noinline
fun demo33(value: builder, dest: builder) {
    // SWAP + STBR => STB
    return dest.storeBuilder(value);
}

@noinline
fun demo34(dest: builder, value: any_address) {
    // SWAP + STSLICE => STSLICER
    return dest.my_storeAddress(value);
}

@noinline
fun demo35(value: slice, dest: builder) {
    // SWAP + STSLICER => STSLICE
    return dest.storeSlice(value);
}

@noinline
fun demo36(x: int) {
    var b = beginCell();
    if (x > 0) {
        // inside IF: N PUSHINT + SWAP + L STU => N PUSHINT + L STUR
        my_PUSHINT_SWAP_STU(mutate b);
    }
    return b;
}

@noinline
fun demo37(op: int32, u: int8 | int256) {
    var b = beginCell();
    // b.storeUint(input.op, 32);
    if (u is int8) {
        // inside IF: SWAP + 0 PUSHINT + 3 STUR => 0 PUSHINT + 3 STU
        b.storeUint(0, 3);
        b.storeUint(u, 8);
    }
    return b;
}

@method_id(130)
fun test30() {
    return (demo37(10, 88 as int8), demo36(50));
}

@method_id(131)
fun test31() {
    var s = createAddressNone();
    var dest = beginCell().storeUint(3, 32);
    assert(demo35(s as slice, dest).endCell().hash() == demo34(dest, s).endCell().hash()) throw 300;
    var r1 = demo35(s as slice, dest).endCell().hash();
    var b = beginCell().storeUint(0xFF, 16);
    assert(demo33(b, dest).endCell().hash() == demo32(dest, b).endCell().hash()) throw 301;
    var r2 = demo33(b, dest).endCell().hash();
    var r = createEmptyCell();
    assert(demo30(r, dest).endCell().hash() == demo31(dest, r).endCell().hash()) throw 302;
    var r3 = demo30(r, dest).endCell().hash();
    return [r1 & 0xFFFFFFFF, r2 & 0xFFFFFFFF, r3 & 0xFFFFFFFF];
}

@method_id(132)
fun test32(p: int) {
    var b = beginCell();
    var x = p + 10;
    b.storeUint(x, 32);     // SWAP + n STU => n STUR
    return b;
}

@method_id(133)
fun test33(p: int) {
    var b = beginCell();
    var x = p + 10;
    b.storeInt(x, 8);       // SWAP + n STI => n STIR
    return b;
}

@method_id(134)
fun test34(p: int, n: int) {
    var b = beginCell();
    var x = p + 10;
    b.storeInt(x, n);       // no changes, it's STIX
    return b;
}

@method_id(135)
fun test35(overflowMode: int) {
    try {
        val b: builder = match (overflowMode) {
            1 => triggerOverflowIntConst(), 
            2 => triggerOverflowUintConst(),
            3 => beginCell().storeUint(10, -4),
            4 => beginCell().storeInt(100, 1),
            5 => beginCell().storeInt(1, 0),
            6 => beginCell().storeInt(115792089237316195423570985008687907853269984665640564039457584007913129639935, 256),
            7 => beginCell().storeInt(15, 4),
            8 => beginCell().storeUint(1<<170, 169),
            else => beginCell(),
        };
        return b.endCell().beginParse().loadUint(1) * 1000
    }
    catch (ex) { return ex }
}

@method_id(136)
fun test36() {
    return beginCell().storeInt(0, 0)
}

@method_id(137)
fun test37() {
    return beginCell().storeInt(12, 32).endCell().hash() & 0xFFFF
}

@method_id(138)
fun test38() {
    return beginCell().endCell().beginParse().remainingBitsCount()
}

@method_id(139)
fun test39() {
    return beginCell().endCell()
}

@method_id(140)
fun test40() {
    return beginCell().toSlice().remainingBitsAndRefsCount()
}

@method_id(141)
fun test41() {
    var empty = "";
    var incorrect = stringHexToSlice("8");
    var incomplete = (address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF") as slice).getFirstBits(266);

    var data = createEmptyTuple();
    var b = beginCell();

    try { data.push(empty.loadAddress()) }
    catch (ex) { data.push(ex) }
    try { data.push(incorrect.loadAddress()) }
    catch (ex) { data.push(ex) }
    try { data.push(incomplete.loadAddress()) }
    catch (ex) { data.push(ex) }

    try { data.push(empty.loadAddressOpt()) }
    catch (ex) { data.push(ex) }
    try { data.push(incorrect.loadAddressOpt()) }
    catch (ex) { data.push(ex) }
    try { data.push(incomplete.loadAddressOpt()) }
    catch (ex) { data.push(ex) }

    try { data.push(empty.loadAddressAny()) }
    catch (ex) { data.push(ex) }
    try { data.push(incorrect.loadAddressAny()) }
    catch (ex) { data.push(ex) }
    try { data.push(incomplete.loadAddressAny()) }
    catch (ex) { data.push(ex) }

    try { b.storeAddress(createAddressNone() as address) }
    catch (ex) { data.push(ex) }
    try { b.storeAddressOpt(createAddressNone() as address) }
    catch (ex) { data.push(ex) }
    try { b.storeAny("123" as address) }
    catch (ex) { data.push(ex) }
    try { b.storeAny("123" as address?) }
    catch (ex) { data.push(ex) }

    try { b.storeAddressOpt(null); data.push(b.bitsCount()) }
    catch (ex) { data.push(ex) }

    return data;
}

@method_id(142)
fun test42() {
    var c1 = beginCell().endCell();
    var c2 = beginCell().endCellSpecial(false);
    return (
        c1.hash() == c1.hashOfLevel(3), 
        c1.depth() == c1.depthOfLevel(3),
        c1.hash() == c2.hash(),
        c2.depth() == 0,
    );
}

@method_id(143)
fun test43() {
    var c3 = beginCell().storeUint(123, 32).endCell();
    var c3lib = c3.toLibraryReference();
    
    var (s, isExotic) = c3lib.beginParseSpecial();
    assert (isExotic) throw 123;
    val cellType = s.loadInt(8) as ExoticCellType;
    return (cellType, s.loadUint(256) == c3.hash(), s.remainingBitsCount());
    // 73669
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
@testcase | 130 |     | BC{00030b10} BC{000205}
@testcase | 131 |     | [ 225345949 901620178 1560646286 ]
@testcase | 132 | 0   | BC{00080000000a}
@testcase | 133 | 0   | BC{00020a}
@testcase | 134 | 0 8 | BC{00020a}
@testcase | 135 | 1   | 5
@testcase | 135 | 2   | 5
@testcase | 135 | 3   | 5
@testcase | 135 | 4   | 5
@testcase | 135 | 5   | 5
@testcase | 135 | 6   | 5
@testcase | 135 | 7   | 5
@testcase | 135 | 8   | 5
@testcase | 136 |     | BC{0000}
@testcase | 137 |     | 39065
@testcase | 138 |     | 0
@testcase | 140 |     | 0 0
@testcase | 141 |     | [ 9 9 9 9 9 9 9 9 9 8 8 8 8 2 ]
@testcase | 142 |     | -1 -1 -1 -1
@testcase | 143 |     | 2 -1 0

We test that consequtive storeInt/storeUint with constants are joined into a single number

@fif_codegen
"""
  test6() PROC:<{
    x{000000010000000200000003} PUSHSLICE
"""

@fif_codegen
"""
  test17() PROC:<{
    x{107b} PUSHSLICE
"""

@fif_codegen
"""
  test18() PROC:<{
    NEWC
    b{00000000000000000110100101} STSLICECONST
"""

@fif_codegen
"""
  test19() PROC:<{
    123 PUSHINT
    NEWC
    4 STI
    x{ffff} STSLICECONST
    -1 PUSHINT
    8 STIR
  }>
"""

@fif_codegen
"""
  test20() PROC:<{
    x{a010} PUSHSLICE
"""

@fif_codegen
"""
  test21() PROC:<{
    14 LDU
    NIP
  }>
"""

@fif_codegen
"""
  test22() PROC:<{
    NEWC                    	//  '2
    NEWC                    	//  b1 b2
    SWAP                        //  b2 b1
    x{000808} STSLICECONST      //  b2 b1
    SWAP                    	//  b1 b2
    x{000000080010000000000000000000000} STSLICECONST  //  b1 b2
"""

@fif_codegen
"""
  test23() PROC:<{
    NEWC
    SWAP
    IF:<{
      x{0000000000000000000000001000000000000000000000000200000000000006} STSLICECONST
      8 PUSHINT
      256 STUR
    }>ELSE<{
      b{000000000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000111} STSLICECONST
    }>
    BBITS
  }>
"""

@fif_codegen
"""
  demo30() PROC:<{            	//  value dest
    STREF                   	//  dest
  }>
  demo31() PROC:<{            	//  dest value
    STREFR                  	//  dest
  }>
  demo32() PROC:<{            	//  dest value
    STBR                    	//  dest
  }>
  demo33() PROC:<{            	//  value dest
    STB                     	//  dest
  }>
  demo34() PROC:<{            	//  dest value
    STSLICER                	//  dest
  }>
  demo35() PROC:<{            	//  value dest
    STSLICE                 	//  dest
  }>
"""

@fif_codegen
"""
  demo36() PROC:<{            	//  x
    NEWC                    	//  x b
    SWAP                    	//  b x
    0 GTINT                 	//  b '4
    IF:<{                   	//  b
      5 PUSHINT
      8 STUR                 	//  b
    }>                      	//  b
  }>
"""

@fif_codegen
"""
  demo37() PROC:<{
    NEWC
    s3 POP
    42 EQINT
    IF:<{
      SWAP
      b{000} STSLICECONST
      8 STU                 	//  b
    }>ELSE<{
      DROP                  	//  b
    }>
  }>
"""

@fif_codegen
"""
  test32() PROC:<{              //  p
    NEWC                        //  p b
    SWAP                        //  b p
    10 ADDCONST                 //  b x
    32 STUR                     //  b
  }>
"""

@fif_codegen
"""
  test33() PROC:<{              //  p
    NEWC                        //  p b
    SWAP                        //  b p
    10 ADDCONST                 //  b x
    8 STIR                      //  b
  }>
"""

@fif_codegen
"""
  test34() PROC:<{
    NEWC
    s0 s2 XCHG
    10 ADDCONST
    -ROT
    STIX
  }>
"""

@fif_codegen
"""
  triggerOverflowIntConst() PROC:<{ 
    10 PUSHINT
    NEWC
    4 STI
  }>
"""

@fif_codegen
"""
  triggerOverflowUintConst() PROC:<{ 
    123 PUSHINT
    NEWC
    6 STU
  }>
"""

@fif_codegen
"""
  test36() PROC:<{ 
    0 PUSHINT
    NEWC
    OVER
    STIX
  }>
"""

@fif_codegen
"""
  test37() PROC:<{
    NEWC
    x{0000000c} STSLICECONST
    HASHBU
"""

@fif_codegen
"""
  test38() PROC:<{ 
    x{} PUSHSLICE
    SBITS
  }>
"""

@fif_codegen
"""
  test39() PROC:<{ 
    <b b> PUSHREF
  }>
"""

@fif_codegen
"""
  test40() PROC:<{ 
    x{} PUSHSLICE
    SBITREFS
  }>
"""

 */
