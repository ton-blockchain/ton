type MInt = int
type MSlice = slice

const int1 = 1
const int2 = 2

const int101: int = 101;
const int111: MInt = 111;

const int1r = int1;

const str1 = "const1";
const str2: MSlice = stringHexToSlice("aabbcc");

const str2r: slice = str2;

const str1int = 0x636f6e737431;
const str2int = 0xAABBCC;

const nibbles: int = 4;

const strange_zero = 1 >> 10;
const strange_minus_1: MInt = -1 as Color as int;

const addr1 = address("Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF");
const addr2: any_address = address("Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF") as any_address;

// since `;` is not mandatory, this is correct from a syntax point of view
const true1 = true const true2 = !!true const true3 = true1 && true2

const false1 = !true;
const false2 = false1 || false;

const tens1 = (1, 2);
const tens2: (int, int, (int8, int16)) = (tens1.1, tens1.0 << 2, tens1);

const intOrN: int? = null;
const int32Or64: int32 | int64 = 7 as int64;

const nameOfAddress = address.typeName()

fun iget1(): int { return int1; }
fun iget2(): int { return int2; }
fun iget3(): int { return int1+int2; }

fun iget1r(): int { return int1r; }

fun sget1(): slice { return str1; }
fun sget2(): slice { return str2; }
fun sget2r(): slice { return str2r; }

const int240: int = ((int1+int2)*10)<<3;

enum Color {
    red,
    green,
    blue,
}

const boolOpsWithInts = (10 < 3, 10 <= 3, 10 > 3, 10 >= 3, 10 == 3, 10 != 3, 10 <= 10, 10 >= (9 + 1), )
const boolOpsWithBools = (true == true, true != true, false == false, !!false != !!false)
const boolBitwise = (true & true, true & false, false & true, false & false, true | false, true ^ true, true ^ false)
const intBitwise = (2 & 3, 2 ^ 3, 2 | 3, 1 | 5, 0xFF & 0x80, !0, !100)
const alsoBlue = Color.blue
const also2 = Color.blue as int

const nullable5: int? = 5
const nullableTensor: (int, int)? = null
const nullableTricky: (UnpackOptions?, int?, (int, slice)?) = (null, null, null)
const unionTricky: (address | int, address | int, int | int8 | null, int8 | int16 | null) = (address("EQDKbjIcfM6ezt8KjKJJLshZJJSqX7XOA4ff-W72r5gqPrHF"), 100, 50, null);

struct SubParams {
    sub1: bool = true
    sub2: int8 = 8
    sub3: coins
}

struct MyOptions {
    allowPenalty: bool = false
    closeTo: (int8, int8)? = null
    sn: void
    sub: SubParams = { sub3: ton("0.05") }
    coeff: int16 = 2
}

const opts1 = MyOptions { allowPenalty: true }
const opts2 = MyOptions { coeff: 3, sub: { sub1: false, sub3: 0 } }
const opts3: MyOptions = { coeff: 0, closeTo: null, allowPenalty: true }
const opts4: MyOptions = { sub: { sub3: ton("1") }, closeTo: (3, 4), coeff: 55 }

@noinline
fun MyOptions.equals(self, rhs: MyOptions = {}) {
    return self.toCell().hash() == rhs.toCell().hash()
}

struct Wrapper<T> {
    item: T
}

const wOneTon = Wrapper<coins> { item: ton("1") }
const wNoTon: Wrapper<int> = { item: 0 }
const wNoSubParams: Wrapper<SubParams>? = null


fun iget240(): MInt { return int240; }

@pure
fun newc(): builder
asm "NEWC";
@pure
fun builder.endcs(self): slice
asm "ENDC" "CTOS";
@pure
fun sdeq(s1: slice, s2: slice): MInt
asm "SDEQ";
@pure
fun builder.stslicer(self, s: slice): builder
asm "STSLICER";

@method_id(101)
fun test1() {
    return (strange_zero, strange_minus_1);
}

@method_id(102)
fun test2() {
    return (true1, true2, true3);
}

@method_id(103)
fun test3() {
    return (false1, false2);
}

@method_id(104)
fun test4() {
    __expect_type(tens1, "(int, int)");
    return (tens1.0, tens2.2);
}

