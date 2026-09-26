fun foo1(x: int): int {
    __expect_inline(true);
    if (x == 1) {
        return 1;
    }
    return 2;
}

@inline
fun foo2(x: int): int {
    __expect_inline(true);
    if (x == 1) {
        return 11;
    }
    return 22;
}

@inline_ref
fun foo3(x: int): int {
    if (x == 1) {
        return 111;
    }
    return 222;
}

@inline_ref
fun foo_light_but_ref() {
    __expect_inline(false);
    return 0;
}

@noinline
fun foo_light_but_no() {
    __expect_inline(false);
    return 0;
}

@method_id(101)
fun test1(x: int): (int, int, int) {
    __expect_inline(false);     // @method_id
    return (foo1(x) + 1, foo2(x) + 1, foo3(x) + 1 + foo_light_but_ref() + foo_light_but_no());
}

global g: int;

@inline
fun foo_repeat() {
    __expect_inline(true);
    g = 1;
    repeat (5) {
        g *= 2;
    }
}

@inline
fun foo_until(): int {
    __expect_inline(true);
    g = 1;
    var i: int = 0;
    do {
        g *= 2;
        i += 1;
    } while (i < 8);
    return i;
}

@inline
fun foo_while(): int {
    __expect_inline(true);
    g = 1;
    var i: int = 0;
    while (i < 10) {
        g *= 2;
        i += 1;
    }
    return i;
}

@method_id(102)
fun test2() {
    foo_repeat();
    var x: int = g;
    foo_until();
    var y: int = g;
    foo_while();
    var z: int = g;
    return (x, y, z);
}

