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

@method_id(107)
fun testIndexedAccessApply() {
    var functions1 = (beginCell, endCell);
    var functions2 = [beginParse];
    var b = functions1.0().storeInt(1, 16);
    b.storeInt(1, 16);
    return functions2.0(functions1.1(b)).loadInt(32);
}

fun getNullable4(): int? { return 4; }
fun myBeginCell(): builder? asm "NEWC";

@method_id(108)
fun testCallingNotNull() {
    var n4: () -> int? = getNullable4;
    var creator: (() -> builder?)? = myBeginCell;
    var end2: [int, (builder -> cell)?] = [0, endCell];
    var c: cell = end2.1!((creator!()!)!.storeInt(getNullable4()!, 32));
    return c.beginParse().loadInt(32);
}

fun sumOfTensorIfNotNull(t: (int, int)?) {
    if (t == null) { return 0; }
    return t!.0 + t!.1;
}

@method_id(109)
fun testTypeTransitionOfVarCall() {
    var summer = sumOfTensorIfNotNull;
    var hh1 = [1, null];
    var tt1 = (3, 4);
    return (summer(null), summer((1,2)), summer(hh1.1), summer(tt1));
}

fun makeTensor(x1: int, x2: int, x3: int, x4: int, x5: int) {
    return (x1, x2, x3, x4, x5);
}

fun eq<T>(x: T): T { return x; }

@method_id(110)
fun testVarsModificationInsideVarCall(x: int) {
    var cb = makeTensor;
    return x > 3 ? cb(x, x += 5, eq(x *= x), x, eq(x)) : null;
}

fun check_assoc_1(op: (int, int) -> int, a: int, b: int, c: int) {
    return op(op(a, b), c) == op(a, op(b, c));
}

@method_id(111)
fun testApplyNativePlus(x: int, y: int, z: int): bool {
    return check_assoc_1(`_+_`, x, y, z);
}

global op: (int, int) -> int;

fun check_assoc_2(a: int, b: int, c: int): bool {
    return op(op(a, b), c) == op(a, op(b, c));
}

@method_id(112)
fun testApplyGlobalVar(x: int, y: int, z: int): bool? {
  op = `_+_`;
  if (0) { return null; }
  return check_assoc_2(x, y, z);
}

fun main() {}

/**
@testcase | 101 |    | 1 2
@testcase | 102 |    | 1000
@testcase | 103 |    | [ 1000 1000 0 1001 ]
@testcase | 104 |    | 240
@testcase | 105 |    | 1
@testcase | 106 |    | 1 1 [ 2 ] [ 2 ]
@testcase | 107 |    | 65537
@testcase | 108 |    | 4
@testcase | 109 |    | 0 3 0 7
@testcase | 110 | 5  | 5 10 100 100 100 -1
@testcase | 110 | 0  | (null) (null) (null) (null) (null) 0
@testcase | 111 | 2 3 9      | -1
@testcase | 111 | 11 22 44   | -1
@testcase | 111 | -1 -10 -20 | -1
@testcase | 112 | 2 3 9      | -1
@testcase | 112 | 11 22 44   | -1
@testcase | 112 | -1 -10 -20 | -1
*/
