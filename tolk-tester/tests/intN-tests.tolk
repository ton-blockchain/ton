fun takeAnyInt(a: int) {  }
fun getAnyInt(): int { return 0; }
fun getNullableInt32(): int32? { return 32; }
fun getNullableVarInt32(): varint32? { return 32; }


fun autoInferInt8(x: int8) {
    if (random()) { return x; }
    else if (random()) { return 0 as int8; }
    else if (random()) { return x!; }
    else { return x += x; }
}

fun autoInferUInt16Opt(x: uint16) {
    if (random()) { return autoInferInt8(x as int8) as uint16; }
    return null;
}

fun autoInferInt_v1() {
    if (random()) { return 0; }
    else if (random()) { return 0 as uint16; }
    else { return 0 as int8; }
}

fun autoInferInt_v2() {
    if (random()) { return 0 as uint1; }
    else if (random()) { return 0 as int123; }
    else { return 0 as int8; }
}

@method_id(101)
fun test1(x: int8) {
    var y: int8 = x;
    y += x;
    x *= y;
    x = 1000000;
    y = x;
    __expect_type(x / y, "int");
    __expect_type(x & y, "int");
    __expect_type(x != y, "bool");
    if (x != y) { return -1; }
    if (!(y == x)) { return -1; }

    var a1: uint8 = 0;
    var a2: uint8? = 0;
    var a3: uint8? = 0 as int?;
    __expect_type(a1, "uint8");
    __expect_type(a2, "uint8");
    __expect_type(a3, "uint8?");

    var z = x + y;
    __expect_type(z, "int");
    return x / y;
}

fun test2(): (uint8, uint8, uint8) {
    var x: uint1 = 1;
    return (1, x as uint8, x as int);
}

fun test3(op: int32, qid: uint64) {
    op = qid as int32;

    op + qid;
    op = op + qid;
    op = op & qid;
    op += qid;
    op &= qid;
    if (op == qid) {}
    if ((op as int32?)! == qid) {}
    if (op == (qid as uint64)) {}
    if ((op as uint257?)! != (qid as int256?)!) {}
    __expect_type(op << qid, "int");

    takeAnyInt(op);
    op = getAnyInt();

    __expect_type(autoInferInt8, "(int8) -> int8");
    __expect_type(autoInferUInt16Opt(0), "uint16?");
    __expect_type(autoInferInt_v1(), "int");
    __expect_type(autoInferInt_v2(), "int");

    var amount: uint100 = 1000;
    var percent: uint8 = 50;
    var new = amount * percent / 100;
    amount = new;
}

@method_id(104)
fun test4(): (int32, int32?, bool, bool) {
    var x = getNullableInt32();
    __expect_type(x, "int32?");
    if (x! != x! || x! != 32) {
        return (0, null, false, false);
    }
    if (x == null) {
        return (-1, null, false, false);
    }
    x += x;
    return (x, x, x == x, x == getAnyInt());
}

@method_id(105)
fun test5() {
    var (x: int8, y: uint16) = (1, 2);
    var cell = beginCell().storeInt(x, 32).storeInt(y, 32 as int32).endCell();
    var slice = cell.beginParse();
    x = slice.loadInt(32);
    y = slice.loadInt(32);
    __expect_type(x & y, "int");
    return (x + y, x && y, x & y);
}

fun test6() {
    var x: int11 = ~(0 as int22);
    while (x) {}
    return x ? x : 0;
}

fun test7() {
    var n = getNullableVarInt32();
    __expect_type(n!, "varint32");
    __expect_type(n, "varint32?");
    if (n != null) {
        __expect_type(n, "varint32");
        __expect_type(n += n, "varint32");
        __expect_type(n += n as int8, "varint32");
    } else {
        n = 0;
        n = 0 as varint32?;
        n = 0 as varint32;
    }
    __expect_type(n, "varint32");
    __expect_type(n as uint32, "uint32");
    __expect_type(n as varint16, "varint16");
    __expect_type((n as varint16) as int, "int");
}

fun test13(x1: int?, x2: int8?, x3: int, x4: int8): (int8?, int8?, int8?, int8?, int32?, int32?) {
    return (x1, x2, x3, x4, x4 as int32, x4 as int32?);
}

@method_id(114)
fun test14(firstComponent: int8?): (int, int16)? {
    return firstComponent! < 10 ? null : (firstComponent!, 2);
}

fun assign0<T>(mutate v: T) { v = 0; }

fun main() {
    var t = createEmptyTuple();
    t.tuplePush(1);
    t.tuplePush(2);
    t.tuplePush(3);
    assign0(mutate t.0 as int8);
    assign0(mutate t.1 as uint16);
    assign0(mutate t.2 as int);
    __expect_type(t.0 as int1, "int1");
    __expect_type(t.0 as uint257, "uint257");
    __expect_type(0 as int32, "int32");
    __expect_type((0 as int32) as uint64?, "uint64?");
    __expect_type(null as (int8, [uint16?])?, "(int8, [uint16?])?");
    __expect_type(10>3 ? 0 as uint8 : 0 as uint16, "int");
    return t;
}

/**
@testcase | 0 |        | [ 0 0 0 ]
@testcase | 101 | 0    | 1
@testcase | 104 |      | 64 64 -1 0
@testcase | 105 |      | 3 -1 0
@testcase | 114 | 5    | (null) (null) 0
@testcase | 114 | 15   | 15 2 -1

@fif_codegen DECLPROC assign0<int8>
@fif_codegen DECLPROC assign0<uint16>
@fif_codegen DECLPROC assign0<int>
*/
