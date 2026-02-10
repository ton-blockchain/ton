// @compilation_should_fail

const cTensor = (1, 2)
const cInt1 = 0x123
const cInt2 = 0x123
const op_increase = 0x123

fun increment(mutate x: int) {
    x += 1;
}

fun takeInt(x: int) {}

fun eq<T>(v: T) { return v }


// @stderr modifying immutable constant `cTensor`
fun errModifyConstantField() {
    increment(mutate cTensor.0);
}

// @stderr modifying immutable constant `cInt1`
// @stderr modifying immutable constant `cInt2`
// @stderr modifying immutable constant `op_increase`
fun errMutateConstant() {
    var x = 10;
    (x, cInt1) = (20, 30);
    op_increase = 500;
    return increment(mutate cInt2);
}

// @stderr modifying immutable variable `myVal`
// @stderr hint: it's declared `val myVal`, keyword 'val' means 'immutable'
// @stderr hint: declare `var myVal` to allow modifications
fun errMutateVal() {
    val myVal = 10;
    increment(mutate myVal);
}

// @stderr modifying immutable variable `x`
// @stderr modifying immutable variable `xxx`
fun errAssignToValInArray() {
    val x = 10;
    var y = 20;
    [y, x] = [30, 40];

    val xxx = 10;
    takeInt(xxx += 1);
}


// @stderr modifying immutable variable `sl`
// @stderr sl.loadUint(33)
// @stderr_avoid sl.loadRef
fun errCallMutatingMethodOnVal(c: cell) {
    val sl: slice = c.beginParse();
    if (1) {
        var sl: slice = c.beginParse();
        _ = sl.loadRef();       // this is ok, 'sl' is another variable
    }
    val i = sl.loadUint(33);
}

// @stderr modifying immutable variable `valTens`
fun errMutateFieldOfImmutableTensor() {
    val valTens = (0, 1);
    increment(mutate ((valTens!)).0);
}


struct Point {
    x: int
    y: int
}

struct ROLine {
    readonly p1: Point
    readonly p2: Point
}

struct ROPoint {
    readonly x: int
    readonly y: int
}

// @stderr modifying readonly field `ROLine.p2`
// @stderr_avoid ok in f
fun errModifyReadonlyFieldInROLine(l: ROLine) {
    l.p1; l.p2; l.p1.x; eq(l.p2.y); eq(eq(l).p2).y; // ok in f

    l.p2!.x += 1;
}

struct ImmSlice {
    readonly s: slice
}

// @stderr modifying readonly field `ImmSlice.s`
fun ImmSlice.read(mutate self) {
    self.s.loadRef()
}

// @stderr modifying readonly field `ROPoint.x`
fun errModifyReadonlyFieldInSubExpr(p1: ROPoint, p2: ROPoint) {
    increment(mutate (p1 = p2).x);
}

// @stderr modifying immutable variable `ks3`
fun errCantMutateFieldOfImmutableTuple() {
    val ks3 = null as [int, [int, [int]]]?;
    eq(ks3!.1.1 = [10]).0 = 10;
}
