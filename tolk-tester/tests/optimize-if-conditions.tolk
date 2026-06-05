// Various tests that optimize `if (x != 0)` to `if (x)`, as well as loops / ternary / assert
// (Yes, I asked AI to generate tricky testscases, but they look reasonable)

global gT: tuple
global gG: int

// --- 1. Multi-use cond-var: DCE must keep the `_!=_` if `c` is used elsewhere ---

@method_id(101)
fun test1MultiUse(x: int): (int, bool) {
    val c = x != 0;
    var r = 0;
    if (c) { r = 100; }
    return (r, c);
}

// --- 2. Two-level inline through `_Let` chain ---

@inline
fun __chk2A(x: int): bool { return x != 0 }
@inline
fun __chk2B(x: int): bool { return __chk2A(x) }

@method_id(102)
fun test2InlineChain(x: int): int {
    if (__chk2B(x)) { return 7; }
    return 13;
}

// --- 3. Side-effect operand: pure-impure split ---

@inline
fun __impureChk(g0: int): int {
    gG = g0 + 1;
    return gG;
}

@method_id(103)
fun test3Impure(g0: int): int {
    if (__impureChk(g0) != 0) { return 1; }
    return 2;
}

// --- 4. Const 0 on the LEFT — `0 != x` ---

@method_id(104)
fun test4ZeroLeft(x: int): int {
    if (0 != x) { return 1; }
    return 0;
}

// --- 5. Same as #4, but via inlined helper ---

@inline
fun __zeroLeft(x: int): bool { return 0 != x }

@method_id(105)
fun test5ZeroLeftInlined(x: int): int {
    if (__zeroLeft(x)) { return 1; }
    return 0;
}

// --- 6. Comparison with non-zero const (`!= 5`) must NOT be optimized ---

@method_id(106)
fun test6NonZero(x: int): int {
    if (x != 5) { return 1; }
    return 0;
}

// --- 7. Same for `== 7` ---

@method_id(107)
fun test7NonZeroEq(x: int): int {
    if (x == 7) { return 1; }
    return 0;
}

// --- 8. Compile-time `5 != 0` — `fwd_analyze` should fold it; our pass must not break that ---

@inline
fun __ret5neq0(): bool { return 5 != 0 }

@method_id(108)
fun test8CompTime(): int {
    if (__ret5neq0()) { return 100; }
    return 200;
}

// --- 9. `==`-swap with else branches (both ending in returns) ---

@inline
fun __isEmptyCnt(c: int): bool { return c == 0 }

@method_id(109)
fun test9IsEmpty(c: int): int {
    if (__isEmptyCnt(c)) { return 100; }
    return 200;
}

// --- 10. `while` with inlined `!= 0` — must drop `0 NEQINT` inside loop cond ---

@inline
fun __hasMore(it: int): bool { return it != 0 }

@method_id(110)
fun test10WhileInline(): int {
    var i = 5;
    var s = 0;
    while (__hasMore(i)) {
        s += i;
        i -= 1;
    }
    return s;
}

// --- 11. `do-until` with inlined `!= 0` ---

@method_id(111)
fun test11DoWhile(): int {
    var i = 4;
    var s = 0;
    do {
        s += i;
        i -= 1;
    } while (__hasMore(i));
    return s;
}

// --- 12. if/else-if chain with `== 0`, `== 1` (match-style — `mark_noreturn::Phase 1`) ---

@method_id(112)
fun test12MatchChain(k: int): tuple {
    gT = createEmptyTuple();
    if (k == 0)      { gT.push(100); return gT; }
    else if (k == 1) { gT.push(200); return gT; }
    else if (k == 2) { gT.push(300); return gT; }
    return gT;
}

// --- 13. `&&` with embedded `!= 0` ---

@method_id(113)
fun test13AndNeq(a: int, x: int): int {
    if (a > 0 && x != 0) { return 1; }
    return 0;
}

// --- 14. `||` with embedded `== 0` ---

@method_id(114)
fun test14OrEq(a: int, x: int): int {
    if (a < 0 || x == 0) { return 1; }
    return 0;
}

