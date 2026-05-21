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

struct Point {
    x: int
    y: int
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
        callMath1020(fun(x: int, y) { return fun() { return x / y }() }),
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
    t.set(fun() { g103 -= 500; }, 1);

    var t2 = [1, fun() {}] as [int16, () -> void];
    t2.1 = fun() { g103 -= 500; };

    return ((t.get(1) as () -> void)(), t2.1(), g103, (t.get(0) as unknown as (int) -> int)(t2.0 += 4));
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

@method_id(112)
fun test12(useCb: bool) {
    var f1: ((int) -> int) | slice = fun(a) {
        __expect_type(a, "int");
        return a * 2
    };
    __expect_type(f1, "(int) -> int");
    var f2: bool | (WithCb -> int) = !useCb ? useCb : fun(a) {
        __expect_type(a, "WithCb");
        __expect_type(a.cb, "(int, int) -> int");
        return a.cb(5, 10)
    };
    __expect_type(f2, "bool | ((WithCb) -> int)");
    return (
        f1(10),
        (f2 is bool) ? f2 as int : f2({cb: fun(a, b) {
            __expect_type(a, "int");
            return a * b;
        }}),
    )
}

@method_id(113)
fun test13(f1null: bool) {
    var a = [] as array<(int -> int)?>;
    a.push(f1null ? null : fun(a) {
        __expect_type(a, "int");
        return a * 2;
    });
    a.push(null);
    return (a.get(0) ?? a.get(1) ?? fun(a: int) { return a * 10 })(6);
}

// closures (lambdas with capture) are now also supported

@method_id(114)
fun test14() {
    var x = 10;
    var cb = fun() {
        return x + 1
    };
    x = 20;     // this modification is not seen in a lambda, values are captured by value
    return (cb(), x);
}

@method_id(115)
fun test15() {
    var x = 10;
    var cb = fun() {
        x += 5;     // this modification is not seen in outer scope
        return x;
    };
    return (cb(), cb(), x);
}

@method_id(116)
fun test16() {
    var p = Point { x: 3, y: 4 };
    var cb = fun() {
        p.x += 100;
        return p.x + p.y;
    };
    p.y = 99;
    return (cb(), p);
}

@method_id(117)
fun test17() {
    var x: int;
    match ([0].first()) {
        0 => x = 7,
        else => throw 123,
    }
    var outer = fun(delta: int) {
        var y = delta * 2;
        return fun(z: int) {
            __expect_type(delta!, "int");
            return x + y + z
        };
    };
    x = 100;
    return outer(5)(1);
}

@method_id(118)
fun test18() {
    var add3 = makeAdder(3);
    return (add3(10), add3(20), makeAdder()(30));
}

@noinline
fun makeAdder(delta: int = 0): int -> int {
    return fun(value) {
        __expect_type((value, delta), "(int, int)");
        return value + delta
    };
}

fun genericLambdaReuseT<T>(value: T) {
    return fun(another: T): T? {
        val inner = fun(nested: T) {
            return nested;
        };
        return inner(another)
    }(value);
}

@method_id(119)
fun test19() {
    var x = [(1, 2) as (int, int)?].first();
    if (x != null) {
        return (fun() {
            var rr = fun() { return x; };
            __expect_type(rr, "() -> (int, int)");
            return rr();
        })();
    }
    x = null;
    __expect_type(x, "null");
    throw 123;
}

@method_id(120)
fun test20(aa: int) {
    var l103 = 0;
    var w: WithCb = { cb: fun(i1, i2) {
        return (g103 = aa) + (l103 = aa);
    } };
    return (g103, l103, w.invoke(0, 0), g103, l103);
}

@method_id(121)
fun test21() {
    var (x, y) = ("", "");
    var f = fun(x: int, y: int) {
        var z = x * y;
        return fun(x: bool, y: bool) {
            __expect_type(x, "bool");
            return x && y ? z : 0;
        }
    };
    return f(5, 10)(true, true);
}

@method_id(122)
fun test22() {
    var args = (1, 2, 3);
    var unused = fun() {
        args;
    };
    return args;
}

@method_id(123)
fun test23() {
    __expect_type(genericLambdaReuseT(123), "int?");
    __expect_type(genericLambdaReuseT(true), "bool?");
    return (genericLambdaReuseT(123), genericLambdaReuseT(true));
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
@testcase | 112 | -1     | 20 50
@testcase | 112 | 0      | 20 0
@testcase | 113 | -1     | 60
@testcase | 113 | 0      | 12
@testcase | 114 |        | 11 20
@testcase | 115 |        | 15 15 10
@testcase | 116 |        | 107 3 99
@testcase | 117 |        | 18
@testcase | 118 |        | 13 23 30
@testcase | 119 |        | 1 2
@testcase | 120 | 5      | (null) 0 10 5 0
@testcase | 121 |        | 50
@testcase | 122 |        | 1 2 3
@testcase | 123 |        | 123 -1
*/
