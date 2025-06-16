type MInt = int;
type MIntN = int?;
type MBuilder = builder;
type CallbackIntToInt = int -> int;

@pure
fun beginParse(c: cell): slice
    asm "CTOS";

@pure
fun endCell(b: builder): cell
    asm "ENDC";

fun getBeginCell() {
    return beginCell
}

fun getBeginParse() {
    return beginParse
}

@method_id(101)
fun testVarApply1() {
    var (_, f_end_cell) = (0, endCell);
    var b: MBuilder = (getBeginCell())().storeInt(1, 32);
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
        val func: (MInt, int) -> MInt = query_id % 2 == 0 ? sum : mul;
        val result = func(a, b);
        return result;
    }
    return -1;
}

@method_id(103)
fun testVarApplyInTernary() {
    var t: tuple = createEmptyTuple();
    try {
        t.push(demo_handler(0xF2, 122, 100, 200));
    } catch(code) {
        t.push(code);
    }
    try {
        t.push(demo_handler(0xF4, 122, 100, 200))
    } catch(code) {
        t.push(code)
    }
    try {
        t.push(demo_handler(0xF2, 122, 10, 10))
    } catch(code) {
        t.push(code)
    }
    try {
        t.push(demo_handler(0xF2, 123, 10, 10))
    } catch(code) {
        t.push(code)
    }
    return t;
}

fun always_throw2(x: int) {
    throw 239 + x
}

global global_f: MInt -> void;

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
    t.push(1);
    var sizer = t.size;
    return sizer(t);
}

fun getTupleLastGetter<X>(): (tuple) -> X {
    return X.last<X>;
}

@method_id(106)
fun testVarApply3() {
    var t = createEmptyTuple();
    t.push(1);
    t.push([2]);
    var getIntAt = t.get<int>;
    var getTupleFirstInt = createEmptyTuple().first<MInt>;
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

fun getNullable4(): int? { return 4 }
fun myBeginCell(): builder? asm "NEWC";

@method_id(108)
fun testCallingNotNull() {
    var n4: () -> MIntN = getNullable4;
    var creator: (() -> builder?)? = myBeginCell;
    var end2: [int, (MBuilder -> cell)?] = [0, endCell];
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

fun check_assoc_2(a: int, b: MInt, c: MIntN): bool {
    return op(op(a, b), c!) == op(a, op(b, c!));
}

@method_id(112)
fun testApplyGlobalVar(x: int, y: int, z: int): bool? {
  op = `_+_`;
  if (0) { return null; }
  return check_assoc_2(x, y, z);
}

fun justAdd2(x: MInt): int { return x + 2; }

@method_id(113)
fun testCallbackAlias(secondNull: bool) {
    var adder1: CallbackIntToInt = justAdd2;
    var adder2: CallbackIntToInt? = secondNull ? null : justAdd2;
    return (adder1(10), adder2 == null ? -1 : adder2(10));
}

fun justGetTensor2() { return (2, 3) }

@method_id(114)
fun testCallbackUnion(call1st: bool): (int, int) | int {
    var cb: (int -> int) | (() -> (MInt, int)) = call1st ? justGetTensor2 : justAdd2;
    if (cb is () -> (int, int)) {
        return cb()
    }
    return cb(10) as int
}

struct WithCallbacks {
    f1: (int -> int)?,
    beginCell: builder -> cell,
}

fun WithCallbacks.doEndCellActually(self, b: builder) { return self.beginCell(b); }

@method_id(115)
fun testCallableFields() {
    var w: WithCallbacks = { f1: null, beginCell: endCell };
    if (10 > 3) {
        w.f1 = justAdd2;
    }
    return (
        w.f1!(5),
        w.doEndCellActually(beginCell().storeInt(6, 32)).beginParse().loadInt(32),
        beginParse(w.beginCell(beginCell().storeInt(7, 32))).loadInt(32)
    );
}

struct Wrapper<T> {
    value: T;
}

fun wrap<T>(value: T): Wrapper<T> {
    return{value};
}

@method_id(116)
fun testCallableGenericField() {
    var w1: Wrapper<(int) -> int> = { value: justAdd2 };
    var w2: Wrapper<CallbackIntToInt?> = { value: justAdd2 };
    var w_f = wrap<int->MInt>;
    return (w1.value(5), w2.value!(9), wrap(w_f(justAdd2)).value.value(78));
}

struct Point {
    x: int;
    y: int;
}

fun Point.createXY(x: int, y: int): Point { return {x,y}; }
fun Point.createFrom<U,V>(x: U, y: V): Point { return {x,y}; }

fun Point.maxCoord(self) { return self.x > self.y ? self.x : self.y; }

@method_id(117)
fun testStaticAndInstanceMethods() {
    var fPointCreateXY = Point.createXY;
    var p1 = fPointCreateXY(5,6);
    var fPointCreateFrom = Point.createFrom<int8,uint16>;
    var p2 = fPointCreateFrom(7,8);
    __expect_type(p1, "Point");
    __expect_type(p2, "Point");
    var getM1 = Point.maxCoord;
    var getM2 = p1.maxCoord;
    return (getM1(p1), (getM2 = p2.maxCoord)(p2));
}

fun Wrapper<T?>.createFromNull(): Wrapper<T?> {
    return {value: null};
}
fun Wrapper<T>.createFrom<U>(value: U): Wrapper<T> {
    return {value: value as T};
}
fun Wrapper<T>.equalsNotNull<U>(self, anotherW: Wrapper<U>) {
    return self.value != null && anotherW.value != null && self.value == anotherW.value;
}

@method_id(118)
fun testMethodsOfGenericStruct() {
    var creator1 = Wrapper<int8>.createFrom<int>;
    var w1 = creator1(10);
    __expect_type(w1, "Wrapper<int8>");
    var creator2 = Wrapper<coins?>.createFrom<int?>;
    var w2 = creator2(null as int16?);
    __expect_type(w2, "Wrapper<coins?>");
    var eq = Wrapper<int8>.equalsNotNull<coins?>;
    __expect_type(eq, "(Wrapper<int8>, Wrapper<coins?>) -> bool");
    var creator3 = Wrapper<int8>.createFromNull;
    __expect_type(creator3, "() -> Wrapper<int8?>");
    return (eq(w1, w2), w2.value = w1.value as int, eq(w1, w2), creator3());
}

fun add3WithDefaults(a: int, b: int = 0, c: int = 0) {
    return a + b + c;
}

@method_id(119)
fun testSavingFunWithDefaults() {
    var cc = add3WithDefaults;
    __expect_type(cc, "(int, int, int) -> int");
    return cc(1, 2, 3);
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
@testcase | 110 | 5  | 5 10 100 100 100 134
@testcase | 110 | 0  | (null) (null) (null) (null) (null) 0
@testcase | 111 | 2 3 9      | -1
@testcase | 111 | 11 22 44   | -1
@testcase | 111 | -1 -10 -20 | -1
@testcase | 112 | 2 3 9      | -1
@testcase | 112 | 11 22 44   | -1
@testcase | 112 | -1 -10 -20 | -1
@testcase | 113 | 0          | 12 12
@testcase | 113 | -1         | 12 -1
@testcase | 114 | -1         | 2 3 131
@testcase | 114 | 0          | (null) 12 1
@testcase | 115 |            | 7 6 7
@testcase | 116 |            | 7 11 80
@testcase | 117 |            | 6 8
@testcase | 118 |            | 0 10 -1 (null)
@testcase | 119 |            | 6
 */
