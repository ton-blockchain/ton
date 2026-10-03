struct TwoUint8 {
    a: uint8
    b: uint8
}

struct OneByte {
    x: uint8
}

struct (0x01) VariantA {
    x: int8
}

struct (0x03) VariantB {
}

type PreserveUnion = VariantA | VariantB

fun inlineDivResult(x: int): int {
    return 1 / x;
}

fun inlineArgOnly(x: int) {
}

fun inlineBeginCell(): builder {
    return beginCell();
}

@noinline
fun noinlineBeginCell(): builder {
    return beginCell();
}

@method_id(101)
fun notPreservedBeginCellCodegen(): int {
    beginCell();        // `NEWC; DROP` is removed by peephole, only `7 PUSHINT` remains
    return 7;
}

@method_id(102)
fun preserveLoadUintOnEmptySlice(): int {
    try {
        var s = createEmptySlice();
        s.loadUint(256);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(103)
fun preserveCascadeThroughUnusedVars(): int {
    try {
        var r1 = createEmptySlice();
        var r2 = r1;
        r2.loadUint(256);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(104)
fun preserveInlineResult(x: int): int {
    try {
        inlineDivResult(x);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(105)
fun preserveInlineArgExpression(x: int): int {
    try {
        inlineArgOnly(1 / x);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(106)
fun preserveSpecialGenFromSlice(): int {
    try {
        TwoUint8.fromSlice("01".hexToSlice(), { assertEndAfterReading: false });
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(107)
fun preserveArrayGet(): int {
    try {
        var a = createEmptyTuple() as array<int>;
        a.get(0);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(108)
fun preservePrimitiveFromSlice(): int {
    try {
        int32.fromSlice(createEmptySlice(), { assertEndAfterReading: false });
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(109)
fun preserveBareOperatorStatement(x: int): int {
    try {
        1 / x;
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(110)
fun inlineBeforeThrow() {
    inlineBeginCell();
    throw 1;
}

@method_id(111)
fun noinlineBeforeThrow() {
    noinlineBeginCell();
    throw 1;
}

@method_id(112)
fun preserveIndirectCall(): int {
    try {
        var cb: (int) -> int = fun(x) { return 1 / x };
        cb(0);
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(113)
fun preserveSetAssignExpression(x: int): int {
    try {
        var y = 1;
        y /= x;
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(114)
fun foldIfNeqZero(x: int): int {
    if (x != 0) {
        return 1;
    }
    return 0;
}

@method_id(115)
fun foldIfEqZero(x: int): int {
    if (x == 0) {
        return 1;
    }
    return 0;
}

@method_id(116)
fun foldAssertNeqZero(x: int): int {
    try {
        assert(x != 0) throw 123;
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(117)
fun foldTernaryNeqZero(x: int): int {
    return x != 0 ? 1 : 2;
}

fun negationShouldRemain(x: int): bool {
    return (-x * 0) == 0;
}

@method_id(118)
fun negationShouldRemainExcno(x: int): int {
    try {
        return negationShouldRemain(x) ? 0 : -1;
    } catch (excno) {
        return excno;
    }
}

@method_id(119)
fun preservePureOperators(x: int, y: int): int {
    x & y;
    x < y;
    return 7;
}

@method_id(120)
fun preserveShiftOverflow(x: int): int {
    try {
        1 << x;
        return 0;
    } catch (excno) {
        return excno;
    }
}

@method_id(121)
fun constFoldedOperatorElided(): int {
    1 << 88;
    return 7;
}

@method_id(122)
fun preserveFromSliceAlwaysTrueCompare(): int {
    try {
        val p = OneByte.fromSlice(createEmptySlice(), { assertEndAfterReading: false });
        if (p.x >= 0) {
            return 1;
        }
        return 2;
    } catch (excno) {
        return excno;
    }
}

@method_id(123)
fun preserveFromSliceUnusedSecondField(): int {
    try {
        val p = TwoUint8.fromSlice("01".hexToSlice(), { assertEndAfterReading: false });
        return p.a;
    } catch (excno) {
        return excno;
    }
}

@method_id(124)
fun preserveFromSliceTimesZero(): int {
    try {
        int32.fromSlice(createEmptySlice(), { assertEndAfterReading: false }) * 0;
        return 123;
    } catch (excno) {
        return excno;
    }
}

@method_id(125)
fun preserveFromSliceUnionIsVariant(): int {
    try {
        PreserveUnion.fromSlice("01".hexToSlice(), { assertEndAfterReading: false }) is VariantA;
        throw 123;
    } catch (excno) {
        return excno;
    }
}

@method_id(127)
fun preserveForceLoadLazyObject(): int {
    try {
        val p = lazy TwoUint8.fromSlice("01".hexToSlice());
        p.forceLoadLazyObject();
        return 0;
    } catch (excno) {
        return excno;
    }
}

fun main() {}

/**
@testcase | 101 |   | 7
@testcase | 102 |   | 9
@testcase | 103 |   | 9
@testcase | 104 | 0 | 4
@testcase | 104 | 2 | 0
@testcase | 105 | 0 | 4
@testcase | 105 | 2 | 0
@testcase | 106 |   | 9
@testcase | 107 |   | 5
@testcase | 108 |   | 9
@testcase | 109 | 0 | 4
@testcase | 109 | 5 | 0
@testcase | 112 |   | 4
@testcase | 113 | 0 | 4
@testcase | 113 | 1 | 0
@testcase | 114 | 0 | 0
@testcase | 114 | 9 | 1
@testcase | 115 | 0 | 1
@testcase | 115 | 9 | 0
@testcase | 116 | 0 | 123
@testcase | 116 | 9 | 0
@testcase | 117 | 0 | 2
@testcase | 117 | 9 | 1
@testcase | 118 | 5 | 0
@testcase | 118 | -1<<256 | 4
@testcase | 119 | 5 3 | 7
@testcase | 120 | 8 | 0
@testcase | 120 | 300 | 4
@testcase | 121 |   | 7
@testcase | 122 |   | 9
@testcase | 123 |   | 9
@testcase | 124 |   | 9
@testcase | 125 |   | 9
@testcase | 127 |   | 9

@fif_codegen
"""
notPreservedBeginCellCodegen() PROC:<{
  7 PUSHINT
}>
"""

@fif_codegen
"""
inlineBeforeThrow() PROC:<{
  1 THROW
}>
"""

@fif_codegen
"""
noinlineBeforeThrow() PROC:<{
  noinlineBeginCell() CALLDICT
  1 THROW
}>
"""

@fif_codegen
"""
foldIfNeqZero() PROC:<{
  IFJMP:<{
    1 PUSHINT
  }>
  0 PUSHINT
}>
"""

@fif_codegen
"""
foldIfEqZero() PROC:<{
  IFNOTJMP:<{
    1 PUSHINT
  }>
  0 PUSHINT
}>
"""

@fif_codegen
"""
foldAssertNeqZero() PROC:<{
  c2 SAVE
  SAMEALTSAVE
  CONT:<{
    NIP
    RETALT
    DROP
  }>
  0b10111010 SETCONTMANY
  CONT:<{
    123 THROWIFNOT
    0 PUSHINT
    RETALT
  }>
  c1 PUSH
  COMPOSALT
  SWAP
  TRY
}>
"""

@fif_codegen
"""
foldTernaryNeqZero() PROC:<{
  1 PUSHINT
  2 PUSHINT
  CONDSEL
}>
"""

@fif_codegen
"""
preservePureOperators() PROC:<{
  2DUP
  AND
  DROP
  LESS
  DROP
  7 PUSHINT
}>
"""

@fif_codegen
"""
constFoldedOperatorElided() PROC:<{
  7 PUSHINT
}>
"""

@fif_codegen
"""
  preserveFromSliceUnionIsVariant() PROC:<{
    c2 SAVE
    SAMEALTSAVE
    CONT:<{
      NIP
      RETALT
      DROP
    }>
    0b10111010 SETCONTMANY
    CONT:<{
      x{01} PUSHSLICE
      x{01} SDBEGINSQ
      IF:<{
        8 PLDI
        DROP
"""
 */
