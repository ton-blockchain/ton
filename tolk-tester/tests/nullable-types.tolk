type MIntN = int?;

fun getNullable4(): MIntN { return 4; }
fun getNullableIntNull(): int? asm "PUSHNULL";

fun eqInt(x: int) { return x }
fun eq<T>(x: T) { return x }

fun unwrap<T>(x: T?): T { return x!; }
fun intOr0(x: int?): int { return null == x ? 0 : x!; }

@pure
fun storeInt(mutate b: builder, x: int, len: int): void
    asm(x b len) "STIX";
@pure
fun tuplePush<T>(mutate t: tuple, value: T): void
    asm "TPUSH";

@method_id(101)
fun test101(x: int) {
    var re = x == 0 ? null : 100;
    return re == null ? re : 200 + getNullable4()!;
}

@method_id(102)
fun test102(a: int) {
    try {
        throw (123, a > 10 ? null : a);
        return 0;
    } catch (excno, arg) {
        var i = arg as int?;
        return excno + (i != null ? i!!!!! : -100);
    }
}

@method_id(103)
fun test103(x: int?): (bool, bool, int) {
    var x_gt_0 = x != null && eqInt(x!) > 0;
    var x_lt_0 = x != null && eq(x)! < 0;
    if (x == null) {
        return (x_gt_0, x_lt_0, 0);
    }
    return (x_gt_0, x_lt_0, x!);
}

@method_id(104)
fun test104(x: MIntN) {
    var x2 = eq(x = 10);
    var ab = (x2, getNullableIntNull());
    return (unwrap(ab.0) + (ab.1 == null ? -100 : ab.1!), ab.1);
}

@method_id(105)
fun test105() {
    var xy: (int?, MIntN) = (5, null);
    var ab = [1 ? [xy.0, xy.1] : null];
    ab.0!.0 = intOr0(ab.0!.0);
    ab.0!.1 = intOr0(ab.0!.1);
    return ab.0!.0! + ab.0!.1!;
}

global gTup106: tuple?;
global gInt106: MIntN;

@method_id(106)
fun test106() {
    gInt106 = 0;
    gInt106! += 5;
    var int106: int? = 0;
    var gTup106 = createEmptyTuple();
    gTup106!.push(createEmptyTuple());
    (gTup106!.0 as tuple?)!.push(0 as int?);
    tuplePush(mutate gTup106!, gInt106);
    tuplePush(mutate gTup106!.0, int106! += 1);
    return (gTup106 == null, null != gTup106, gTup106, gTup106!.0 as tuple?);
}

@method_id(107)
fun test107() {
    var b: builder? = beginCell();
    b!.storeInt(1, 32).storeInt(2, 32);
    b = b!.storeInt(3, 32);
    storeInt(mutate b!, 4, 32);
    (b! as builder).storeInt(5, 32);
    return b!.bitsCount();
}

@method_id(108)
fun test108() {
    var (a, b: cell?, c) = (1, beginCell().endCell(), 3);
    if (10>3) { b = null; }
    return a + (b == null ? 0 : b!.beginParse().loadInt(32)) + c;
}

@method_id(109)
fun test109() {
    var a = getNullable4();
    var b = getNullable4();
    return ([a, b] = [3, 4], a, b);
}

fun main(x: int?, y: MIntN) {
    __expect_type(x! == y!, "bool");
    __expect_type(5 != y!, "bool");
    __expect_type(y == null, "bool");
    __expect_type(true == (true as bool?)!, "bool");
}

/**
@testcase | 101 | 0   | (null)
@testcase | 101 | -1  | 204
@testcase | 102 | 5   | 128
@testcase | 102 | 15  | 23
@testcase | 103 | 10  | -1 0 10
@testcase | 104 | 8   | -90 (null)
@testcase | 105 |     | 5
@testcase | 106 |     | 0 -1 [ [ 0 1 ] 5 ] [ 0 1 ]
@testcase | 107 |     | 160
@testcase | 108 |     | 4
@testcase | 109 |     | [ 3 4 ] 3 4
 */
