// @compilation_should_fail
// various tests for type checker errors (not all! there are lots of independent err-xxxx)

struct S1 { v: int; }

// @stderr can not assign `array<unknown>` to field of type `int`
fun errAssignTupleToField() {
    var s: S1 = { v: 0 };
    s.v = createEmptyTuple();
}

// @stderr can not assign `int` to a tensor
// @stderr can not apply operator `+` to `undefined` and `undefined`
fun errAssignIntToTensor() {
    var (x, y) = 2;
    x + y;
}

// @stderr can not assign `int` to `slice`
// @stderr const errConstIntToSlice
const errConstIntToSlice: slice = 123 + 456;

// @stderr can not assign `[int, int, int]` to `[int, int]`
fun errAssignArraySizeMismatch() {
    var [x, y] = [2, 3, 4];
    x + y;
}

struct S2 { f1: void; }

// @stderr can not assign `int` to field of type `void`
// @stderr a.f1 = 100
fun errAssignIntToVoidField() {
    var a: S2 = {};     // f1 is void, can be omitted
    a.f1 = 100;
}

// @stderr can not assign `(int, slice)`, sizes mismatch
fun errAssignTensorSizeMismatch(k: (int, slice)) {
    var (a, b, c: int?) = k;
}

fun getNullableInt(): int? { return 5; }

// @stderr can not apply operator `<` to `null` and `null`
fun testFlowContextAppliedInBinaryOperator() {
    var x: int? = getNullableInt();
    var y: int? = getNullableInt();
    if ((y = null) < y) {
        return -100;
    }
    return 0;
}

fun tupleLast<T>(t: tuple): T
    asm "LAST";

fun getTupleLastGetter<X>(): tuple -> X {
    return tupleLast<X>;
}

// @stderr can not assign `int` to variable of type `cell`
fun failTypeMismatch() {
    var t = createEmptyTuple();
    var c: cell = getTupleLastGetter<int>()(t);
}

// @stderr can not apply operator `*` to `int?` and `int`
fun tryBodyDontSmartCast() {
    var x = getNullableInt();
    try {
        x = 5;
    } catch {}
    // x is not `int` here; for now, we have no exception edges, assuming it can be anywhere inside `try`
    return x * 10;
}

// @stderr can not assign `int?` to variable of type `slice?`
fun cantAssignIncompatiblePrimitiveNullable(m: int?) {
    var a: slice? = m;
}

fun setFirstToSlice_A117(mutate t: (int|slice, int)) {
    t.0 = beginCell().endCell().beginParse();
}

// @stderr can not apply operator `+` to `int | slice` and `int`
fun checkSmartCastInvalidatedAfterMutate(): int {
    var t: (int|slice, int) = (1, 0);
    if (t.0 is int) {
        setFirstToSlice_A117(mutate t);
        return t.0 + 1;
    }
    return 0;
}

// @stderr method `crc32` requires a constant string, like "some_str".crc32()
fun constEvalMethodNotAsDot() {
    string.crc32(123);
}
