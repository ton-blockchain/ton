enum EFits2Bits { ZERO, ONE, TWO }

fun EFits2Bits.sliceWithZero(): slice asm "b{00} PUSHSLICE"
fun EFits2Bits.sliceWithThree(): slice asm "b{11} PUSHSLICE"

enum EStartFrom1 {
    ONE = 1
    TWO
    THREE
}

enum EFillsAllUint3 {
    p0, p1, p2, p3, p4, p5, p6, p7
}

enum EStartFromM2 {
    M2 = -2,
    M1, ZERO, P1, P2, P3 = 3,
}

enum EFits8Bits {
    E0 = 0;
    E110 = 110;
    E220 = 220;
}

const MAX_UINT = 115792089237316195423570985008687907853269984665640564039457584007913129639935
    
enum EMinMax {
    MIN_INT = -115792089237316195423570985008687907853269984665640564039457584007913129639935 - 1,
    MAX_INT = +115792089237316195423570985008687907853269984665640564039457584007913129639935,
}

enum E0Max {
    ZERO,
    MAX_INT = +115792089237316195423570985008687907853269984665640564039457584007913129639935,
}

struct WithEnumsUnion {
    u: EFits8Bits | EStartFromM2
}
    
enum Role: int8 {
    Admin, User,
}

fun (Role, Role).decode(mutate self, s: slice) {
    self = s.loadAny()
}

enum EncodedVari : varint16 {
    ONE = 1,
    TWO = 2,
    MANY = 1<<100,
}

enum EOfCrc32: uint32 {
    hello = stringCrc32("hello")
    world = stringCrc32("world")
}

enum Amounts: coins {
    zero = ton("0"),
    nanoton = 1,
    small = ton("0.0001"),
    more = ton("0.05"),
    more1,
    one = ton("1"),
}


@method_id(101)
fun test1() {
    var e = EFits2Bits.fromSlice(EFits2Bits.sliceWithZero());
    return (e == EFits2Bits.ZERO, e == EFits2Bits.TWO);
}

@method_id(102)
fun test2(c: EFits2Bits) {
    try {
        return (c.toCell().beginParse().loadAny<EFits2Bits>() == c) as int
    } catch (ex) {
        return ex * 10
    }
}

@method_id(103)
fun test3() {
    return EStartFrom1.fromCell(EStartFrom1.ONE.toCell())
}

@method_id(104)
fun test4(v: EFillsAllUint3) {
    return EFillsAllUint3.fromCell(v.toCell())
}
    
@method_id(105)
fun test5(s: slice) {
    return EStartFromM2.fromSlice(s, {assertEndAfterReading: false})
}

@method_id(106)
fun test6() {
    try {
        return (4 as EFits2Bits).toCell().beginParse().loadUint(1)
    } catch (ex) {
        return ex
    }
}

@method_id(107)
fun test7(v: EFits8Bits) {
    try { return EFits8Bits.fromCell(v.toCell()) as int }
    catch (ex) { return ex * 1000 }
}

@method_id(108)
fun test8() {
    var s_min = beginCell().storeInt(-MAX_UINT - 1, 257).endCell();
    var s_max = beginCell().storeInt(+MAX_UINT, 257).endCell();
    return (
        EMinMax.fromCell(s_min) == EMinMax.MIN_INT, EMinMax.fromCell(s_max) == EMinMax.MAX_INT,
        EMinMax.fromCell(s_max) == EMinMax.MIN_INT, EMinMax.fromCell(s_min) == EMinMax.MAX_INT,
    )
}

@method_id(109)
fun test9() {
    var s_min = beginCell().storeUint(0, 256).endCell();
    var s_max = beginCell().storeUint(MAX_UINT, 256).endCell();
    return (
        E0Max.fromCell(s_min) == E0Max.ZERO, E0Max.fromCell(s_max) == E0Max.MAX_INT,
    )
}

