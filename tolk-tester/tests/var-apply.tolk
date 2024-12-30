fun getBeginCell() {
    return beginCell;
}

fun getBeginParse() {
    return beginParse;
}

@method_id(101)
fun testVarApply1() {
    var (_, f_end_cell) = (0, endCell);
    var b: builder = (getBeginCell())().storeInt(1, 32);
    b.storeInt(2, 32);
    var s = (getBeginParse())(f_end_cell(b));
    return (s.loadInt(32), s.loadInt(32));
}

@inline
fun my_throw_always() {
    throw 1000;
}

@inline
fun get_raiser() {
    return my_throw_always;
}

@method_id(102)
fun testVarApplyWithoutSavingResult() {
    try {
        var raiser = get_raiser();
        raiser();   // `some_var()` is always impure, the compiler has no considerations about its runtime value
        return 0;
    } catch (code) {
        return code;
    }
}

@inline
fun sum(a: int, b: int) {
    assert(a + b < 24, 1000);
    return a + b;
}

@inline
fun mul(a: int, b: int) {
    assert(a * b < 24, 1001);
    return a * b;
}

fun demo_handler(op: int, query_id: int, a: int, b: int): int {
    if (op == 0xF2) {
        val func = query_id % 2 == 0 ? sum : mul;
        val result = func(a, b);
        return 0;  // result not used, we test that func is nevertheless called
    }
    if (op == 0xF4) {
        val func = query_id % 2 == 0 ? sum : mul;
        val result = func(a, b);
        return result;
    }
    return -1;
}

@method_id(103)
fun testVarApplyInTernary() {
    var t: tuple = createEmptyTuple();
    try {
        t.tuplePush(demo_handler(0xF2, 122, 100, 200));
    } catch(code) {
        t.tuplePush(code);
    }
    try {
        t.tuplePush(demo_handler(0xF4, 122, 100, 200));
    } catch(code) {
        t.tuplePush(code);
    }
    try {
        t.tuplePush(demo_handler(0xF2, 122, 10, 10));
    } catch(code) {
        t.tuplePush(code);
    }
    try {
        t.tuplePush(demo_handler(0xF2, 123, 10, 10));
    } catch(code) {
        t.tuplePush(code);
    }
    return t;
}

fun always_throw2(x: int) {
    throw 239 + x;
}

global global_f: int -> void;

@method_id(104)
fun testGlobalVarApply() {
    try {
        global_f = always_throw2;
        global_f(1);
        return 0;
    } catch (code) {
        return code;
    }
}

@method_id(105)
fun testVarApply2() {
    var creator = createEmptyTuple;
    var t = creator();
    t.tuplePush(1);
    var sizer = t.tupleSize;
    return sizer(t);
}

fun getTupleLastGetter<X>(): (tuple) -> X {
    return tupleLast<X>;
}

@method_id(106)
fun testVarApply3() {
    var t = createEmptyTuple();
    t.tuplePush(1);
    t.tuplePush([2]);
    var getIntAt = t.tupleAt<int>;
    var getTupleFirstInt = createEmptyTuple().tupleFirst<int>;
    var getTupleLastTuple = getTupleLastGetter<tuple>();
    return (getIntAt(t, 0), getTupleFirstInt(t), getTupleLastTuple(t), getTupleLastGetter<tuple>()(t));
}

fun main() {}

/**
@testcase | 101 |    | 1 2
@testcase | 102 |    | 1000
@testcase | 103 |    | [ 1000 1000 0 1001 ]
@testcase | 104 |    | 240
@testcase | 105 |    | 1
@testcase | 106 |    | 1 1 [ 2 ] [ 2 ]
 */
