struct JustInt32 {
    value: int32;
}

type MaybeInt32 = int32?;
type MaybeCell = cell?;
type Int16Or32 = int16 | int32;

struct JustMaybeInt32 {
    value: MaybeInt32;
}

fun T.estimate(): [int, int, int, int] {
    // convert a tensor to a tuple for better output readability
    val (minBits, maxBits, minRefs, maxRefs) = T.estimatePackSize();
    return [minBits, maxBits, minRefs, maxRefs];
}

@method_id(101)
fun test1() {
    return (
        int32.estimate(),
        uint64.estimate(),
        int1.estimate(),
        bool.estimate(),
        RemainingBitsAndRefs.estimate(),
        coins.estimate(),
        bits6.estimate(),
        bytes8.estimate(),
        varint32.estimate(),
    );
}

@method_id(102)
fun test2() {
    return (
        JustInt32.estimate(),
        JustMaybeInt32.estimate(),
        MaybeInt32.estimate(),
        Int16Or32.estimate(),
    );
}

struct Test3_1 {
    f1: JustInt32,
    f2: JustMaybeInt32,
    f3: JustMaybeInt32,
    f4: JustInt32 | uint100,
}

struct Test3_2 {
    f1: Test3_1;
    f2: Test3_1?;
    f3: null | Test3_1 | null;
}

@method_id(103)
fun test3() {
    return (Test3_1.estimate(), Test3_2.estimate());
}

struct Test4_1 {
    f1: address;
    f2: address;
}

struct Test4_2 {
    f1: address?;
    f2: address?;
}

@overflow1023_policy("suppress")
struct Test4_3 {
    f: (Test4_1, Test4_2);
}

@method_id(104)
fun test4() {
    return (Test4_1.estimate(), Test4_2.estimate(), Test4_3.estimate());
}

struct Test5_1 {
    f1: cell;
    f2: cell?;
    f3: Cell<int32>;
    f4: Cell<int32>?;
}

struct Test5_2 {
    f1: Cell<address?>?;
    f2: Cell<int32 | int64>?;
    f3: Cell<Cell<int8>>?;
    f4: Cell<cell>?;
    f5: Cell<Test5_1>?;
    rest: RemainingBitsAndRefs;
}

struct Test5_3 {
    f1: int8 | Cell<int8>;
    f2: bytes2 | Cell<int16>;
    f3: Cell<int8> | cell;
    f4: Cell<coins> | coins;
}

@method_id(105)
fun test5() {
    return (Test5_1.estimate(), Test5_2.estimate(), Test5_3.estimate());
}

struct(0x00112233) Test6_1 {
    f1: int32;
}

struct(0b0010) Test6_2 {
    f1: int32?;
    f2: Cell<int10>;
}

type Test6_or = Test6_1 | Test6_2;

@method_id(106)
fun test6() {
    return (Test6_1.estimate(), Test6_2.estimate(), Test6_or.estimate());
}

struct(0x1020) Test7_1;
struct(0x1030) Test7_2;
struct(0x1040) Test7_3;

type Test7_or = | Test7_1 | Test7_2 | Test7_3

@method_id(107)
fun test7() {
    assert((Test7_1{} as Test7_or).toCell().beginParse().remainingBitsCount() == Test7_or.estimatePackSize().0, 400);
    return (Test7_1.estimate(), Test7_or.estimate());
}

struct(0x10) Inner8_1 {
    ea: any_address;
}
struct(0b1) CellInner8_1 {
    ref: Cell<Inner8_1>;
}
struct Inner8_2 {
    t: (bits32, int1, coins?);
}

@overflow1023_policy("suppress")
struct Test8 {
    f1: Inner8_1;
    f2: Inner8_2;
    f3: Inner8_1?;
    f4: Inner8_2?;
    f5: Inner8_1 | CellInner8_1;
}

@method_id(108)
fun test8() {
    return (Inner8_1.estimate(), Inner8_2.estimate(), Test8.estimate());
}

struct Test9_bits2 { f: bits2; }
struct Test9_bits4 { f: bits4; }

type Test9_f1 = int32 | int64 | int128;                         // auto-generated 2-bit prefix
type Test9_f2 = int32 | Inner8_2;                               // auto-generated 1-bit prefix (Either)
type Test9_f3 = bits1 | Test9_bits2 | bits3 | bits4 | bits5;    // auto-generated 3-bit prefix
type Test9_f4 = bits1 | Test9_bits2 | bits3 | Test9_bits4?;     // auto-generated 0 / 100/101/110/111

