
fun slice.assertEqDeeply(self, rhs: slice): slice {
    var lhs = self;
    assert(lhs.bitsEqual(rhs), 400);
    assert(lhs.remainingRefsCount() == rhs.remainingRefsCount(), 400);
    while (lhs.remainingRefsCount()) {
        lhs.loadRef().beginParse().assertEqDeeply(rhs.loadRef().beginParse());
    }
    return self;
}


fun run<TInputStruct>(input: TInputStruct, ans: slice) {
    repeat (2) {
        var s = input.toCell().beginParse();
        input = TInputStruct.fromSlice(s.assertEqDeeply(ans));
    }
    input.toCell().beginParse().skipAny<TInputStruct>().assertEnd();
}


type Union_8_16_32 = int8 | int16 | int32;

fun ans101_8(): slice asm "b{0000001111} PUSHSLICE";
fun ans101_16(): slice asm "b{010000000000001111} PUSHSLICE";
fun ans101_32(): slice asm "b{1000000000000000000000000000001111} PUSHSLICE";

@method_id(101)
fun test1() {
    run<Union_8_16_32>(15 as int8, ans101_8());
    run<Union_8_16_32>(15 as int16, ans101_16());
    run<Union_8_16_32>(15 as int32, ans101_32());

    return 0;
}


type Union_8_16_32_n = int8 | null | int16 | int32;

fun ans102n_8(): slice asm "b{0100001111} PUSHSLICE";
fun ans102n_16(): slice asm "b{100000000000001111} PUSHSLICE";
fun ans102n_32(): slice asm "b{1100000000000000000000000000001111} PUSHSLICE";
fun ans102n_null(): slice asm "b{00} PUSHSLICE";

@method_id(102)
fun test2() {
    run<Union_8_16_32_n>(15 as int8, ans102n_8());
    run<Union_8_16_32_n>(15 as int16, ans102n_16());
    run<Union_8_16_32_n>(15 as int32, ans102n_32());
    run<Union_8_16_32_n>(null, ans102n_null());

    return 0;
}


struct Test4_8 { a: int8 }
struct Test4_16 { a: int16 }
struct Test4_32 { a: int32 }

type UnionStructs_8_16_32 = int8 | Test4_16 | Test4_32;
type UnionStructs_8_16_32_n = Test4_8 | null | int16 | int32;

fun ans104_8(): slice asm "b{0000001111} PUSHSLICE";
fun ans104_16(): slice asm "b{010000000000001111} PUSHSLICE";
fun ans104_32(): slice asm "b{1000000000000000000000000000001111} PUSHSLICE";

fun ans104n_8(): slice asm "b{0100001111} PUSHSLICE";
fun ans104n_16(): slice asm "b{100000000000001111} PUSHSLICE";
fun ans104n_32(): slice asm "b{1100000000000000000000000000001111} PUSHSLICE";
fun ans104n_null(): slice asm "b{00} PUSHSLICE";

@method_id(104)
fun test4() {
    // when mixing primitives and structs with no opcode, it's like mixing primitives
    run<UnionStructs_8_16_32>(15, ans104_8());
    run<UnionStructs_8_16_32>(Test4_16{a:15}, ans104_16());
    run<UnionStructs_8_16_32>(Test4_32{a:15}, ans104_32());
    // with null — the same behavior
    run<UnionStructs_8_16_32_n>({a:15}, ans104n_8());
    run<UnionStructs_8_16_32_n>(15 as int16, ans104n_16());
    run<UnionStructs_8_16_32_n>(15 as int32, ans104n_32());
    run<UnionStructs_8_16_32_n>(null, ans104n_null());

    return 0;
}


type U105 = int8 | int32 | int64;   // auto-prefixes 0b00 0b01 0b10

fun invalid105_slice(): slice asm "b{1100} PUSHSLICE";  // prefix 0b11 doesn't exist

@method_id(105)
fun test5() {
    try {
        var u = U105.fromSlice(invalid105_slice(), {throwIfOpcodeDoesNotMatch: 9});
        return (u is int8) ? 8 : -8;
    } catch (excode) {
        return excode;
    }
}


type U106 = int8 | int16 | int32 | int64;   // exhaustive prefixes, checked via codegen

fun s106_int16(): slice asm "b{010000000000001111} PUSHSLICE";

@method_id(106)
fun test6(): int16 {
    var u = U106.fromSlice(s106_int16());
    if (u is int16) { return u; }
    else { return -1; }
}


fun main() {}

/**
@testcase | 101 | | 0
@testcase | 102 | | 0
@testcase | 104 | | 0
@testcase | 105 | | 9
@testcase | 106 | | 15

@fif_codegen
"""
  test6 PROC:<{                 //
    b{010000000000001111} PUSHSLICE     //  s
    b{00} SDBEGINSQ             //  s '8
    IF:<{                       //  s
      8 LDI                     //  '12 s
      42 PUSHINT                //  'USlot1 s 'UTag=42
    }>ELSE<{                    //  s
      b{01} SDBEGINSQ           //  s '8
      IF:<{                     //  s
        16 LDI                  //  '17 s
        44 PUSHINT              //  'USlot1 s 'UTag=44
      }>ELSE<{                  //  s
        b{10} SDBEGINSQ         //  s '8
        IF:<{                   //  s
          32 LDI                //  '22 s
          46 PUSHINT            //  'USlot1 s 'UTag=46
        }>ELSE<{                //  s
          b{11} SDBEGINSQ       //  s '8
          IFNOTJMP:<{           //  s
            63 THROW
          }>
          64 LDI                //  '27 s
          48 PUSHINT            //  'USlot1 s 'UTag=48
        }>
      }>
    }>
    SWAP                        //  'USlot1 'UTag s
    ENDS                        //  u.USlot1 u.UTag
    44 EQINT                    //  u.USlot1 '29
    IFJMP:<{                    //  u.USlot1
    }>                          //  u.USlot1
    DROP                        //
    -1 PUSHINT                  //  '31=-1
  }>
"""
 */

