const ZERO = 0;

fun sumABDef0(a: int, b: int = 0) {
    return a + b;
}

fun sumADef1BDef2(a: int = ((1)), b: int = 2+ZERO) {
    return a + b;
}

fun getAOrB<T>(a: T, b: T, getA: bool = false): T {
    return getA ? a : b;
}

fun pushToTuple<T>(mutate t: tuple, v: T? = null) {
    t.push(v);
}

struct Point {
    x: int;
    y: int;
}

fun Point.create(x: int = 0, y: int = 0): Point {
    return {x,y};
}

fun Point.incrementX(mutate self, deltaX: int = 1) {
    self.x += deltaX;
}

fun makeCost(cost: coins = ton("0.05")) {
    return cost + ton("0.05");
}

fun makeUnion(v: int | slice = 0) {
    return v;
}

struct MyOptions {
    negate: bool = false,
    mulBy: int = 0,
}

global t105: tuple;

fun log105(a: int, options: MyOptions = {}) {
    if (options.negate) {
        a = -a;
    }
    if (options.mulBy) {
        a *= options.mulBy;
    }
    t105.push(a);
}

struct AnotherOptions {
    c1: int;
    leaveSign: bool = true;
}

fun helper106(a: int, options: AnotherOptions = { c1: 1 }) {
    a *= options.c1;
    return options.leaveSign ? +a : -a;
}


@method_id(101)
fun test1(a: int) {
    return (
        sumABDef0(a),
        sumABDef0(a, 5),
        sumABDef0(100),
        sumADef1BDef2(a),
        sumADef1BDef2(),
        sumADef1BDef2(a, 100),
        sumADef1BDef2(200, 100),
    )
}

@method_id(102)
fun test2(a: int) {
    var t = createEmptyTuple();
    pushToTuple(mutate t, getAOrB(sumADef1BDef2(a), sumADef1BDef2(a, a)));
    pushToTuple(mutate t, true);
    pushToTuple<int>(mutate t, null);
    pushToTuple<slice?>(mutate t, null);
    pushToTuple(mutate t, null);
    return t;
}

@method_id(103)
fun test3() {
    var p = Point.create();
    p.incrementX();
    var p2 = Point.create(8);
    p2.incrementX(p.x += 1);
    return (p, p2);
}

@method_id(104)
fun test4() {
    return (
        makeCost(),
        makeCost(ton("0.1")),
        makeUnion() is int,
        makeUnion(),
        makeUnion(makeCost()),
    )
}

@method_id(105)
fun test5() {
    t105 = createEmptyTuple();
    log105(1);
    log105(1, { negate: true });
    log105(1, { mulBy: 100 });
    log105(1, { mulBy: 100, negate: true });
    log105(1, {});
    return t105;
}

@method_id(106)
fun test6(l4: bool) {
    return (
        helper106(5),
        helper106(5, {leaveSign: false, c1: 1}),
        helper106(5, {c1: 10}),
        helper106(5, {leaveSign: l4, c1: 10}),
        helper106(5, {c1: 0}),
    );
}

fun int.plus(self, v: int = 1) {
    return self + v;
}

@method_id(107)
fun test7() {
    return (10.plus(5.plus()), int.plus(4));
}

fun createTFrom<U = int, V = int100>(v: (int, (U, V)) = (1, (2, 3))) {
    return [v.0, v.1.0, v.1.1];
}

@method_id(108)
fun test8() {
    __expect_type(createTFrom<coins,int8>, "((int, (coins, int8))) -> [int, coins, int8]");
    return (createTFrom(), createTFrom((5, (8, createTFrom().0))));
}

fun main() {}

/**
@testcase | 101 | 10      | 10 15 100 12 3 110 300
@testcase | 102 | 10      | [ 20 -1 (null) (null) (null) ]
@testcase | 103 |         | 2 0 10 0
@testcase | 104 |         | 100000000 150000000 -1 0 1 100000000 1
@testcase | 105 |         | [ 1 -1 100 -100 1 ]
@testcase | 106 | 0       | 5 -5 50 -50 0
@testcase | 107 |         | 16 5
@testcase | 108 |         | [ 1 2 3 ] [ 5 8 1 ]
 */
