// @compilation_should_fail

fun takeInt(x: int) {
}

// @stderr can not mutate a temporary expression
fun errCallMutatingFunctionWithAssignmentLValue() {
    var t: tuple = createEmptyTuple();
    (t = createEmptyTuple()).push(1);
}

// @stderr can not assign to a temporary expression
fun errAsOperatorAsLvalue() {
    var (i, j) = (0, 0);
    (i as int)! = 10;
    return (i, j);
}

// @stderr `_` can't be used as a value; it's a placeholder for a left side of assignment
// @stderr takeInt(_)
fun errUnderscoreRValue() {
    return takeInt(_);
}
