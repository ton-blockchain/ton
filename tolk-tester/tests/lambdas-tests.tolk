fun callMath1020(fn: (int, int) -> int) {
    return fn(10, 20)
}

fun callMathXY(x: int, y: int, fn: (int, int) -> int) {
    return fn(x, y)
}

fun invokeGeneric2<T1, T2, R>(x: T1, y: T2, cb: (T1, T2) -> R): R {
    return cb(x, y)
}

fun cbCopy<TArg, TRet>(cb: (TArg) -> TRet): TArg -> TRet {
    return cb;
}

fun assign<T>(mutate v: T, newV: T) {
    v = newV
}

fun assignLambda(mutate v: (int, int) -> int) {
    v = fun(x, y) { return x + y }
}

struct WithCb {
    cb: (int, int) -> int
}

fun WithCb.invoke(self, x: int, y: int) {
    return self.cb(x, y)
}

fun WithCb.replaceCb<T>(mutate self, newCb: (T, T) -> T) {
    self.cb = newCb
}

@noinline
fun assertThrows(cb: () -> void, excno: int) {
    try {
        cb();
        throw 123;
    } catch (ex) {
        assert (ex == excno) throw 124;
        return true;
    }
}

fun genericFnWithLambda<T>(arg: T, expected: int) {
    var cb1 = fun() { return 10 };
    var cb2 = fun(argT: int) { return fun(argT: int) { return argT + 0 }(argT) };
    var cb3 = fun(): int { throw 123 };
    return cb1() == expected || cb2(arg as int) == expected || cb3() == expected;
}

@method_id(101)
fun test1() {
    return (
        callMath1020(min),
        callMath1020(fun(x, y) { return x + y }),
        callMath1020(fun(x: int, y) { return x / y }),
        callMath1020(fun(x: int, y: int): int { return abs(x) + abs(y); }),
    )
}

@method_id(102)
fun test2(x: int, y: int) {
    return (
        callMathXY(x, y, min),
        callMathXY(x, y, fun(x, y) { __expect_type(x, "int"); return x + y }),
        callMathXY(x, y, fun(x: int, y) { return x /~ y }),
        callMathXY(x, y, fun(x: int, y: int): int { return abs(x) + abs(y); }),
    )
}

global g103: int;

@method_id(103)
fun test3() {
    var s = stringHexToSlice("FF");
    var cbBuilder = fun(b1: builder, b2: builder) {
        return b1.bitsCount() + b2.bitsCount();
    };
    g103 = 0;
    return (
        invokeGeneric2(5, 10, fun(a, b) { g103 += 1; return a + b + g103 }),
        invokeGeneric2(s, 10, fun(s, num) { __expect_type(s, "slice"); return s.loadUint(8) * num }),
        s.remainingBitsCount(),
        invokeGeneric2(beginCell(), beginCell().storeUint(0, 9), cbBuilder),
        g103,
    )
}

@method_id(104)
fun test4() {
    return fun(x: int, y: int): int {
        return x + y
    }(10, 20);
}

@method_id(105)
fun test5() {
    var i30 = fun(x: int) {
        return fun(x: int, y: int) {
            return x + y
        }(x, 20)
    }(10);
    var cb2 = fun() {
        return fun(x: int, y: int) {
            return x + y
        }
    };
    __expect_type(i30, "int");
    __expect_type(cb2, "() -> (int, int) -> int");
    return (i30, cb2()(10, 20));
}

@method_id(106)
fun test6() {
    var cbIntToCell = cbCopy<int8, cell>(fun(i) {
        return i.toCell()
    });
    return (
        int8.fromCell(cbIntToCell(5)),
        int16.fromCell(cbCopy<int16, cell>(fun(i) { return i.toCell() })(10)),
        int32.fromCell((fun() { return (123 as int32).toCell() })()),
        int64.fromCell((fun(v: int) { return (v as int64).toCell() })(500)),
    )
}

@method_id(107)
fun `test with throws`() {
    return assertThrows(fun() { throw 88 }, 88);
}

@method_id(108)
fun test8() {
    var r1 = genericFnWithLambda(17 as int32, 10);
    var r2 = genericFnWithLambda(true, -1);
    try {
        genericFnWithLambda(500, 0);
        return null;
    } catch (ex) {
        return (r1, r2, ex);
    }
}

@method_id(109)
fun test9() {
    g103 = 0;
    var t = createEmptyTuple();
    t.push(fun() { return fun(x: int) { return x + 3 } }());
    t.push(fun() {});
    t.1 = fun() { g103 -= 500; };

    var t2 = [1, fun() {}];
    __expect_type(t2, "[int, () -> void]");
    t2.1 = fun() { g103 -= 500; };

    return (t.get<() -> void>(1)(), t2.1(), g103, t.get<(int) -> int>(0)(t2.0 += 4));
}

@method_id(110)
fun test10() {
    var t = (0, 0, 0);
    var l = min;
    var w: WithCb = { cb: min };
    t.0 = l(10, 20) + w.cb(10, 20);
    assign(mutate l, fun(x, y) { return x * y });
    w.replaceCb<int>(fun(x, y) { __expect_type(y, "int"); return x * y });
    t.1 = l(10, 20) + w.invoke(10, 20);
    assignLambda(mutate l);
    assignLambda(mutate w.cb);
    t.2 = l(10, 20) + w.invoke(10, 20);
    return t;
}

@method_id(111)
fun test11(v: bool) {
    var nullable_cb = v ? null : fun(x: int) { return x * 2 };
    var union_int: int | (int -> int) = v ? v as int : fun(x: int) { return x * 2 };
    var union_cn: (()->int) | (int->int) = v ? fun() { return 500 } : fun(x : int) { return x * 2 };
    
    return (
        nullable_cb != null ? nullable_cb(10) : null,
        (union_int is int) ? union_int : union_int(10),
        (union_int is int -> int) ? union_int(10) : union_int,
        (union_cn is () -> int) ? union_cn() : union_cn(10),
    )
}

fun main() {
    __expect_type(fun() {}, "() -> void");
    __expect_type(fun(x: int) { x }, "(int) -> void");
    __expect_type(fun(x: int) { return x }, "(int) -> int");
    __expect_type(fun(x: int) { return x }(10), "int");
}

/**
@testcase | 101 |        | 10 30 0 30
@testcase | 102 | 10 -20 | -20 -10 0 30
@testcase | 103 |        | 16 2550 8 9 1
@testcase | 104 |        | 30 
@testcase | 105 |        | 30 30 
@testcase | 106 |        | 5 10 123 500 
@testcase | 107 |        | -1 
@testcase | 108 |        | -1 -1 123 typeid-1 
@testcase | 109 |        | -1000 8 
@testcase | 110 |        | 20 400 60 
@testcase | 111 | -1     | (null) -1 -1 500
@testcase | 111 | 0      | 20 20 20 20
*/