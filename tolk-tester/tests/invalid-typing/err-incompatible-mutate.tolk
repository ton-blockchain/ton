// @compilation_should_fail
// various tests when `f(mutate arg)` is incompatible

fun incrementOrSetNull(mutate x: int?) {
    if (random.uint256() != 0) { x! += 1; }
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

fun overwriteWithGeneralInt(mutate x: int) {
    x = 300;
}

// @stderr can not pass `int | int8` to mutate `int`, because mutation is not type compatible
fun cantMutateSmartCastedUnionVariantAsAnotherVariant(flag: int) {
    var u: int | int8 = flag > 10 ? 1 : (1 as int8);
    if (flag != 0 && u is int8) {
        overwriteWithGeneralInt(mutate u);
    }
}

fun int.overwriteWithGeneralIntSelf(mutate self) {
    self = 300;
}

// @stderr can not call method for mutate `int` with object of type `int | int8`, because mutation is not type compatible
fun cantMutateSmartCastedUnionReceiverAsAnotherVariant(flag: int) {
    var u: int | int8 = flag > 10 ? 1 : (1 as int8);
    if (flag != 0 && u is int8) {
        u.overwriteWithGeneralIntSelf();
    }
}

fun firstMutateSecondNot(x77: int, mutate y77: int) {
}

// @stderr incorrect `mutate`, since `firstMutateSecondNot` does not mutate parameter `x77`
// @stderr function `firstMutateSecondNot` mutates parameter `y77`
// @stderr you need to specify `mutate` when passing an argument, like `mutate b77`
// @stderr function `slice.loadUint` mutates parameter `slice`
// @stderr incorrect `mutate`, since `slice.loadUint` does not mutate parameter `int`
fun invalidMutateArgumentPassed(a77: int, b77: int, p77: slice) {
    firstMutateSecondNot(mutate a77, b77);
    slice.loadUint(p77, mutate b77);
}
