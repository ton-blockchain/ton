type MInt = int;
type Tensor2Int = (int, int);
type Tensor2IntN = (int, int)?;

fun eq1<X>(value: X): X { return value; }
fun eq2<X>(value: X)    { return value; }
fun eq3<X>(value: X): X { var cp: [X] = [eq1(value)]; var ((([v: X]))) = cp; return v; }
fun eq4<X>(value: X)    { return eq1<X>(value); }

@pure
fun tuplePush<T>(mutate t: tuple, value: T): void
    asm "TPUSH";

@method_id(101)
fun test101(x: int) {
    var (a, b, c) = (x, (x,x), [x,x]);
    return (eq1(a), eq1(b), eq1(c), eq2(a), eq2(b), eq2(c), eq3(a), eq4(b), eq3(createEmptyTuple()));
}

fun getTwo<X>(): X { return 2 as X; }

fun takeInt(a: int) { return a; }

struct Wrapper<T> {
    value: T;
}
struct Err<T> {
    errPayload: T
}

@method_id(102)
fun test102(): (int, int, int, [int, int], Wrapper< Wrapper<int8> >) {
    var a: int = getTwo();
    var _: int = getTwo();
    var b = getTwo() as int;
    var c: int = 1 ? getTwo() : getTwo();
    var c redef = getTwo();
    var d: int = match (1 as int|slice|builder) { int => getTwo(), slice => getTwo(), builder => getTwo() };
    var ab_tens = (0, (1, 2));
    ab_tens.0 = getTwo();
    ab_tens.1.1 = getTwo();
    var ab_tup = [0, [1, 2]];
    ab_tup.0 = getTwo();
    ab_tup.1.1 = getTwo();
    var ab_tens_al: Tensor2Int = (getTwo(), getTwo());
    var cint: Wrapper<int> = { value: getTwo() };
    var cuni: Wrapper<int> | Err<int> = Wrapper { value: getTwo() };
    cuni = Err { errPayload: getTwo() };
    return (eq1<int>(a), eq2<int>(b), takeInt(getTwo()), [getTwo(), ab_tens.1.1], eq1({ value: { value: getTwo() } } as Wrapper< Wrapper<int8> >));
}

@method_id(103)
fun test103(first: int): (int, int, int) {
    var t = createEmptyTuple();
    var cs = beginCell().storeInt(100, 32).endCell().beginParse();
    t.push(first);
    t.push(2);
    t.push(cs);
    cs = t.get(2);
    cs = t.get(2) as slice;
    return (t.get(0), cs.loadInt(32), t.get<slice>(2).loadInt(32));
}

fun manyEq<T1, T2, T3>(a: T1, b: T2, c: T3): [T1, T2, T3] {
    return [a, b, c];
}

@method_id(104)
fun test104(f: int) {
    var result = (
        manyEq(1 ? 1 : 1, f ? 0 : null, !f ? getTwo() as int : null),
        manyEq(f ? null as int? : eq2(2), beginCell().storeBool(true).endCell().beginParse().loadBool(), eq4(f))
    );
    __expect_type(result, "([int, int?, int?], [int?, bool, int])");
    return result;
}

fun calcSum<X>(x: X, y: X) { return x! + y!; }

@method_id(105)
fun test105() {
    if (0) { calcSum(((0 as int?)), null); }
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
    __expect_type(calcYPlus1<int>, "(int) -> int");
    return [
        abstractTransform(cellToSlice, calcLoad32, c),
        abstractTransform(calcYPlus1<int>, calcYPlus1<int>, 0),
        abstractTransform(calcTensorPlus1, calcTensorMul2, (2, 2)).0,
        abstractTransform(calcTensorPlus1, calcTensorMul2, (2, 2)).1
    ];
}

fun X.callTupleFirst<Y>(self): Y { return self.first(); }
fun callTupleFirst<X, Y>(t: X): Y { return t.first(); }
fun T.callTuplePush<V>(mutate self, v1: V, v2: V): self { self.push(v1); tuplePush(mutate self, v2); return self; }
fun getTupleLastInt(t: tuple) { return t.last<int>(); }
fun getTupleSize(t: MTuple) { return t.size(); }
fun callAnyFn<TObj, TResult>(f: (TObj) -> TResult, arg: TObj) { return f(arg); }
fun callAnyFn2<TCallback>(f: TCallback, arg: tuple) { return f(arg); }

global t107: MTuple;
type MTuple = tuple;

