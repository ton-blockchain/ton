// @compilation_should_fail

fun int.assign10(mutate self) {
    self = 10;
}

fun takeInt(x: int) {
}

// @stderr assignment can not be used as lvalue
fun errCallMutatingFunctionWithAssignmentLValue() {
    var t: tuple = createEmptyTuple();
    (t = createEmptyTuple()).push(1);
}

// @stderr operator `!` can not be used as lvalue
fun errAsOperatorAsLvalue() {
    var (i, j) = (0, 0);
    (i as int)!.assign10();
    return (i, j);
}

// @stderr function call can not be used as lvalue
fun errFunctionCallAsLvalue() {
    tuplePush(mutate createEmptyTuple(), 1);
}

@pure
fun tuplePush<T>(mutate t: tuple, value: T): void
    asm "TPUSH";


fun incBoth(mutate x: int, mutate y: int) {
    x += 10;
    y += 10;
}

// @stderr literal can not be used as lvalue
// @stderr mutate 30
fun errLiteralAsLvalue() {
    var x = 10;
    incBoth(mutate x,
            mutate 30);
}

// @stderr `_` can't be used as a value; it's a placeholder for a left side of assignment
// @stderr takeInt(_)
fun errUnderscoreRValue() {
    return takeInt(_);
}