@inline
fun foo_big(x: int): int {
    __expect_inline(true);
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

fun foo_big_not_annotated_called_once(x: int): int {
    __expect_inline(true);     // called only once, inline
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

fun foo_big_not_annotated_called_twice(x: int): int {
    __expect_inline(false);     // too big for in-place auto-inlining when called multiple times
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

@method_id(103)
fun test3(x: int): int {
    return foo_big(x) * 10 + 5;
}

@method_id(104)
fun test4(x: int) {
    return (foo_big_not_annotated_called_once(x) * 10, foo_big_not_annotated_called_twice(x) * 10, foo_big_not_annotated_called_twice(x));
}

struct Storage {
    owner: address;
    lastCall: int64;
    extra: Cell<(int8, uint256)>;
}

fun Storage.load() {
    __expect_inline(true);
    return Storage.fromCell(contract.getData());
}

fun Storage.save(self) {
    __expect_inline(true);
    contract.setData(self.toCell());
}

@method_id(105)
fun test5() {
    var st = Storage.load();
    st.lastCall = 0;
    st.save();
}

fun factorial(x: int): int {
    __expect_inline(false);   // due to recursion
    if (x <= 1) {
        return 1;
    }
    return x * factorial(x - 1);
}

fun recurse1(x: int): int { __expect_inline(false); return recurse2(x + 1); }
fun recurse2(x: int): int { __expect_inline(false); return recurse3(x + 1); }
fun recurse3(x: int): int { __expect_inline(false); return recurse1(x + 1); }

global g10: int;

fun startCellWith(x: int) {
    __expect_inline(true);
    return beginCell().storeUint(x, 32);
}

fun startCellWithG10() {
    __expect_inline(true);
    return beginCell().storeUint(g10, 32);
}

fun builder.myBitsCount(self): int {
    __expect_inline(true);
    return self.bitsCount();
}

fun builder.store32(mutate self, v: int32): self {
    __expect_inline(true);
    return self.storeUint(v, 32);
}

fun sum(a: int, b: int) {
    __expect_inline(true);     // even though it's never called
    return a + b;
}

fun eq<T>(v: T): T {
    __expect_inline(true);
    return v;
}

fun get20() { return 20; }

struct Point {
    x: int;
    y: int;
}

fun Point.getX(self) {
    __expect_inline(true);
    return self.x;
}

fun Point.create(x: int, y: int): Point {
    __expect_inline(true);
    return {x, y}
}

fun increment(mutate x: int) {
    __expect_inline(true);
    x += 1;
}

@method_id(106)
fun test6() {
    var p = Point.create(10, 20);
    increment(mutate p.x);
    p.y = p.getX();
    return p;
}

@method_id(107)
fun test7() {
    g10 = 10;
    return startCellWithG10().store32(20).myBitsCount();
}

@method_id(108)
fun test8() {
    var x = 0;
    return (startCellWith(x = 5).store32(x += 20).store32(x), x);
}

@method_id(109)
fun test9(x: int) {
    var p1: Point = {x, y: 0};
    var p2: Point = {x: 0, y: p1.getX()};
    var t: tuple = createEmptyTuple();
    t.pushPoint(p1);
    t.pushPoint(p2);
    return (t.popPoint(), t.popPoint(), t);
}

fun tuple.pushPoint(mutate self, p: Point) {
    __expect_inline(true);
    var x = p.x;
    self.push(x);
    self.push(p.y);
}

fun tuple.popPoint(mutate self): Point {
    __expect_inline(true);
    var y = self.pop() as int;
    return {x: self.pop() as int, y};
}

@method_id(110)
fun test10() {
    var (a, b, c) = usedIn10ButDeclaredBelow(5);
    return (a, b, c.bitsCount());
}

global g11_1: int;
global g11_2: int;

fun mutateGlobals(mutate v: int, inc11_2: int) {
    __expect_inline(true);
    v += 1;
    g11_2 += inc11_2;
}

@method_id(111)
fun test11() {
    g11_1 = 0;
    g11_2 = 0;
    mutateGlobals(mutate g11_1, 5);         // 1 5
    mutateGlobals(mutate g11_1, g11_2);     // 2 10
    mutateGlobals(mutate g11_2, g11_1);     // 2 11
    return (g11_1, g11_2);
}

fun nested1(x: int) {
    var y = x + 2;
    x += 7;
    return y;
}

fun nested2(x: int) {
    var y = x + 2;
    var z = nested1(y);
    y += z;
    return y + z;
}

@method_id(112)
fun test12(x: int) {
    var r1 = nested2(10);
    var r2 = nested2(x);
    return (r1, r2);
}

fun anotherMath(arg: int) {
    __expect_inline(true);
    var cp_x = arg;
    increment(mutate arg);
    return cp_x + arg;
}

@method_id(113)
fun test13(p: Point) {
    anotherMath(p.x);
    var r = anotherMath(p.y);
    return (p, r);
}

fun evalPoint(p: Point) {
    __expect_inline(true);
    increment(mutate p.x);
    p.y *= 2;
    return max(p.x, p.y);
}

@method_id(114)
fun test14(p2: Point) {
    var p1: Point = { x: 0, y: 0 };
    var r1 = evalPoint(p1);
    return (r1, p1, evalPoint(p2), p2);
}

global t: tuple;

fun logT(a: int) {
    __expect_inline(true);
    t.push(a);
}

fun demoInlined1(x: int) {
    if (x == -1) {
        logT(10);
    } else if (x == 0) {
        logT(20);
    }
}

fun demoInlined2(x: int) {
    __expect_inline(true);
    match (x) {
        -1 => logT(10),
        else => logT(20),
    }
    return;
    throw 123;
}

fun wrapperNotInlined(x: int?) {
    if (x != null) {
        demoInlined1(x);
        return;
    }
    logT(100);
}

fun someUsages(x: int?) {
    __expect_inline(true);
    wrapperNotInlined(x);
    demoInlined2(x!);
    return;
}

@method_id(115)
fun test15(x: int?) {
    t = createEmptyTuple();
    someUsages(x);
    return t;
}

struct (0x01) A16 { a: int8; }
struct (0x02) B16 {}
struct (0x03) C16 {}
type U16 = A16 | B16 | C16;

fun check16(msg: U16) {
    match (msg) {
        A16 => { assert(msg.a > 0, 100); }
        B16 => {}
        C16 => {}
    }
}

fun wrap16(msg: U16, check: bool) {
    if (check) {
        check16(msg);
        return;
    }
    throw 123;
}

@method_id(116)
fun test16() {
    wrap16(U16.fromSlice(stringHexToSlice("0170")), true);
    return -1;
}

fun guardChain17(x: int): int {
    __expect_inline(true);
    if (x < 0) {
        return -1;
    }
    if (x == 0) {
        return 0;
    }
    return x * 10;
}

fun ifnotGuard17(x: int): int {
    __expect_inline(true);
    var isPositive = x > 0;
    if (!isPositive) {
        return 71;
    }
    return 72;
}

fun tree17(x: int): int {
    __expect_inline(true);
    if (x < 0) {
        if (x == -1) {
            return 11;
        }
        return 12;
    } else {
        if (x == 0) {
            return 20;
        }
        return 30;
    }
}

@method_id(117)
fun test17(x: int) {
    return (guardChain17(x), ifnotGuard17(x), tree17(x));
}

fun maybeInc18(mutate x: int, shouldInc: int) {
    __expect_inline(true);
    if (shouldInc == 0) {
        return;
    }
    x += 10;
    return;
}

@method_id(118)
fun test18(x: int, shouldInc: int): int {
    maybeInc18(mutate x, shouldInc);
    return x;
}

fun genericGuard19<T>(value: T, fallback: T, useFallback: bool): T {
    __expect_inline(true);
    if (useFallback) {
        return fallback;
    }
    return value;
}

@method_id(119)
fun test19(x: int): int {
    return genericGuard19(x, 100, x == 0);
}

fun matchAllTerminal20(x: int): int {
    __expect_inline(true);
    match (x) {
        0 => { return 5; }
        1 => {
            var y = x + 6;
            return y;
        }
        else => throw 123
    }
}

fun matchGuard20(x: int): int {
    __expect_inline(true);
    match (x) {
        0 => { return 50; }
        1 => { return 60; }
    }
    return x + 100;
}

fun matchOneFallthrough20(x: int): int {
    __expect_inline(true);
    var y = 10;
    match (x) {
        0 => {
            y += 1;
        }
        1 => { return 101; }
        else => { return 102; }
    }
    return y;
}

fun matchType20(x: int | slice): int {
    __expect_inline(true);
    match (x) {
        int => { return x + 1; }
        slice => { return 200; }
    }
}

fun matchPartialArm20(x: int): int {
    __expect_inline(true);
    match (x) {
        0 => {
            if (x == 0) {
                return 70;
            }
        }
        else => { return 80; }
    }
    return 90;
}

@method_id(120)
fun test20(x: int): (int, int, int, int, int) {
    return (matchAllTerminal20(x), matchGuard20(x), matchOneFallthrough20(x), matchType20(x as int | slice), matchPartialArm20(x));
}

fun returnThenThrow21(x: int): int {
    __expect_inline(true);
    if (x > 0) {
        return 1;
    }
    throw 123;
}

@noinline
fun alwaysThrow21(): never {
    throw 777;
}

fun returnThenNeverCall21(x: int): int {
    __expect_inline(true);
    if (x > 0) {
        return 2;
    }
    alwaysThrow21();
}

@method_id(121)
fun test21(x: int): (int, int) {
    return (returnThenThrow21(x), returnThenNeverCall21(x));
}

global g22: int;

fun voidWithMidIf22(x: int) {
    __expect_inline(true);
    if (x > 0) {
        g22 = 1;
    }
    g22 = g22 + 100;
    return;
    return;
}

@method_id(122)
fun test22(x: int) {
    voidWithMidIf22(x);
    return g22;
}

fun tree23(x: int, mutate z: int): int {
    __expect_inline(true);
    z += x;
    if (x < 0) {
        if (x == -1) {
            return 11;
        }
        return 12;
    } else {
        if (x == 0) {
            return 20;
        }
        return 30;
    }
}

@method_id(123)
fun test23(x: int) {
    var z = 0;
    return (tree23(x, mutate z), z);
}

global g24: int;

fun spliceIntoElse(x: int): int {
    __expect_inline(true);
    if (x > 0) { return 1; }
    else { var a = 5; g24 = a * x; }
    var y = x * 2;
    return y;
}

@method_id(124)
fun test24(x: int) {
    return (spliceIntoElse(x), spliceIntoElse(-x), g24);
}

global g25: int;

// a loop and try/catch are okay inside an inlined function, as long as `return` is not inside them
fun guardThenLoopAndTry25(x: int): int {
    __expect_inline(true);
    if (x < 0) {
        return -1;
    }
    var s = 0;
    repeat (x) {
        s += 2;
    }
    try {
        g25 = 100 / (x - 5);
    } catch {
        g25 = 777;
    }
    return s + g25;
}

@method_id(125)
fun test25(x: int) { return guardThenLoopAndTry25(x); }

// an inlined function with early returns calling another inlined function with early returns
fun inner26(x: int): int {
    __expect_inline(true);
    if (x == 0) {
        return 100;
    }
    return x * 2;
}

fun outer26(x: int): int {
    __expect_inline(true);
    if (x < 0) {
        return -5;
    }
    return inner26(x) + 1;
}

@method_id(126)
fun test26(x: int) { return outer26(x); }

// early returns in a method with `self` and in a method returning `self`
struct Point27 { x: int; y: int; }

fun Point27.pick(self, useX: bool): int {
    __expect_inline(true);
    if (useX) {
        return self.x;
    }
    return self.y;
}

fun Point27.bump(mutate self, by: int): self {
    __expect_inline(true);
    if (by == 0) {
        return self;
        throw 123;      // unreachable, not lowered
    }
    self.x += by;
    return self;
    assert (by == 0) throw 123;    // unreachable, not lowered
}

@method_id(127)
fun test27(x: int, by: int) {
    var p = Point27{ x: x, y: x * 10 };
    return (p.pick(true), p.pick(false), p.bump(by).x);
}

// an inlined function with early returns, called from a loop / try-catch / match arm of a caller
@inline
fun guard28(x: int): int {
    if (x < 0) {
        return -1;
        throw 123;
    }
    return x * 2;
}

@method_id(128)
fun test28(x: int): (int, int, int) {
    var inLoop = 0;
    repeat (3) {
        inLoop += guard28(x);
        x -= 1;
    }
    var inTry = 0;
    try {
        inTry = guard28(x) + guard28(-x);
    } catch {
        inTry = 999;
    }
    var inMatch = match (x) {
        0 => guard28(-1),
        else => guard28(x),
    };
    return (inLoop, inTry, inMatch);
}

// a chain of nested tails: both `else` and `match` have tails
global g29: int;

fun nestedSplice29(x: int): (int, int, int) {
    __expect_inline(true);
    if (x > 100) {
        return (1, 1, 1);
    } else {
        match (x) {
            50 => { return (2, 2, 2); /* unreachable */ g29 = 123; }
            60 => { return (3, 3, 3); /* unreachable */ g29 = 124; }
        }
        g29 = x;
    }
    return (x, x + 1, x + 2);
    g29 = 555;
}

@method_id(129)
fun test29(x: int) { return nestedSplice29(x); }

// `match` whose arms are expressions, not blocks, and one of them does `return`
fun matchExprArms30(x: int) {
    __expect_inline(true);
    match (x) {
        0 => return 5,
        else => g29 = 2,
    };
    return g29;
}

@method_id(130)
fun test30(x: int) { return (matchExprArms30(x), g29); }

fun matchSomeArmsThrow31(x: int): int {
    __expect_inline(true);
    match (x) {
        -1 => {}
        0 => {}
        1 => {}
        else => throw 123
    }
    return x + 100;
}

@method_id(131)
fun test31(x: int): int {
    return matchSomeArmsThrow31(x);
}

fun nestedIfThrow32(x: int): int {
    __expect_inline(true);
    if (x < 0) {
        if (x < 100) {
            throw 123;
        }
        x += 10;
    }
    return x;
}

@method_id(132)
fun test32(x: int): int {
    return nestedIfThrow32(x);
}

struct TinyLazy { x: int8 }

fun TinyLazy.getOrSkip(self, flag: int): int {
    __expect_inline(true);
    var dummy = lazy TinyLazy.fromSlice(stringHexToSlice("00"));
    if (flag != 0) {
        return dummy.x;
    }
    return self.x;
}

@method_id(133)
fun testLazyEarlyReturn(flag: int): int {
    var p = lazy TinyLazy.fromSlice(stringHexToSlice("09"));
    return p.getOrSkip(flag);
}

@inline
fun ret_then_dead_throw(flag: int, b: int): int {
    if (flag != 0) {
        if (b != 0) {
            return 1;
        }
        throw 2;
    }
    return 9;
}

@method_id(134)
fun testInlineRetThrow(flag: int, b: int): int {
    return ret_then_dead_throw(flag, b);
}

@inline
fun match_throw_then_ret(flag: int, x: int): int {
    if (flag != 0) {
        match (x) {
            0 => { throw 1; }
        }
    } else {
        return 2;
    }
    return 9;
}

@method_id(135)
fun testMatchThrowThenRet(flag: int, x: int): int {
    return match_throw_then_ret(flag, x);
}

struct (0x00) LazyRetA { a: int8 }
struct (0x10) LazyRetB { b: int16 }
type LazyRetU = LazyRetA | LazyRetB

fun has_lazy_union_both_return(s: slice): int {
    __expect_inline(true);
    var x = lazy LazyRetU.fromSlice(s);
    match (x) {
        LazyRetA => { return x.a; }
        LazyRetB => { return x.b; }
    }
}

@method_id(136)
fun testLazyUnionBothReturn(flag: int) {
    val s = flag == 0 ? "0008".hexToSlice() : "10FFFF".hexToSlice();
    return has_lazy_union_both_return(s);
}

fun has_lazy_union_single_return(s: slice): int {
    __expect_inline(true);
    var x = lazy LazyRetU.fromSlice(s);
    match (x) {
        LazyRetA => { return x.a; }
        LazyRetB => {  }
    }
    return 123;
}

@method_id(137)
fun testLazyUnionSingleReturn(flag: int) {
    val s = flag == 0 ? "0008".hexToSlice() : "10FFFF".hexToSlice();
    return has_lazy_union_single_return(s);
}

fun has_lazy_union_multi_tail(s: slice, flag: int, mutate out: int): int {
    __expect_inline(true);
    var x = lazy LazyRetU.fromSlice(s);
    __expect_lazy("[x] lazy match");
    match (x) {
        LazyRetA => {
            if (flag > 100) {
                out -= 90;
                return 100;
            }
            out += x.a;
        }
        LazyRetB => {
            if (flag < -100) {
                return -100;
            }
            throw 100;
        }
    }
    out += 123;
    return 123;
}

@method_id(138)
fun testLazyUnionMultiTail(flag: int) {
    var r = 0;
    val s = flag > 0 ? "0008".hexToSlice() : "10FFFF".hexToSlice();
    return (has_lazy_union_multi_tail(s, flag, mutate r), r);
}

// An all-terminal inner `if` after an early return must not be counted as fallthrough.
@inline
fun return_then_terminal_if_in_both_arms(useLeft: bool, doReturn: bool, chooseThrow: bool): int {
    __expect_inline(true);
    if (useLeft) {
        if (doReturn) {
            return 11;
        }
        if (chooseThrow) {
            throw 201;
        } else {
            throw 202;
        }
    } else {
        if (doReturn) {
            return 21;
        }
        if (chooseThrow) {
            throw 203;
        } else {
            throw 204;
        }
    }
}

@method_id(139)
fun testReturnThenTerminalIfInBothArms(useLeft: bool, doReturn: bool, chooseThrow: bool): int {
    try {
        return return_then_terminal_if_in_both_arms(useLeft, doReturn, chooseThrow);
    } catch (excNo) {
        return excNo;
    }
}

// An exhaustive all-terminal inner `match` after an early return must not be counted as fallthrough.
@inline
fun return_then_terminal_match_in_both_arms(outerKind: int, doReturn: bool, throwKind: int): int {
    __expect_inline(true);
    match (outerKind) {
        0 => {
            if (doReturn) {
                return 31;
            }
            match (throwKind) {
                0 => { throw 211; }
                else => { throw 212; }
            }
        }
        else => {
            if (doReturn) {
                return 41;
            }
            match (throwKind) {
                0 => { throw 213; }
                else => { throw 214; }
            }
        }
    }
}

@method_id(140)
fun testReturnThenTerminalMatchInBothArms(outerKind: int, doReturn: bool, throwKind: int): int {
    try {
        return return_then_terminal_match_in_both_arms(outerKind, doReturn, throwKind);
    } catch (excNo) {
        return excNo;
    }
}


fun main() {
    // regardless of number of calls, it will be inlined, since it's lightweight in AST terms
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    usedIn10ButDeclaredBelow(10);
}

fun usedIn10ButDeclaredBelow(x: int) {
    __expect_inline(true);
    var y = eq(x);
    return (x, y, eq(startCellWith(x)));
}



/**
      method_id | in     | out
@testcase | 101 | 1      | 2 12 112
@testcase | 101 | 2      | 3 23 223
@testcase | 102 |        | 32 256 1024
@testcase | 103 | 9      | 911111111111111111111111111111111111111111111111115
@testcase | 104 | 9      | 911111111111111111111111111110 911111111111111111111111111110 91111111111111111111111111111
@testcase | 106 |        | 11 11
@testcase | 107 |        | 64
@testcase | 108 |        | BC{0018000000050000001900000019} 25
@testcase | 109 | 10     | 0 10 10 0 []
@testcase | 110 |        | 5 5 32
@testcase | 111 |        | 2 11
@testcase | 112 | 10     | 40 40
@testcase | 113 | 10 20  | 10 20 41
@testcase | 114 | 10 20  | 1 0 0 40 10 20
@testcase | 115 | -1     | [ 10 10 ]
@testcase | 115 | 1      | [ 20 ]
@testcase | 116 |        | -1
@testcase | 117 | -1     | -1 71 11
@testcase | 117 | 0      | 0 71 20
@testcase | 117 | 5      | 50 72 30
@testcase | 118 | 5 0    | 5
@testcase | 118 | 5 1    | 15
@testcase | 119 | 0      | 100
@testcase | 119 | 7      | 7
@testcase | 120 | 0      | 5 50 11 1 70
@testcase | 120 | 1      | 7 60 101 2 80
@testcase | 121 | 5      | 1 2
@testcase | 122 | 10     | 101
@testcase | 123 | -1     | 11 -1
@testcase | 124 | 8      | 1 -16 -40
@testcase | 125 | -1     | -1
@testcase | 125 | 3      | -44
@testcase | 125 | 5      | 787
@testcase | 126 | -1     | -5
@testcase | 126 | 0      | 101
@testcase | 126 | 3      | 7
@testcase | 127 | 7 0    | 7 70 7
@testcase | 127 | 7 3    | 7 70 10
@testcase | 128 | 5      | 24 3 4
@testcase | 128 | 1      | 1 3 -1
@testcase | 129 | 200    | 1 1 1
@testcase | 129 | 50     | 2 2 2
@testcase | 129 | 7      | 7 8 9
@testcase | 130 | 0      | 5 (null)
@testcase | 130 | 9      | 2 2
@testcase | 131 | -1     | 99
@testcase | 131 | 0      | 100
@testcase | 131 | 1      | 101
@testcase | 132 | 0      | 0
@testcase | 132 | 5      | 5
@testcase | 133 | 0      | 9
@testcase | 133 | 1      | 0
@testcase | 134 | 1 1    | 1
@testcase | 134 | 0 0    | 9
@testcase | 135 | 1 1    | 9
@testcase | 135 | 0 0    | 2
@testcase | 136 | 0      | 8
@testcase | 136 | 5      | -1
@testcase | 137 | 0      | 8
@testcase | 137 | 5      | 123
@testcase | 138 | 10     | 123 131
@testcase | 138 | 500    | 100 -90
@testcase | 138 | -500   | -100 0
@testcase | 139 | 1 1 1  | 11
@testcase | 139 | 0 1 0  | 21
@testcase | 139 | 1 0 1  | 201
@testcase | 139 | 0 0 0  | 204
@testcase | 140 | 0 1 0  | 31
@testcase | 140 | 1 1 1  | 41
@testcase | 140 | 0 0 0  | 211
@testcase | 140 | 1 0 1  | 214

@fif_codegen_avoid Storage.load
@fif_codegen_avoid Storage.save
@fif_codegen
"""
  test5() PROC:<{               //
    c4 PUSH                     //  '4
    CTOS                        //  s
    LDSTDADDR                   //  '8 s
"""

@fif_codegen
"""
  test6() PROC:<{
    11 PUSHINT
    DUP
  }>
"""

@fif_codegen
"""
  test8() PROC:<{               //
    NEWC                        //  '5
    x{00000005} STSLICECONST    //  '3
    25 PUSHINT                  //  '3 '9
    SWAP                        //  x self
    x{0000001900000019} STSLICECONST  //  x '3
    SWAP                        //  '3 x
  }>
"""

@fif_codegen
"""
  test9() PROC:<{               //  x
    0 PUSHINT                   //  p1.x p1.y=0
    s0 s1 PUSH2                 //  p1.x p1.y=0 p2.x=0 p2.y
    0 TUPLE                     //  x p1.y=0 p2.x=0 p2.y self
    s0 s4 XCHG2                 //  p2.y p1.y=0 p2.x=0 self x
    TPUSH                       //  p2.y p1.y=0 p2.x=0 self
    ROT                         //  p2.y p2.x=0 self p1.y=0
    TPUSH                       //  p2.y x=0 self
    SWAP                        //  p2.y self x=0
    TPUSH                       //  p2.y self
    SWAP                        //  self p2.y
    TPUSH                       //  self
    TPOP                        //  self y
    SWAP                        //  y self
    TPOP                        //  '26 self '25
    SWAP                        //  '26 '25 self
    TPOP                        //  '26 '25 self y
    SWAP                        //  '26 '25 y self
    TPOP                        //  '26 '25 '36 t '35
    s3 s4 XCHG                  //  '25 '26 '36 t '35
    -ROT                        //  '25 '26 '35 '36 t
  }>
"""

@fif_codegen
"""
  test10() PROC:<{
    5 PUSHINT                   //  '0=5
    DUP                         //  '0=5 y=5
    NEWC                        //  '0=5 y=5 '10
    x{00000005} STSLICECONST    //  a=5 b=5 c
    BBITS                       //  a=5 b=5 '18
  }>
"""

@fif_codegen
"""
  test12() PROC:<{              //  x
    40 PUSHINT                  //  x r1
    SWAP                        //  r1 x
    2 ADDCONST                  //  r1 y
    ...
"""

@fif_codegen
"""
  test17() PROC:<{              //  x
    DUP                         //  x x
    0 LESSINT                   //  x '4
    IF:<{                       //  x
      -1 PUSHINT                //  x '1=-1
    }>ELSE<{                    //  x
"""

@fif_codegen
"""
  test22() PROC:<{              //  x
    0 GTINT                     //  '3
    IF:<{                       // 
      1 PUSHINT                 //  g_g22=1
      $g22 SETGLOB              // 
    }>                          // 
    $g22 GETGLOB                //  g_g22
    100 ADDCONST                //  g_g22
    $g22 SETGLOB                // 
    $g22 GETGLOB                //  g_g22
  }>
"""
*/