// --- 15. Inside `try { if (...) ... } catch` ---

@inline
fun __hasNext(s: int): bool { return s != 0 }

@method_id(115)
fun test15TryCatch(s: int): int {
    try {
        if (__hasNext(s)) { return 1; }
        return 2;
    } catch {
        return 3;
    }
}

// --- 16. Ternary: `let r = (x != 0) ? a : b` (handled by `__condsel`) ---

@method_id(116)
fun test16Ternary(x: int, a: int, b: int): int {
    return (x != 0) ? a : b;
}

// --- 17. Nested loop+if with both `!=` and `==` simultaneously ---

@method_id(117)
fun test17Nested(): int {
    var a = 3;
    var b = 0;
    var c = 0;
    while (__hasMore(a)) {
        if (b == 0) { c += 1; } else { c += 2; }
        a -= 1;
        b += 1;
    }
    return c;
}

// --- 18. `bool != false` — AST normalization should strip it before IR ---

@method_id(118)
fun test18BoolNeqFalse(b: bool): int {
    if (b != false) { return 1; }
    return 0;
}

// === Below: tests for `__condsel` (lowered `?:`) and `__throw_if/__throw_ifnot`
//     (lowered `assert(...)`); they exercise the pseudo-builtin branch of the
//     IR pass — strip `_!=_(x,0)`, swap operands for `_==_(x,0)` in `__condsel`,
//     flip `__throw_if` <-> `__throw_ifnot` for `_==_(x,0)` in asserts. ===

// --- 19. Ternary strip: `(x != 0) ? a : b` -> `CONDSEL` without `0 NEQINT` ---

@method_id(119)
fun test19TernaryStrip(x: int, a: int, b: int): int {
    return (x != 0) ? a : b;
}

// --- 20. Ternary flip: `(x == 0) ? a : b` -> swap when_true/when_false,
//        emitted as `SWAP CONDSEL` (no `0 EQINT`) ---

@method_id(120)
fun test20TernaryFlip(x: int, a: int, b: int): int {
    return (x == 0) ? a : b;
}

// --- 21. Ternary with INLINED `!= 0` cond — fold happens AFTER inline,
//        which the AST-level `maybe_strip_neq_0_from_condition` could not do ---

@inline
fun __nz(x: int): bool { return x != 0 }

@method_id(121)
fun test21TernaryInlNeq(x: int, a: int, b: int): int {
    return __nz(x) ? a : b;
}

// --- 22. Ternary with INLINED `== 0` cond — verify the swap fires post-inline.
//        This case is NOT reachable at AST level (helper only stripped `!= 0`) ---

@inline
fun __ez(x: int): bool { return x == 0 }

@method_id(122)
fun test22TernaryInlEq(x: int, a: int, b: int): int {
    return __ez(x) ? a : b;
}

// --- 23. Nested ternaries: outer `(a != 0) ? <inner> : z`, inner `(b == 0) ? x : y`.
//        Both should be folded; the inner one (with `==`) gets its operands swapped ---

@method_id(123)
fun test23TernaryNested(a: int, b: int, x: int, y: int, z: int): int {
    return (a != 0) ? ((b == 0) ? x : y) : z;
}

// --- 24. Multi-use cond_var: `c = x != 0` used in BOTH `c ? a : b` AND returned.
//        Strip happens in the ternary, BUT `_!=_` must stay alive for the return —
//        regression for the `args.clear()` interaction with DCE  ---

@method_id(124)
fun test24TernaryMultiUse(x: int, a: int, b: int): (int, bool) {
    val c = x != 0;
    return (c ? a : b, c);
}

// --- 25. `assert(x != 0, N)` — strip → `THROWIFNOT` (and no `0 NEQINT`) ---

@method_id(125)
fun test25AssertStrip(x: int): int {
    assert(x != 0, 555);
    return x + 1;
}

// --- 26. `assert(x == 0, N)` — flip `__throw_ifnot` to `__throw_if` → `THROWIF`
//        (NOT achievable at AST level: the old helper only handled `!= 0`) ---

