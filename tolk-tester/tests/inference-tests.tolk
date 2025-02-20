// the goal of this file is not only to @testcase results —
// but to check that this file compiles

fun eq<X>(value: X): X { return value; }

fun test1(x: int, y: int) {
    __expect_type(0, "int");
    __expect_type("0"c, "int");
    __expect_type(x, "int");
    __expect_type(x + y, "int");
    __expect_type(x * y, "int");
    __expect_type(x & y, "int");
    __expect_type(x << y, "int");
    __expect_type((((x))), "int");
    __expect_type(x = x, "int");
    __expect_type(x += x, "int");
    __expect_type(x &= x, "int");
    __expect_type(random() ? x : y, "int");
    __expect_type(eq(x), "int");
    __expect_type(eq<int>(x), "int");
    __expect_type(eq<int>(null), "int");
    __expect_type(x as int, "int");
    __expect_type(+x, "int");
    __expect_type(~x, "int");
    {
        var x: slice = beginCell().endCell().beginParse();
        __expect_type(x, "slice");
        __expect_type(beginCell(), "builder");
        __expect_type(beginCell().endCell(), "cell");
    }
}

fun test2(x: int, y: bool) {
    __expect_type(!x, "bool");
    __expect_type(x != x, "bool");
    __expect_type(x <= x, "bool");
    __expect_type(x <=> x, "bool");
    __expect_type(x <=> x, "bool");
    __expect_type(!random(), "bool");
    __expect_type(!!(x != null), "bool");
    __expect_type(x ? x != null : null == x, "bool");
    __expect_type(y & true, "bool");
    __expect_type(y ^= false, "bool");
    __expect_type(x && y, "bool");
    __expect_type(true && false && true, "bool");
    __expect_type(x || x, "bool");
    __expect_type(x || !x || (true & false), "bool");
}

fun test3() {
    __expect_type(true as int, "int");
    __expect_type(!random() as int, "int");
}

fun test4(x: int) {
    __expect_type((), "()");
    __expect_type((x, x), "(int, int)");
    __expect_type((x, (x, x), x), "(int, (int, int), int)");
}

fun test5(x: int) {
    __expect_type([], "[]");
    __expect_type([x], "[int]");
    __expect_type([x, x >= 1], "[int, bool]");
    __expect_type([x, x >= 1, null as slice], "[int, bool, slice]");
    __expect_type((x, [x], [[x], x]), "(int, [int], [[int], int])");
    __expect_type(getMyOriginalBalanceWithExtraCurrencies(), "[int, cell]");
}

fun test6() {
    var t = createEmptyTuple();
    __expect_type(t, "tuple");
    t.tuplePush(1);
    __expect_type(t, "tuple");
}

fun test7() {
    __expect_type(test3(), "void");
    __expect_type(test3, "() -> void");
    var cb = test1;
    __expect_type(cb, "(int, int) -> void");
    var t = createEmptyTuple();
    __expect_type(beginCell().endCell, "(builder) -> cell");
    // __expect_type(eq<(int, slice)>, "(int, slice) -> (int, slice)");
}


fun main() {
    return 0;
}

/**
@testcase | 0 | | 0
*/
