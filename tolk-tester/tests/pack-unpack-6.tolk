struct (0x01) CounterIncrement { byValue: int8; }
struct (0x03) CounterDecrementBy1 {}
type CounterMsg = CounterIncrement | CounterDecrementBy1;

struct SomeBytesFields {
    f1: bytes1;
}

@method_id(101)
fun test1() {
    try {
        return CounterIncrement.fromSlice(stringHexToSlice("880f"), {throwIfOpcodeDoesNotMatch: 101}).byValue as int;
    } catch (excno) {
        return excno;
    }
}

@method_id(102)
fun test2() {
    try {
        return CounterIncrement.fromSlice(stringHexToSlice("890f")).byValue as int;
    } catch (excno) {
        return excno;
    }
}

@method_id(103)
fun test3() {
    var cc: Cell<CounterIncrement> = {
        tvmCell: beginCell().storeSlice(stringHexToSlice("0109ab")).endCell()
    };
    return cc.load({assertEndAfterReading: false});
}

@method_id(104)
fun test4() {
    try {
        var msg = CounterMsg.fromSlice(stringHexToSlice("88"), {throwIfOpcodeDoesNotMatch: 104, assertEndAfterReading: false});
        __expect_type(msg, "CounterMsg");
        return (-1, msg is CounterIncrement);
    } catch (excno) {
        return (excno, null);
    }
}

@method_id(105)
fun test5() {
    return SomeBytesFields { f1: stringHexToSlice("11") as bytes1 }.toCell().hash() & 0xFFFF;
}

@method_id(106)
fun test6() {
    return SomeBytesFields { f1: stringHexToSlice("11") as bytes1 }.toCell({skipBitsNFieldsValidation: true}).hash() & 0xFFFF;
}

@method_id(107)
fun test7(believe: bool) {
    try {
        return SomeBytesFields { f1: stringHexToSlice("ffff") as bytes1 }.toCell({skipBitsNFieldsValidation: believe}).hash() & 0xFFFF;
    } catch (excno) {
        return excno;
    }
}

@method_id(108)
fun test8() {
    return CounterIncrement.fromSlice(stringHexToSlice("010f"), {
        throwIfOpcodeDoesNotMatch: 0xFFFF
    }).byValue;
}

@method_id(109)
fun test9() {
    return CounterMsg.fromSlice(stringHexToSlice("010f"), {
        throwIfOpcodeDoesNotMatch: 0xFFFF,
        assertEndAfterReading: false,
    }) is CounterIncrement;
}

@method_id(110)
fun test10() {
    var c = beginCell().storeUint(123, 64).endCell() as Cell<SomeBytesFields>;
    try {
        var b = c.load();
        return -(b.f1 as slice).remainingBitsCount();
    } catch (excno) {
        return excno;
    }
}

@method_id(111)
fun test11() {
    var c = beginCell().storeUint(123, 64).endCell() as Cell<SomeBytesFields>;
    var b = c.load({assertEndAfterReading: false});
    return -(b.f1 as slice).remainingBitsCount();
}

fun main(){}

/**
@testcase | 101 |       | 101
@testcase | 102 |       | 63
@testcase | 103 |       | 9
@testcase | 104 |       | 104 (null)
@testcase | 105 |       | 36896
@testcase | 106 |       | 36896
@testcase | 107 | -1    | 2142
@testcase | 107 | 0     | 9
@testcase | 108 |       | 15
@testcase | 109 |       | -1
@testcase | 110 |       | 9
@testcase | 111 |       | -8

@fif_codegen
"""
x{880f} PUSHSLICE
x{01} SDBEGINSQ
101 THROWIFNOT
"""

@fif_codegen
"""
x{890f} PUSHSLICE
x{01} SDBEGINSQ
63 THROWIFNOT
8 LDI
ENDS
RETALT
"""

@fif_codegen
"""
  test3 PROC:<{
    NEWC
    x{0109ab} PUSHSLICE
    STSLICER
    ENDC
    CTOS
    x{01} SDBEGINSQ
    63 THROWIFNOT
    8 LDI
    DROP
  }>
"""

@fif_codegen
"""
IF:<{
  DROP
  129 PUSHINT
}>ELSE<{
  x{03} SDBEGINSQ
  NIP
  IFNOTJMP:<{
    104 THROW
  }>
  130 PUSHINT
}>
"""

@fif_codegen
"""
  test5 PROC:<{
    x{11} PUSHSLICE
    NEWC
    OVER
    SBITREFS
    9 THROWIF
    8 EQINT
    9 THROWIFNOT
    SWAP
    STSLICER
"""

@fif_codegen
"""
  test6 PROC:<{
    x{11} PUSHSLICE
    NEWC
    SWAP
    STSLICER
    ENDC
    HASHCU
"""

@fif_codegen
"""
  test8 PROC:<{                 //
    x{010f} PUSHSLICE           //  '0
    16 PUSHPOW2DEC              //  s '2=65535
    SWAP                        //  '2=65535 s
    x{01} SDBEGINSQ             //  '2=65535 s '4
    s1 s2 XCHG                  //  s '2=65535 '4
    THROWANYIFNOT               //  s
    8 LDI                       //  '9 s
    ENDS                        //  '9
  }>
"""

@fif_codegen
"""
  test9 PROC:<{                 //
    x{010f} PUSHSLICE           //  s
    x{01} SDBEGINSQ             //  s '6
    IF:<{                       //  s
      DROP                      //
      129 PUSHINT               //  'UTag=129
    }>ELSE<{                    //  s
      x{03} SDBEGINSQ           //  s '6
      NIP                       //  '6
      IFNOTJMP:<{               //
        16 PUSHPOW2DEC
        THROWANY
      }>
      130 PUSHINT               //  'UTag=130
    }>
    129 PUSHINT                 //  'UTag '17=129
    SWAP                        //  '17=129 'UTag
    EQUAL                       //  '16
  }>
"""

 */