@method_id(126)
fun test26AssertFlip(x: int): int {
    assert(x == 0, 666);
    return x + 100;
}

// --- 27. `assert(__ez(x), N)` — inlined `== 0` cond + flip; this is the case
//        that originally exposed the args-staleness bug (without `args.clear()`,
//        DCE kept `0 EQINT` alive and the asm peephole `0 EQINT THROWIF -> THROWIFNOT`
//        silently flipped semantics) ---

@method_id(127)
fun test27AssertInlEq(x: int): int {
    assert(__ez(x), 888);
    return x;
}

// --- 28. assert with side-effecting cond producer: the `_!=_` is DCE'd, but the
//        impure call MUST stay (its result feeds the THROWIFNOT, and it has a
//        global write) ---

@inline
fun __sideEff(g0: int): int { gG = g0 + 1; return gG; }

@method_id(128)
fun test28AssertImpure(g0: int): int {
    assert(__sideEff(g0) != 0, 999);
    return gG;
}

// --- 29. `assert(x != 0, N); return (x != 0) ? 1 : 2;` — the same `_!=_(x, 0)`
//        is shared between assert and ternary. After BOTH are folded, the
//        `_!=_` producer must be DCE'd (regression: ensure args.clear() fires
//        for both consumers, not just one) ---

@method_id(129)
fun test29AssertAndTernary(x: int): int {
    assert(x != 0, 100);
    return (x != 0) ? 1 : 2;
}

// --- 30. Compile-time constant cond: `assert(5 != 0, N)` — fwd_analyze should
//        propagate `always_true`, then `compile_throw_if_ifnot` skips the whole op.
//        Our pass must not break this fold — even when the const survives long
//        enough to be matched as `_!=_(5, 0)` ---

@method_id(130)
fun test30AssertConstTrue(): int {
    assert(5 != 0, 100500);
    return 42;
}

// --- 31. Zero on the LEFT inside ternary: `(0 == x) ? a : b` — verify the
//        `args[0] is_int_const == 0` branch of `match_eq_neq_zero` fires, then
//        we still swap `__condsel` operands ---

@method_id(131)
fun test31TernaryZeroOnLeft(x: int, a: int, b: int): int {
    return (0 == x) ? a : b;
}

// --- 32. Do not fold through reassignment of the compared operand (`if`) ---

@method_id(132)
fun test32ReassignIf(x: int): int {
    val c = x != 0;
    x = 555;
    if (c) { return 1; }
    return 0;
}

// --- 33. Do not fold through reassignment of the compared operand (`?:`) ---

@method_id(133)
fun test33ReassignTernary(x: int, y: int, a: int, b: int): int {
    val c = x == 0;
    x = y;
    return c ? a : b;
}

// --- 34. Do not fold through reassignment of the compared operand (`assert`) ---

@method_id(134)
fun test34ReassignAssert(x: int): int {
    val c = x != 0;
    x = 555;
    try {
        assert(c, 777);
        return 1;
    } catch {
        return 0;
    }
}

// --- 35. Same guard when the reassignment reads a global ---

@method_id(135)
fun test35ReassignFromGlobal(x: int): int {
    gG = 777;
    val c = x != 0;
    x = gG;
    if (c) { return 1; }
    return 0;
}

// --- 36. Same guard when the reassignment is inside a nested block ---

@method_id(136)
fun test36ReassignInNestedIf(x: int, rewrite: int): int {
    val c = x != 0;
    if (rewrite != 0) {
        x = 0;
    }
    if (c) { return 1; }
    return 0;
}

// --- 37. PoC variant: ternary with `!= 0` and constant reassignment ---

@method_id(137)
fun test37ReassignTernaryNeqConst(x: int): int {
    val c = x != 0;
    x = 999;
    return c ? 100 : 200;
}

// --- 38. PoC variant: assert with `== 0` and constant reassignment ---