@method_id(107)
fun test107() {
    t107 = createEmptyTuple();
    t107.callTuplePush(1, 2);
    t107.callTuplePush(3, 4).callTuplePush(5, 6);
    var first: int = t107.callTupleFirst();
    return (
        callAnyFn<tuple, int>(getTupleSize, t107),
        callAnyFn2(getTupleSize, t107),
        first,
        callTupleFirst(t107) as int,
        callAnyFn(getTupleLastInt, t107 as tuple),
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

@method_id(109)
fun test109(initialX: int32) {
    var x: MInt = initialX;
    __expect_type(eq1(x), "MInt");
    __expect_type(eq1<int>(x), "int");
    __expect_type(eq1<int8?>(x), "int8?");
    return (eq1<uint16?>(x), eq1<uint16?>(null), eq1<MInt?>(null));
}

@method_id(110)
fun test110() {
    var ab_tens_al: Tensor2Int = (getTwo(), getTwo());
    ab_tens_al.0 = getTwo();
    var ab_tens_al_n: Tensor2Int? = (5, 6);
    ab_tens_al_n.1 = getTwo();
    return (ab_tens_al, ab_tens_al_n);
}

type Tup2Int = [int, int];

fun isSomeNullableNull<T>(v: T?) { return v == null; }

type MIntN = int?;
type Tup2IntN = Tup2Int?;

@method_id(111)
fun test111() {
    var i1: MIntN = 3;
    var i2: Tup2IntN = [1, 2];
    return (isSomeNullableNull(i1), isSomeNullableNull(i2), isSomeNullableNull<int8>(i1), isSomeNullableNull<int?>(i1));
}

@method_id(112)
fun test112(v: int | slice?) {
    return (eq1(v), eq4(v == null));
}

fun makeNullable<T1, T2>(a: T1 | T2): T1 | T2 | null {
    return a;
}

@method_id(113)
fun test113(a: int | slice) {
    var b: int | (int, int) = match (a) {
        int => a,
        slice => (a.loadInt(32), -1)
    };
    __expect_type(makeNullable(b), "int | (int, int) | null");
    return (
        makeNullable(a), -100, makeNullable(b), -100, makeNullable<int, null>(9), -100, makeNullable<slice, null>(null), -100,
        makeNullable<int, (int, int) | builder>(b), -100, makeNullable<int, (int, int, int)?>(null)
    );
}


fun main(x: int): (int, [Tup2Int]) {
    __expect_type(test110, "() -> (Tensor2Int, Tensor2Int)");

    try { if(x) { throw (1, x); } }
    catch (excNo, arg) { return (arg as int, [[eq2(arg as int), getTwo()]]); }
    return (0, [[x, 1]]);
}

/**
@testcase | 0   | 1 | 1 [ [ 1 2 ] ]
@testcase | 101 | 0 | 0 0 0 [ 0 0 ] 0 0 0 [ 0 0 ] 0 0 0 []
@testcase | 102 |   | 2 2 2 [ 2 2 ] 2
@testcase | 103 | 0 | 0 100 100
@testcase | 104 | 0 | [ 1 (null) 2 ] [ 2 -1 0 ]
@testcase | 105 |   | 3
@testcase | 106 |   | [ 106 2 6 6 ]
@testcase | 107 |   | 6 6 1 1 6 6
@testcase | 108 |   | 45
@testcase | 109 | 5 | 5 (null) (null)
@testcase | 110 |   | 2 2 5 2
@testcase | 111 |   | 0 0 0 0
@testcase | 112 | 5 1 | 5 1 0
@testcase | 112 | 0 0 | 0 0 -1
@testcase | 113 | 5 1 | 5 1 -100 (null) 5 1 -100 9 -100 (null) -100 (null) 5 1 -100 (null) (null) (null) 0

@fif_codegen DECLPROC eq1<int>
@fif_codegen DECLPROC eq1<tuple>
@fif_codegen DECLPROC eq1<(int,int)>
@fif_codegen DECLPROC eq1<[int,int]>
@fif_codegen DECLPROC eq1<MInt?>
@fif_codegen DECLPROC eq1<int|slice|null>
@fif_codegen DECLPROC eq1<Wrapper<Wrapper<int8>>>
@fif_codegen DECLPROC getTwo<int>

@fif_codegen_avoid DECLPROC eq1
@fif_codegen_avoid DECLPROC eq2
@fif_codegen_avoid DECLPROC eq3
 */