@method_id(109)
fun test9() {
    return (Test9_f1.estimate(), Test9_f2.estimate(), Test9_f3.estimate(), Test9_f4.estimate());
}

struct Test10_1 {
    a: int32;
    b: builder;     // unpredictable
}

type Test10_2 = (Test10_1, bool?, RemainingBitsAndRefs);

@method_id(110)
fun test10() {
    return (Test10_1.estimate(), Test10_2.estimate());
}

struct Test11_1 {
    data: bits1022;
    next: Cell<Test11_1>?;
}

struct Test11_2 {
    self1: Cell<Test11_2>;
    self2: Cell<Test11_2>;
}

@method_id(111)
fun test11() {
    return (Test11_1.estimate(), Test11_2.estimate());
}

@method_id(120)
fun test20() {
    return (Test7_1 .getDeclaredPackPrefixLen(), Test7_1 .getDeclaredPackPrefix(),
        CellInner8_1.getDeclaredPackPrefixLen(), CellInner8_1.getDeclaredPackPrefix());
}

enum Color { Red, Green, Blue }
enum EInt9 { P133 = 133, M1 = -1 }
enum EUint6 { P63 = 63, P30 = 30, P1 = 1 }
enum EUint1 { A, B }
enum EInt1 { A = -1, B }
enum EUint123 { Z, V5 = 5, E123 = 1<<122, E123_2 = (1<<122) + (1<<121) }
type EUint123Alias = EUint123

@method_id(121)
fun test21() {
    return (
        Color.estimate(), EInt9.estimate(), EUint6.estimate(), 
        EUint1.estimate(), EInt1.estimate(), EInt1.estimate(), 
        EUint123.estimate(), EUint123Alias.estimate(),
    )
}

struct JustAddress {
    a: address
}
type Nullable<T> = T | null
type OwnerAddress = address

@method_id(122)
fun test22() {
    return (
        address.estimate(), Nullable<address>.estimate(),
        JustAddress.estimate(), Nullable<JustAddress>.estimate(),
        OwnerAddress.estimate(), Nullable<OwnerAddress>.estimate(),
        any_address.estimate(), Nullable<any_address>.estimate(),
    )
}

fun main() {
    __expect_type(int8.estimatePackSize(), "(int, int, int, int)");
}

/**
@testcase | 101 | | [ 32 32 0 0 ] [ 64 64 0 0 ] [ 1 1 0 0 ] [ 1 1 0 0 ] [ 0 9999 0 4 ] [ 4 124 0 0 ] [ 6 6 0 0 ] [ 64 64 0 0 ] [ 5 253 0 0 ]
@testcase | 102 | | [ 32 32 0 0 ] [ 1 33 0 0 ] [ 1 33 0 0 ] [ 17 33 0 0 ]
@testcase | 103 | | [ 67 199 0 0 ] [ 69 599 0 0 ]
@testcase | 104 | | [ 534 534 0 0 ] [ 4 534 0 0 ] [ 538 1068 0 0 ]
@testcase | 105 | | [ 2 2 2 4 ] [ 5 9999 0 9 ] [ 4 152 1 4 ]
@testcase | 106 | | [ 64 64 0 0 ] [ 5 37 1 1 ] [ 5 65 0 1 ]
@testcase | 107 | | [ 16 16 0 0 ] [ 16 16 0 0 ]
@testcase | 108 | | [ 10 531 0 0 ] [ 34 158 0 0 ] [ 47 1911 0 1 ]
@testcase | 109 | | [ 34 130 0 0 ] [ 33 159 0 0 ] [ 4 8 0 0 ] [ 1 7 0 0 ]
@testcase | 110 | | [ 32 9999 0 4 ] [ 33 9999 0 8 ]
@testcase | 111 | | [ 1023 1023 0 1 ] [ 0 0 2 2 ]
@testcase | 120 | | 16 4128 1 1
@testcase | 121 | | [ 2 2 0 0 ] [ 9 9 0 0 ] [ 6 6 0 0 ] [ 1 1 0 0 ] [ 1 1 0 0 ] [ 1 1 0 0 ] [ 123 123 0 0 ] [ 123 123 0 0 ] 
@testcase | 122 | | [ 267 267 0 0 ] [ 2 267 0 0 ] [ 267 267 0 0 ] [ 1 268 0 0 ] [ 267 267 0 0 ] [ 2 267 0 0 ] [ 2 523 0 0 ] [ 1 524 0 0 ] 
 */
