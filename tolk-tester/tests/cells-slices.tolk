fun store_u32(mutate self: builder, value: int): self {
    return self.storeUint(value, 32);
}

fun load_u32(mutate self: slice): int {
    return self.loadUint(32);
}

fun myLoadInt(mutate self: slice, len: int): int
    asm(-> 1 0) "LDIX";
fun myStoreInt(mutate self: builder, x: int, len: int): self
    asm(x self len) "STIX";

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
    b.myStoreInt(3, 32);

    var cs: slice = b.endCell().beginParse();
    var one: int = cs.myLoadInt(32);
    var (two: int, three: int) = (cs.myLoadInt(32), cs.myLoadInt(32));

    return [one,two,three];
}

@method_id(103)
fun test3(ret: int): int {
    val same: int = beginCell().storeUint(ret,32).endCell().beginParse().loadUint(32);
    return same;
}

@method_id(104)
fun test4(): [int,int] {
    var b: builder = beginCell().myStoreInt(1, 32);
    b = b.storeInt(2, 32).storeInt(3, 32);

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
    return beginCell().storeUint(1, 32).storeUint(2, 32).storeUint(3, 32);
}

@method_id(107)
fun test7() {
    // since .store() methods now mutate, this piece of code works not as earlier (mutates uri_builder)
    var uri_builder = beginCell();
    var uri_slice = uri_builder.storeSlice(".json").endCell().beginParse();
    var image_slice = uri_builder.storeSlice(".png").endCell().beginParse();
    return (uri_builder.getBuilderBitsCount(), uri_slice.getRemainingBitsCount(), image_slice.getRemainingBitsCount());
}

@method_id(108)
fun test8() {
    var uri_builder = beginCell();
    var fresh = uri_builder;
    var uri_slice = fresh.storeSlice(".json").endCell().beginParse();
    var fresh redef = uri_builder;
    var image_slice = fresh.storeSlice(".png").endCell().beginParse();
    return (uri_builder.getBuilderBitsCount(), uri_slice.getRemainingBitsCount(), image_slice.getRemainingBitsCount());
}


fun sumNumbersInSlice(mutate self: slice): int {
    var result = 0;
    while (!self.isEndOfSliceBits()) {
        result += self.loadUint(32);
    }
    return result;
}

@method_id(110)
fun test10() {
    var ref = beginCell().storeInt(100, 32).endCell();
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeRef(ref).endCell().beginParse();
    var result = (getRemainingBitsCount(s), s.sumNumbersInSlice(), getRemainingBitsCount(s), isEndOfSlice(s), isEndOfSliceBits(s), isEndOfSliceRefs(s));
    var ref2: cell = s.loadRef();
    var s2: slice = ref2.beginParse();
    s.assertEndOfSlice();
    return (result, s2.loadInt(32), s2.isEndOfSlice());
}

@method_id(111)
fun test11() {
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeInt(3, 32).storeInt(4, 32).storeInt(5, 32).storeInt(6, 32).storeInt(7, 32).endCell().beginParse();
    var size1 = getRemainingBitsCount(s);
    s.skipBits(32);
    var s1: slice = s.getFirstBits(64);
    var n1 = s1.loadInt(32);
    var size2 = getRemainingBitsCount(s);
    s.loadInt(32);
    var size3 = getRemainingBitsCount(s);
    s.removeLastBits(32);
    var size4 = getRemainingBitsCount(s);
    var n2 = s.loadInt(32);
    var size5 = getRemainingBitsCount(s);
    return (n1, n2, size1, size2, size3, size4, size5);
}

@method_id(112)
fun test12() {
    var (result1, result2) = (0, 0);
    try {
        beginCell().storeRef(beginCell().endCell()).endCell().beginParse().assertEndOfSlice();
        result1 = 100;
    } catch (code) {
        result1 = code;
    }
    try {
        beginCell().endCell().beginParse().assertEndOfSlice();
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
    var (n_cells1, n_bits1, n_refs1) = c.calculateCellSizeStrict(10);
    var s = c.beginParse();
    s.loadRef();
    s.loadRef();
    var n = s.loadInt(32);
    var (n_cells2, n_bits2, n_refs2) = s.calculateSliceSizeStrict(10);
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
    if (s.addressIsNone()) {
        s.skipBits(2);
    }
    if (s.loadBool() == false) {
        assert(!s.loadBool()) throw 444;
        s.skipBouncedPrefix();
    }
    var op2 = s.loadMessageOp();
    var q2 = s.loadMessageQueryId();
    s.skipBits(64);
    s.assertEndOfSlice();
    assert(isMessageBounced(0x001) && !isMessageBounced(0x002)) throw 444;
    return (op1, q1, op2, q2);
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
@testcase | 107 |     | 72 40 72
@testcase | 108 |     | 0 40 32
@testcase | 110 |     | 64 3 0 0 -1 0 100 -1
@testcase | 111 |     | 2 3 224 192 160 128 96
@testcase | 112 |     | 9 100
@testcase | 113 |     | [ 3 128 5 ] [ 2 96 3 ] 444
@testcase | 114 | -1  | -1 0 -1
@testcase | 114 | 0   | 0 0 0
@testcase | 115 |     | 123 456 123 456

Note, that since 'compute-asm-ltr' became on be default, chaining methods codegen is not quite optimal.
@fif_codegen
"""
  test6 PROC:<{
    //
    NEWC	//  _0
    1 PUSHINT	//  _0 _1=1
    SWAP	//  _1=1 _0
    32 STU	//  _0
    2 PUSHINT	//  _0 _5=2
    SWAP	//  _5=2 _0
    32 STU	//  _0
    3 PUSHINT	//  _0 _9=3
    SWAP	//  _9=3 _0
    32 STU	//  _0
  }>
"""
 */