@method_id(105)
fun test5() {
    return (intOrN == null, int32Or64 is int32, int32Or64);
}

@method_id(106)
fun test6() {
    __expect_type(addr1, "address");
    __expect_type(addr2, "any_address");
    return (addr1 == addr1, addr1 != addr1, addr1 == createAddressNone(), addr1.getWorkchain(), addr1 == addr2);
}

@method_id(107)
fun test7() {
    return boolOpsWithInts
}

@method_id(108)
fun test8() {
    return boolOpsWithBools
}

@method_id(109)
fun test9() {
    return boolBitwise
}

@method_id(110)
fun test10() {
    return intBitwise
}

@method_id(111)
fun test11() {
    __expect_type(alsoBlue, "Color");
    __expect_type(also2, "int");
    return (alsoBlue, also2)
}

@method_id(112)
fun test12() {
    return nullableTensor
}

@method_id(113)
fun test13() {
    return nullableTricky
}

@method_id(114)
fun test14() {
    return (unionTricky.0 is address, unionTricky.1 is address, unionTricky.1 is int, unionTricky.1 is slice, unionTricky.1, 777, unionTricky.2, unionTricky.3)
}

@method_id(115)
fun test15() {
    return (
        opts1.equals(), opts1.equals(opts1), opts1.equals(opts2), opts1.equals(opts3), opts1.equals(opts4),  
        opts4.equals(), opts4.equals(opts1), opts4.equals(opts2), opts4.equals(opts3), opts4.equals(opts4),
        777,
        opts1.equals({ closeTo: null, allowPenalty: true }),
        opts2.equals({ coeff: 3, sub: { sub1: false, sub3: 0 } }),
        opts3.equals({ closeTo: null, coeff: 0, allowPenalty: true }),
        opts4.equals({ sub: { sub3: ton("1") }, coeff: 55, closeTo: (3, 4) })
    )
}

@method_id(116)
fun test16() {
    __expect_type(wOneTon, "Wrapper<coins>");
    return (wOneTon, wNoTon, wNoSubParams)
}

@method_id(117)
fun test17() {
    __expect_type(nameOfAddress, "slice");
    return (nameOfAddress.bitsEqual("address"), nameOfAddress.typeNameOfObject().bitsEqual("slice"))
}

fun main() {
    var i1: int = iget1();
    var i2: int = iget2();
    var i3: int = iget3();

    assert(i1 == 1) throw int101;
    assert(i2 == 2) throw 102;
    assert(i3 == 3) throw 103;

    var s1: slice = sget1();
    var s2: slice = sget2();
    var s3: slice = newc().stslicer(str1).stslicer(str2r).endcs();

    assert(sdeq(s1, newc().storeUint(str1int, 12 * nibbles).endcs())) throw int111;
    assert(sdeq(s2, newc().storeUint(str2int, 6 * nibbles).endcs())) throw 112;
    assert(sdeq(s3, newc().storeUint(0x636f6e737431AABBCC, 18 * nibbles).endcs())) throw 113;

    var i4: int = iget240();
    assert(i4 == 240) throw ((104));
    return 0;
}

/**
@testcase | 0   |   | 0
@testcase | 101 |   | 0 -1
@testcase | 102 |   | -1 -1 -1
@testcase | 103 |   | 0 0
@testcase | 104 |   | 1 1 2
@testcase | 105 |   | -1 0 7 48
@testcase | 106 |   | -1 0 0 -1 -1
@testcase | 107 |   | 0 0 -1 -1 0 -1 -1 -1
@testcase | 108 |   | -1 0 -1 0
@testcase | 109 |   | -1 0 0 0 -1 0 -1
@testcase | 110 |   | 2 1 3 5 128 -1 0
@testcase | 111 |   | 2 2
@testcase | 112 |   | (null) (null) 0
@testcase | 113 |   | (null) (null) 0 (null) (null) (null) 0
@testcase | 114 |   | -1 0 -1 0 100 1 777 50 1 (null) 0
@testcase | 115 |   | 0 -1 0 0 0 0 0 0 0 -1 777 -1 -1 -1 -1
@testcase | 116 |   | 1000000000 0 (null) (null) (null) 0
@testcase | 117 |   | -1 -1

@code_hash 75101380693091438363237198341416144164642759114287088786910200935773618931276
*/