@method_id(138)
fun test38ReassignAssertEqConst(x: int): int {
    val c = x == 0;
    x = 1;
    assert(c, 42);
    return 7;
}

// --- 39. PoC variant: assert with `== 0` and runtime reassignment ---

@method_id(139)
fun test39ReassignAssertEqRuntime(x: int, y: int): int {
    val c = x == 0;
    x = y;
    assert(c, 999);
    return 7;
}

// --- 40. PoC variant: reassignment from a global without a preceding write ---

@method_id(140)
fun test40ReassignFromGlobalNoWrite(x: int): int {
    val c = x != 0;
    x = gG;
    if (c) { return 1; }
    return 0;
}

// --- 41. `match (x) { 0 => ... }` lowers to `x == 0` and must be optimized ---

@method_id(141)
fun test41MatchZero(x: int): int {
    return match (x) {
        0 => 10,
        else => 20,
    };
}

// --- 42. A `match (x) { 0 => ... }` reassignment must not affect an earlier cond-var ---

@method_id(142)
fun test42MatchZeroReassignBeforeUse(x: int): int {
    val wasZero = x == 0;
    x = match (x) {
        0 => 1,
        else => 0,
    };
    if (wasZero) { return 10 + x; }
    return 20 + x;
}

// --- 43. The cond-var (not the compared operand!) is conditionally rewritten inside a nested block ---

@method_id(143)
fun test43ReassignCondInNestedIf(x: int, flip: bool): int {
    var cond = x != 0;
    if (flip) {
        cond = false;
    }
    if (cond) {
        return 1;
    }
    return 2;
}

// --- 44. Same guard for ternaries (`__condsel` consumer) ---

@method_id(144)
fun test44ReassignCondInNestedTernary(x: int, flip: bool): int {
    var cond = x != 0;
    if (flip) {
        cond = false;
    }
    return cond ? 1 : 2;
}

// --- 45. Rename-chain version: the consumer reads `b`, not `cond`, but `cond` is rewritten ---

@method_id(145)
fun test45ReassignCondViaRename(x: int, flip: bool): int {
    var cond = x != 0;
    if (flip) {
        cond = false;
    }
    val b = cond;
    if (b) {
        return 1;
    }
    return 2;
}

// --- 46. Double-nested `== 0` in assert: `((x == 0) as int) == 0` ≡ `x != 0`.
//        Each `_==_(...,0)` fold toggles `__throw_ifnot` <-> `__throw_if` ---

@method_id(146)
fun test46AssertDoubleNestedEq(x: int): int {
    try {
        assert(((x == 0) as int) == 0, 146);
        return 1;
    } catch (err) {
        return err;
    }
}

// --- 47. Triple-nested `== 0` in assert: three flips cancel to one ⇒ THROWIF.
//        Semantically `assert(x == 0, N)`. ---

@method_id(147)
fun test47AssertTripleNestedEq(x: int): int {
    try {
        assert(((((x == 0) as int) == 0) as int) == 0, 147);
        return 1;
    } catch (err) {
        return err;
    }
}

// --- 48. Mixed `!=`-then-`==` in assert: outer `== 0` flips once,
//        inner `!= 0` does not flip. Net: one flip ⇒ THROWIF. ---

@method_id(148)
fun test48AssertMixedNeqEq(x: int): int {
    try {
        assert(((x != 0) as int) == 0, 148);
        return 1;
    } catch (err) {
        return err;
    }
}

// --- 49. Same double-nested `== 0` pattern in a ternary; sanity / regression guard. ---

@method_id(149)
fun test49TernaryDoubleNestedEq(x: int, a: int, b: int): int {
    return ((((x == 0) as int) == 0) ? a : b);
}

// --- 50. Self-referential `c = (c as int) != 0` ---

@method_id(150)
fun test50SelfRefIf(x: int): int {
    var c = x != 0;
    c = (c as int) != 0;
    if (c) {
        return 1;
    }
    return 0;
}

// --- 51. Ternary form of the same self-referential pattern. ---

