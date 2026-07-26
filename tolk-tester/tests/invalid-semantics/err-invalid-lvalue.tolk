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

// @stderr can not assign to a temporary expression
fun errTensorLiteralIndexedAssign() {
    var (a, b) = (0, 0);
    (a, b).0 = 10;
    return (a, b);
}

// @stderr `_` can't be used as a value; it's a placeholder for a left side of assignment
// @stderr takeInt(_)
fun errUnderscoreRValue() {
    return takeInt(_);
}

fun mutateArg<T>(mutate arg: T) {}
fun T.mutateSelf(mutate self) {}

// @stderr can not mutate a temporary expression
// @stderr mutateArg(mutate (a, b))
fun errMutateTensorLiteral() {
    var a = 1;
    var b = 2;
    mutateArg(mutate (a, b));
}

// @stderr mutateArg(mutate [a, b])
fun errMutateSquareLiteral() {
    var a = 1;
    var b = 2;
    mutateArg(mutate [a, b]);
}

// @stderr (a, b).mutateSelf()
fun errTensorLiteralMutatingMethod() {
    var a = 1;
    var b = 2;
    (a, b).mutateSelf();
}

// @stderr [a, b].mutateSelf()
fun errSquareLiteralMutatingMethod() {
    var a = 1;
    var b = 2;
    [a, b].mutateSelf();
}

// @stderr mutateArg(mutate (u, other))
fun errSmartCastUnionTensorLiteralMutate() {
    var u: (int, int) | (int, int, int) = (1, 2);
    var other = 0;
    if (u is (int, int)) {
        mutateArg(mutate (u, other));
    }
}
