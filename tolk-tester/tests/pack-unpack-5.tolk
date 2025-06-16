struct JustInt32 {
    value: int32;
}

type MaybeInt32 = int32?;
type MaybeCell = cell?;
type Int16Or32 = int16 | int32;

struct JustMaybeInt32 {
    value: MaybeInt32;
}


@method_id(101)
fun test1() {
    return (
        int32.estimatePackSize(),
        uint64.estimatePackSize(),
        int1.estimatePackSize(),
        bool.estimatePackSize(),
        RemainingBitsAndRefs.estimatePackSize(),
        coins.estimatePackSize(),
        bits6.estimatePackSize(),
        bytes8.estimatePackSize(),
    );
}

@method_id(102)
fun test2() {
    return (
        JustInt32.estimatePackSize(),
        JustMaybeInt32.estimatePackSize(),
        MaybeInt32.estimatePackSize(),
        Int16Or32.estimatePackSize(),
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
    return (Test3_1.estimatePackSize(), Test3_2.estimatePackSize());
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
    return (Test4_1.estimatePackSize(), Test4_2.estimatePackSize(), Test4_3.estimatePackSize());
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
    return (Test5_1.estimatePackSize(), Test5_2.estimatePackSize(), Test5_3.estimatePackSize());
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
    return (Test6_1.estimatePackSize(), Test6_2.estimatePackSize(), Test6_or.estimatePackSize());
}

struct(0x1020) Test7_1 { }
struct(0x1030) Test7_2 { }
struct(0x1040) Test7_3 { }

type Test7_or = Test7_1 | Test7_2 | Test7_3 |;

@method_id(107)
fun test7() {
    assert((Test7_1{} as Test7_or).toCell().beginParse().remainingBitsCount() == Test7_or.estimatePackSize().0, 400);
    return (Test7_1.estimatePackSize(), Test7_or.estimatePackSize());
}

struct(0x10) Inner8_1 {
    ea: address;
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
    return (Inner8_1.estimatePackSize(), Inner8_2.estimatePackSize(), Test8.estimatePackSize());
}

struct Test9_bits2 { f: bits2; }
struct Test9_bits4 { f: bits4; }

type Test9_f1 = int32 | int64 | int128;                         // auto-generated 2-bit prefix
type Test9_f2 = int32 | Inner8_2;                               // auto-generated 1-bit prefix (Either)
type Test9_f3 = bits1 | Test9_bits2 | bits3 | bits4 | bits5;    // auto-generated 3-bit prefix
type Test9_f4 = bits1 | Test9_bits2 | bits3 | Test9_bits4?;     // auto-generated 3-bit prefix

@method_id(109)
fun test9() {
    return (Test9_f1.estimatePackSize(), Test9_f2.estimatePackSize(), Test9_f3.estimatePackSize(), Test9_f4.estimatePackSize());
}

struct Test10_1 {
    a: int32;
    b: builder;     // unpredictable
}

type Test10_2 = (Test10_1, bool?, RemainingBitsAndRefs);

@method_id(110)
fun test10() {
    return (Test10_1.estimatePackSize(), Test10_2.estimatePackSize());
}

@method_id(120)
fun test20() {
    return (Test7_1 .getDeclaredPackPrefixLen(), Test7_1 .getDeclaredPackPrefix(),
        CellInner8_1.getDeclaredPackPrefixLen(), CellInner8_1.getDeclaredPackPrefix());
}

fun main() {
    __expect_type(int8.estimatePackSize(), "[int, int, int, int]");
}

/**
@testcase | 101 | | [ 32 32 0 0 ] [ 64 64 0 0 ] [ 1 1 0 0 ] [ 1 1 0 0 ] [ 0 9999 0 4 ] [ 4 124 0 0 ] [ 6 6 0 0 ] [ 64 64 0 0 ]
@testcase | 102 | | [ 32 32 0 0 ] [ 1 33 0 0 ] [ 1 33 0 0 ] [ 17 33 0 0 ]
@testcase | 103 | | [ 67 199 0 0 ] [ 69 599 0 0 ]
@testcase | 104 | | [ 4 534 0 0 ] [ 2 536 0 0 ] [ 6 1070 0 0 ]
@testcase | 105 | | [ 2 2 2 4 ] [ 5 9999 0 9 ] [ 4 152 1 4 ]
@testcase | 106 | | [ 64 64 0 0 ] [ 5 37 1 1 ] [ 5 65 0 1 ]
@testcase | 107 | | [ 16 16 0 0 ] [ 16 16 0 0 ]
@testcase | 108 | | [ 10 275 0 0 ] [ 34 158 0 0 ] [ 47 1143 0 1 ]
@testcase | 109 | | [ 34 130 0 0 ] [ 33 159 0 0 ] [ 4 8 0 0 ] [ 3 7 0 0 ]
@testcase | 110 | | [ 32 9999 0 4 ] [ 33 9999 0 8 ]
@testcase | 120 | | 16 4128 1 1
 */