@method_id(151)
fun test51SelfRefTernary(x: int): int {
    var c = x != 0;
    c = (c as int) != 0;
    return c ? 11 : 22;
}

// --- 52. Assert form of the same self-referential pattern. ---

@method_id(152)
fun test52SelfRefAssert(x: int): int {
    var c = x != 0;
    c = (c as int) != 0;
    try {
        assert(c, 555);
        return 1;
    } catch (err) {
        return err;
    }
}

// --- 53. A long constant-condition chain delays exposing the `cond` rewrite ---

fun test53_inner(x: int): int {
    var gate = 0;
    var cond = false;
    if (true)        { gate = 1; } else { gate = 100; }
    if (gate == 1)   { gate = 2; } else { gate = 100; }
    if (gate == 2)   { gate = 3; } else { gate = 100; }
    if (gate == 3)   { gate = 4; } else { gate = 100; }
    if (gate == 4)   { gate = 5; } else { gate = 100; }
    if (gate == 5)   { gate = 6; } else { gate = 100; }
    if (gate == 6)   { gate = 7; } else { gate = 100; }
    if (gate == 7)   { cond = x != 0; } else { cond = false; }
    assert (cond) throw 123;
    return x;
}

@method_id(153)
fun test53LastIterAssertRewrite(x: int): int {
    try {
        return test53_inner(x);
    } catch (err) {
        return err;
    }
}

// --- 54. `assert(!true, N)` is terminal just like `assert(false, N)`, `return` not needed ---

@method_id(154)
fun test54AssertNegatedConstFalse(): int {
    assert(!true, 5);
}

fun main(): int {
    return 0;
}

