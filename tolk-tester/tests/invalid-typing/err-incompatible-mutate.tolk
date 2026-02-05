// @compilation_should_fail
// various tests when `f(mutate arg)` is incompatible

fun incrementOrSetNull(mutate x: int?) {
    if (random.uint256()) { x! += 1; }
    else { x = null; }
}

// @stderr can not pass `int` to mutate `int?`
// @stderr because mutation is not type compatible
fun cantCallMutateMethodNotNullable() {
    var x = 1;
    incrementOrSetNull(mutate x);
    return x;
}

fun (int, int)?.acceptMutateNullableTensor(mutate self) {
}

// @stderr can not call method for mutate `(int, int)?` with object of type `null`
fun cantModifyTupleIndexWithTypeTransition() {
    var t: [int, null] = [1, null];
    t.1.acceptMutateNullableTensor();
}

fun wipe(mutate s: slice?) {
    s = null;
}

// @stderr can not pass `slice | int | null` to mutate `slice?`, because mutation is not type compatible
fun smartCastedUnionStackLayout(v: slice | int | null) {
    if (v is slice || v == null) {
        __expect_type(v, "slice?");
        // this is not supported, because of a different stack layout (no UTag after smart cast)
        wipe(mutate v);
    }
}
