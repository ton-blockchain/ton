fun store_u32(b: builder, value: int): builder {
    return b.storeUint(value, 32);
}
fun ~store_u32(b: builder, value: int): (builder, ()) {
    return ~storeUint(b, value, 32);
}

fun load_u32(cs: slice): (slice, int) {
    return cs.loadUint(32);
}

fun my_loadInt(s: slice, len: int): (slice, int)
    asm(s len -> 1 0) "LDIX";  // top is "value slice"
fun my_storeInt(b: builder, x: int, len: int): builder
    asm(x b len) "STIX";
fun ~my_storeInt(b: builder, x: int, len: int): (builder, ())
    asm(x b len) "STIX";

@method_id(101)
fun test1(): [int,int,int,int,int] {
    var b: builder = beginCell().storeUint(1, 32);
    b = b.storeUint(2, 32);
    b~storeUint(3, 32);
    b = b.store_u32(4);
    b~store_u32(5);

    var cs: slice = b.endCell().beginParse();
    var (cs redef, one: int) = cs.loadUint(32);
    var (two: int, three: int) = (cs~loadUint(32), cs~load_u32());
    var (cs redef, four: int) = cs.load_u32();
    var five: int = cs~load_u32();

    return [one,two,three,four,five];
}

@method_id(102)
fun test2(): [int,int,int] {
    var b: builder = beginCell().my_storeInt(1, 32);
    b = b.my_storeInt(2, 32);
    b~my_storeInt(3, 32);

    var cs: slice = b.endCell().beginParse();
    var (cs redef, one: int) = cs.my_loadInt(32);
    var (two: int, three: int) = (cs~my_loadInt(32), cs~my_loadInt(32));

    return [one,two,three];
}

@method_id(103)
fun test3(ret: int): int {
    var (_, same: int) = beginCell().storeUint(ret,32).endCell().beginParse().loadUint(32);
    return same;
}

@method_id(104)
fun test4(): [int,int] {
    var b: builder = my_storeInt(beginCell(), 1, 32);
    b = storeInt(storeInt(b, 2, 32), 3, 32);

    var cs: slice = b.endCell().beginParse();
    var cs32: slice = cs.getFirstBits(32);      // todo s.first_bits()~loadUint() doesn't work, 'lvalue expected'
    var (one, _, three) = (cs32~loadInt(32), cs~skipBits(64), cs~load_u32());

    return [one,three];
}

@method_id(105)
fun test5(): [int,int] {
    var cref: cell = endCell(store_u32(beginCell(), 105));
    var c: cell = beginCell().storeRef(cref).storeRef(cref).store_u32(1).endCell();

    var cs: slice = beginParse(c);
    // todo I want cs~loadRef().beginParse()~load_u32(), but 'lvalue expected'
    var ref1 = cs~loadRef().beginParse();
    var ref2 = cs~loadRef().beginParse();
    var sto5x2: int = ref1~load_u32() + ref2~loadUint(32);
    return [sto5x2, cs~load_u32()];
}


fun ~sumNumbersInSlice(s: slice): (slice, int) {
    var result = 0;
    while (!s.isEndOfSliceBits()) {
        result += s~loadUint(32);
    }
    return (s, result);
}

@method_id(106)
fun test6() {
    var ref = beginCell().storeInt(100, 32).endCell();
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeRef(ref).endCell().beginParse();
    var result = (getRemainingBitsCount(s), s~sumNumbersInSlice(), getRemainingBitsCount(s), isEndOfSlice(s), isEndOfSliceBits(s), isEndOfSliceRefs(s));
    var ref2: cell = s~loadRef();
    var s2: slice = ref2.beginParse();
    s.assertEndOfSlice();
    return (result, s2~loadInt(32), s2.isEndOfSlice());
}

@method_id(107)
fun test7() {
    var s: slice = beginCell().storeInt(1, 32).storeInt(2, 32).storeInt(3, 32).storeInt(4, 32).storeInt(5, 32).storeInt(6, 32).storeInt(7, 32).endCell().beginParse();
    var size1 = getRemainingBitsCount(s);
    s~skipBits(32);
    var s1: slice = s.getFirstBits(64);
    var n1 = s1~loadInt(32);
    var size2 = getRemainingBitsCount(s);
    s~loadInt(32);
    var size3 = getRemainingBitsCount(s);
    s~removeLastBits(32);
    var size4 = getRemainingBitsCount(s);
    var n2 = s~loadInt(32);
    var size5 = getRemainingBitsCount(s);
    return (n1, n2, size1, size2, size3, size4, size5);
}

@method_id(108)
fun test108() {
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

@method_id(109)
fun test109() {
    var ref2 = beginCell().storeInt(1, 32).endCell();
    var ref1 = beginCell().storeInt(1, 32).storeRef(ref2).endCell();
    var c = beginCell().storeInt(444, 32).storeRef(ref1).storeRef(ref1).storeRef(ref1).storeRef(ref2).storeInt(4, 32).endCell();
    var (n_cells1, n_bits1, n_refs1) = c.calculateCellSizeStrict(10);
    var s = c.beginParse();
    s~loadRef();
    s~loadRef();
    var n = s~loadInt(32);
    var (n_cells2, n_bits2, n_refs2) = s.calculateSliceSizeStrict(10);
    return ([n_cells1, n_bits1, n_refs1], [n_cells2, n_bits2, n_refs2], n);
}

@method_id(110)
fun test110(x: int) {
    var s = beginCell().storeBool(x < 0).storeBool(0).storeBool(x).endCell().beginParse();
    return (s~loadBool(), s~loadBool(), s~loadBool());
}

@method_id(111)
fun test111() {
    var s = beginCell().storeMessageOp(123).storeMessageQueryId(456)
        .storeAddressNone().storeAddressNone()
        .storeUint(0, 32)
        .storeUint(123, 32).storeUint(456, 64).storeUint(789, 64)
        .endCell().beginParse();
    var op1 = s~loadUint(32);
    var q1 = s~loadUint(64);
    if (s.addressIsNone()) {
        s~skipBits(2);
    }
    if (s~loadBool() == 0) {
        assert(s~loadBool() == 0) throw 444;
        s~skipBits(32);
    }
    var op2 = s~loadMessageOp();
    var q2 = s~loadMessageQueryId();
    s~skipBits(64);
    s.assertEndOfSlice();
    assert(isMessageBounced(0x001)) throw 444;
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
@testcase | 106 |     | 64 3 0 0 -1 0 100 -1
@testcase | 107 |     | 2 3 224 192 160 128 96
@testcase | 108 |     | 9 100
@testcase | 109 |     | [ 3 128 5 ] [ 2 96 3 ] 444
@testcase | 110 | -1  | -1 0 -1
@testcase | 110 | 0   | 0 0 0
@testcase | 111 |     | 123 456 123 456
 */