@method_id(110)
fun test10() {
    var w1 = WithEnumsUnion{u: EFits8Bits.E110}.toCell().load();
    var w2 = WithEnumsUnion{u: (220 as EFits8Bits)}.toCell().load();
    var w3 = WithEnumsUnion{u: ([0] as tuple).get<EStartFromM2>(0)}.toCell().load();
    var w4 = WithEnumsUnion{u: EStartFromM2.P3}.toCell().load();
    return (
        w1.u is EFits8Bits && w1.u == EFits8Bits.E110,
        w2.u is EFits8Bits && w2.u == EFits8Bits.E220,
        w3.u is EStartFromM2 && w3.u == EStartFromM2.ZERO,
        w4.u is EStartFromM2 && w4.u == EStartFromM2.P3,
        w4.u is EFits8Bits && w4.u == EFits8Bits.E220,
    ) 
}

@method_id(111)
fun test11() {
    var (r1, r2) = (-1 as Role, -1 as Role);
    val before = (r1 == Role.Admin, r2 == Role.Admin);
    (r1, r2).decode(stringHexToSlice("0001"));
    return (before, r1, r2, r1 == Role.Admin, r2 == Role.Admin, r1 == Role.User, r2 == Role.User)
}

@method_id(112)
fun test12() {
    var c3_one = EncodedVari.fromSlice(stringHexToSlice("1018_"));
    var c3_many = EncodedVari.fromSlice(stringHexToSlice("d100000000000000000000000008_"));
    return (c3_one, c3_one == EncodedVari.MANY, c3_many == EncodedVari.MANY);
}

@method_id(113)
fun test13() {
    return (
        EOfCrc32.world.toCell().beginParse().loadUint(32),
        EOfCrc32.fromSlice(stringHexToSlice("3610a686")) == EOfCrc32.hello
    )
}

@method_id(114)
fun test14() {
    var ex = 0;
    try { ex = Amounts.fromSlice(stringHexToSlice("32dc6c08_")) as int }
    catch (no) { ex = no }
    return (
        Amounts.one.toCell().hash() == 101872335586569371892273454115677983665588664548247484580448471612407076042677,
        Amounts.more.toCell().hash() == 82789494441528276360388239783934598298305390626975708015812230018243378632521,
        beginCell().storeAny(Amounts.zero).bitsCount(),
        beginCell().storeAny(Amounts.more).bitsCount(),
        Amounts.fromSlice(stringHexToSlice("402faf080")) as int64,
        Amounts.fromSlice(stringHexToSlice("402faf080")) as coins == ton("0.05"),
        Amounts.fromSlice(stringHexToSlice("402faf080")) == Amounts.more,
        Amounts.more1,
        ex,
    )
}

fun main() {
    
}

/**
@testcase | 101 |               | -1 0
@testcase | 102 | 1             | -1
@testcase | 102 | 3             | 50
@testcase | 103 |               | 1
@testcase | 104 | 3             | 3
@testcase | 104 | 7             | 7
@testcase | 105 | x{d}          | -2
@testcase | 105 | x{7}          | 3
@testcase | 106 |               | 5
@testcase | 107 | 0             | 0
@testcase | 107 | 110           | 110
@testcase | 107 | 220           | 220
@testcase | 107 | -50           | 5000
@testcase | 107 | 1             | 5000
@testcase | 107 | 222           | 5000
@testcase | 108 |               | -1 -1 0 0
@testcase | 109 |               | -1 -1
@testcase | 110 |               | -1 -1 -1 -1 0
@testcase | 111 |               | 0 0 0 1 -1 0 0 -1
@testcase | 112 |               | 1 0 -1
@testcase | 113 |               | 980881731 -1
@testcase | 114 |               | -1 -1 4 36 50000000 -1 -1 50000001 5

@fif_codegen
"""
  test1() PROC:<{ 
    b{00} PUSHSLICE
    2 LDU
    OVER
    2 GTINT
    5 THROWIF
    ENDS
"""

@fif_codegen
"""
  test4() PROC:<{               //  v
    NEWC                        //  v b
    3 STU                       //  b
    BTOS                        //  s
    3 LDU                       //  '9 s
    ENDS                        //  '9
  }>
"""

@fif_codegen
"""
  test5() PROC:<{
    // s
    3 PLDI
    DUP
    -2 LESSINT
    5 THROWIF
    DUP
    3 GTINT
    5 THROWIF
  }>
"""

 */
