fun eq1<X>(value: X): X { return value; }
fun eq2<X>(value: X)    { return value; }
fun eq3<X>(value: X): X { var cp: [X] = [eq1(value)]; var ((([v: X]))) = cp; return v; }
fun eq4<X>(value: X)    { return eq1<X>(value); }

@method_id(101)
fun test101(x: int) {
    var (a, b, c) = (x, (x,x), [x,x]);
    return (eq1(a), eq1(b), eq1(c), eq2(a), eq2(b), eq2(c), eq3(a), eq4(b), eq3(createEmptyTuple()));
}

fun getTwo<X>(): X { return 2 as X; }

fun takeInt(a: int) { return a; }

@method_id(102)
fun test102(): (int, int, int, [(int, int)]) {
    var a: int = getTwo();
    var _: int = getTwo();
    var b = getTwo() as int;
    var c: int = 1 ? getTwo() : getTwo();
    var c redef = getTwo();
    return (eq1<int>(a), eq2<int>(b), takeInt(getTwo()), [(getTwo(), getTwo())]);
}

@method_id(103)
fun test103(first: int): (int, int, int) {
    var t = createEmptyTuple();
    var cs = beginCell().storeInt(100, 32).endCell().beginParse();
    t.tuplePush(first);
    t.tuplePush(2);
    t.tuplePush(cs);
    cs = t.tupleAt(2);
    cs = t.tupleAt(2) as slice;
    return (t.tupleAt(0), cs.loadInt(32), t.tupleAt<slice>(2).loadInt(32));
}

fun manyEq<T1, T2, T3>(a: T1, b: T2, c: T3): [T1, T2, T3] {
    return [a, b, c];
}

@method_id(104)
fun test104(f: int) {
    return (
        manyEq(1 ? 1 : 1, f ? 0 : null, !f ? getTwo() as int : null),
        manyEq((f ? null as int : eq2(2), beginCell().storeBool(true).endCell().beginParse().loadBool()), 0, eq4(f))
    );
}

fun calcSum<X>(x: X, y: X) { return x + y; }

@method_id(105)
fun test105() {
    if (0) { calcSum(((0)), null); }
    return (calcSum(1, 2));
}

fun calcYPlus1<Y>(value: Y) { return value + 1; }
fun calcLoad32(cs: slice) { return cs.loadInt(32); }
fun calcTensorPlus1(tens: (int, int)) { var (f, s) = tens; return (f + 1, s + 1); }
fun calcTensorMul2(tens: (int, int)) { var (f, s) = tens; return (f * 2, s * 2); }
fun cellToSlice(c: cell) { return c.beginParse(); }
fun abstractTransform<X, Y, R>(xToY: (X) -> Y, yToR: (((Y))) -> R, initialX: X): R {
    var y = xToY(initialX);
    return yToR(y);
}

@method_id(106)
fun test106() {
    var c = beginCell().storeInt(106, 32).endCell();
    return [
        abstractTransform(cellToSlice, calcLoad32, c),
        abstractTransform(calcYPlus1<int>, calcYPlus1<int>, 0),
        abstractTransform(calcTensorPlus1, calcTensorMul2, (2, 2))
    ];
}

fun callTupleFirst<X, Y>(t: X): Y { return t.tupleFirst(); }
fun callTuplePush<T, V>(mutate self: T, v1: V, v2: V): self { self.tuplePush(v1); tuplePush(mutate self, v2); return self; }
fun getTupleLastInt(t: tuple) { return t.tupleLast<int>(); }
fun getTupleSize(t: tuple) { return t.tupleSize(); }
fun callAnyFn<TObj, TResult>(f: (TObj) -> TResult, arg: TObj) { return f(arg); }
fun callAnyFn2<TCallback>(f: TCallback, arg: tuple) { return f(arg); }

global t107: tuple;

@method_id(107)
fun test107() {
    t107 = createEmptyTuple();
    callTuplePush(mutate t107, 1, 2);
    t107.callTuplePush(3, 4).callTuplePush(5, 6);
    var first: int = t107.callTupleFirst();
    return (
        callAnyFn<tuple, int>(getTupleSize, t107),
        callAnyFn2(getTupleSize, t107),
        first,
        callTupleFirst(t107) as int,
        callAnyFn(getTupleLastInt, t107),
        callAnyFn2(getTupleLastInt, t107)
    );
}

global g108: int;

fun inc108(by: int) { g108 += by; }
fun getInc108() { return inc108; }
fun returnResult<RetT>(f: () -> RetT): RetT { return f(); }
fun applyAndReturn<ArgT, RetT>(f: () -> (ArgT) -> RetT, arg: ArgT): () -> ArgT -> RetT {
    f()(arg);
    return f;
}

@method_id(108)
fun test108() {
    g108 = 0;
    getInc108()(1);
    returnResult<(int) -> void>(getInc108)(2);
    applyAndReturn<int, void>(getInc108, 10)()(10);
    returnResult(getInc108)(2);
    applyAndReturn(getInc108, 10)()(10);
    return g108;
}

fun main(x: int): (int, [[int, int]]) {
    try { if(x) { throw (1, x); } }
    catch (excNo, arg) { return (arg as int, [[eq2(arg as int), getTwo()]]); }
    return (0, [[x, 1]]);
}

/**
@testcase | 0   | 1 | 1 [ [ 1 2 ] ]
@testcase | 101 | 0 | 0 0 0 [ 0 0 ] 0 0 0 [ 0 0 ] 0 0 0 []
@testcase | 102 |   | 2 2 2 [ 2 2 ]
@testcase | 103 | 0 | 0 100 100
@testcase | 104 | 0 | [ 1 (null) 2 ] [ 2 -1 0 0 ]
@testcase | 105 |   | 3
@testcase | 106 |   | [ 106 2 6 6 ]
@testcase | 107 |   | 6 6 1 1 6 6
@testcase | 108 |   | 45

@fif_codegen DECLPROC eq1<int>
@fif_codegen DECLPROC eq1<tuple>
@fif_codegen DECLPROC eq1<(int,int)>
@fif_codegen DECLPROC eq1<[int,int]>
@fif_codegen DECLPROC getTwo<int>

@fif_codegen_avoid DECLPROC eq1
@fif_codegen_avoid DECLPROC eq2
@fif_codegen_avoid DECLPROC eq3
 */