/**
    method_id | in              | out
@testcase | 0   |                 | 0
@testcase | 101 | 0               | 0 0
@testcase | 101 | 5               | 100 -1
@testcase | 102 | 0               | 13
@testcase | 102 | 7               | 7
@testcase | 103 | 5               | 1
@testcase | 103 | -1              | 2
@testcase | 104 | 0               | 0
@testcase | 104 | 9               | 1
@testcase | 105 | 0               | 0
@testcase | 105 | 9               | 1
@testcase | 106 | 5               | 0
@testcase | 106 | 6               | 1
@testcase | 107 | 7               | 1
@testcase | 107 | 0               | 0
@testcase | 108 |                 | 100
@testcase | 109 | 0               | 100
@testcase | 109 | 5               | 200
@testcase | 110 |                 | 15
@testcase | 111 |                 | 10
@testcase | 113 | 1 1             | 1
@testcase | 113 | 1 0             | 0
@testcase | 113 | 0 5             | 0
@testcase | 114 | -1 5            | 1
@testcase | 114 | 1 0             | 1
@testcase | 114 | 1 5             | 0
@testcase | 115 | 0               | 2
@testcase | 115 | 1               | 1
@testcase | 116 | 0 9 7           | 7
@testcase | 116 | 1 9 7           | 9
@testcase | 117 |                 | 5
@testcase | 118 | 0               | 0
@testcase | 118 | -1              | 1
@testcase | 119 | 0 9 7           | 7
@testcase | 119 | 1 9 7           | 9
@testcase | 120 | 0 9 7           | 9
@testcase | 120 | 1 9 7           | 7
@testcase | 121 | 0 9 7           | 7
@testcase | 121 | 5 9 7           | 9
@testcase | 122 | 0 9 7           | 9
@testcase | 122 | 5 9 7           | 7
@testcase | 123 | 0 0 1 2 3       | 3
@testcase | 123 | 1 0 1 2 3       | 1
@testcase | 123 | 1 5 1 2 3       | 2
@testcase | 124 | 0 9 7           | 7 0
@testcase | 124 | 5 9 7           | 9 -1
@testcase | 125 | 5               | 6
@testcase | 125 | -3              | -2
@testcase | 126 | 0               | 100
@testcase | 127 | 0               | 0
@testcase | 128 | 0               | 1
@testcase | 128 | 5               | 6
@testcase | 129 | 7               | 1
@testcase | 129 | -3              | 1
@testcase | 130 |                 | 42
@testcase | 131 | 0 9 7           | 9
@testcase | 131 | 5 9 7           | 7
@testcase | 132 | 0               | 0
@testcase | 132 | 5               | 1
@testcase | 133 | 0 777 10 20     | 10
@testcase | 133 | 5 0 10 20       | 20
@testcase | 134 | 0               | 0
@testcase | 134 | 5               | 1
@testcase | 135 | 0               | 0
@testcase | 135 | 5               | 1
@testcase | 136 | 5 1             | 1
@testcase | 136 | 0 0             | 0
@testcase | 137 | 0               | 200
@testcase | 137 | 5               | 100
@testcase | 138 | 0               | 7
@testcase | 139 | 0 5             | 7
@testcase | 140 | 0               | 0
@testcase | 140 | 5               | 1
@testcase | 141 | 0               | 10
@testcase | 141 | 5               | 20
@testcase | 142 | 0               | 11
@testcase | 142 | 5               | 20
@testcase | 143 | 5 -1            | 2
@testcase | 143 | 5 0             | 1
@testcase | 143 | 0 -1            | 2
@testcase | 143 | 0 0             | 2
@testcase | 144 | 5 -1            | 2
@testcase | 144 | 5 0             | 1
@testcase | 144 | 0 -1            | 2
@testcase | 144 | 0 0             | 2
@testcase | 145 | 5 -1            | 2
@testcase | 145 | 5 0             | 1
@testcase | 145 | 0 -1            | 2
@testcase | 145 | 0 0             | 2
@testcase | 146 | 0               | 146
@testcase | 146 | 5               | 1
@testcase | 146 | -3              | 1
@testcase | 147 | 0               | 1
@testcase | 147 | 5               | 147
@testcase | 147 | -3              | 147
@testcase | 148 | 0               | 1
@testcase | 148 | 5               | 148
@testcase | 148 | -3              | 148
@testcase | 149 | 0 11 22         | 22
@testcase | 149 | 5 11 22         | 11
@testcase | 149 | -3 11 22        | 11
@testcase | 150 | 0               | 0
@testcase | 150 | 5               | 1
@testcase | 150 | -3              | 1
@testcase | 151 | 0               | 22
@testcase | 151 | 5               | 11
@testcase | 151 | -3              | 11
@testcase | 152 | 0               | 555
@testcase | 152 | 5               | 1
@testcase | 152 | -3              | 1
@testcase | 153 | 0               | 123
@testcase | 153 | 5               | 5
@testcase | 153 | -3              | -3

@fif_codegen
"""
test1MultiUse() PROC:<{
    DUP
    0 NEQINT
    ...
    IF:<{
"""

@fif_codegen
"""
test2InlineChain() PROC:<{
    IFJMP:<{
      7 PUSHINT
"""

@fif_codegen
"""
test3Impure() PROC:<{
    INC
    $gG SETGLOB
    $gG GETGLOB
    IFJMP:<{
"""


@fif_codegen
"""
test4ZeroLeft() PROC:<{
    IFJMP:<{
"""

@fif_codegen
"""
test5ZeroLeftInlined() PROC:<{
    IFJMP:<{
"""

@fif_codegen
"""
test6NonZero() PROC:<{
    5 NEQINT
    IFJMP:<{
"""

@fif_codegen
"""
test7NonZeroEq() PROC:<{
    7 EQINT
    IFJMP:<{
"""

@fif_codegen
"""
test9IsEmpty() PROC:<{
    IFNOTJMP:<{
"""

@fif_codegen
"""
test10WhileInline() PROC:<{
    5 PUSHINT
    0 PUSHINT
    WHILE:<{
        OVER
    }>DO<{
"""

@fif_codegen
"""
test11DoWhile() PROC:<{
    4 PUSHINT
    0 PUSHINT
    UNTIL:<{
"""

@fif_codegen
"""
test12MatchChain() PROC:<{
    ...
    DUP
    0 EQINT
    IFJMP:<{
"""

@fif_codegen
"""
test15TryCatch() PROC:<{
    c2 SAVE
    SAMEALTSAVE
    CONT:<{
"""

@fif_codegen
"""
    CONT:<{
      IFJMP:<{
        1 PUSHINT
        RETALT
      }>
      2 PUSHINT
      RETALT
    }>
"""

@fif_codegen
"""
test16Ternary() PROC:<{
    CONDSEL
"""

@fif_codegen
"""
test17Nested() PROC:<{
    3 PUSHINT
    0 PUSHINT
    DUP
    WHILE:<{
        s2 PUSH
    }>DO<{
        OVER
        IF:<{
"""

@fif_codegen
"""
test18BoolNeqFalse() PROC:<{
    IFJMP:<{
"""

@fif_codegen
"""
test19TernaryStrip() PROC:<{
    CONDSEL
"""

@fif_codegen
"""
test20TernaryFlip() PROC:<{
    SWAP
    CONDSEL
"""

@fif_codegen
"""
test21TernaryInlNeq() PROC:<{
    CONDSEL
"""

@fif_codegen
"""
test22TernaryInlEq() PROC:<{
    SWAP
    CONDSEL
"""

@fif_codegen
"""
test23TernaryNested() PROC:<{
    s0 s4 XCHG
    IF:<{
      s3 POP
      s1 s2 XCHG
      CONDSEL
    }>ELSE<{
      3 BLKDROP
"""

@fif_codegen
"""
test24TernaryMultiUse() PROC:<{
    s2 PUSH
    0 NEQINT
    3 -ROLL
    CONDSEL
    SWAP
"""

@fif_codegen
"""
test25AssertStrip() PROC:<{
    DUP
    555 THROWIFNOT
    INC
"""

@fif_codegen
"""
test26AssertFlip() PROC:<{
    DUP
    666 THROWIF
    100 ADDCONST
"""

@fif_codegen
"""
test27AssertInlEq() PROC:<{
    DUP
    888 THROWIF
"""

@fif_codegen
"""
test28AssertImpure() PROC:<{
    INC
    $gG SETGLOB
    $gG GETGLOB
    999 THROWIFNOT
    $gG GETGLOB
"""

@fif_codegen
"""
test29AssertAndTernary() PROC:<{
    DUP
    100 THROWIFNOT
    1 PUSHINT
    2 PUSHINT
    CONDSEL
"""

@fif_codegen
"""
test30AssertConstTrue() PROC:<{
    42 PUSHINT
"""

@fif_codegen
"""
test31TernaryZeroOnLeft() PROC:<{
    SWAP
    CONDSEL
"""

@fif_codegen
"""
test41MatchZero() PROC:<{
    IF:<{
      20 PUSHINT
    }>ELSE<{
      10 PUSHINT
"""

@fif_codegen
"""
test42MatchZeroReassignBeforeUse() PROC:<{
    DUP
    0 EQINT
    SWAP
    IF:<{
      0 PUSHINT
    }>ELSE<{
      1 PUSHINT
    }>
    SWAP
    IFJMP:<{
      10 ADDCONST
"""

@fif_codegen
"""
test45ReassignCondViaRename() PROC:<{
    SWAP
    0 NEQINT
    SWAP
    IF:<{
      DROP
      FALSE
    }>
    IFJMP:<{
      1 PUSHINT
    }>
    2 PUSHINT
"""

@fif_codegen
"""
    CONT:<{
      146 THROWIFNOT
"""

@fif_codegen
"""
    CONT:<{
      147 THROWIF
"""

@fif_codegen
"""
    CONT:<{
      148 THROWIF
"""

@fif_codegen
"""
test49TernaryDoubleNestedEq() PROC:<{
    CONDSEL
"""

@fif_codegen
"""
test50SelfRefIf() PROC:<{
    0 NEQINT
    0 NEQINT
    IFJMP:<{
      1 PUSHINT
    }>
    0 PUSHINT
"""

@fif_codegen
"""
test54AssertNegatedConstFalse() PROC:<{
    5 THROW
}>
"""
*/
