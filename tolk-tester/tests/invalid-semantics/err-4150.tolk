fun increment(mutate x: int) {
    x += 1;
}

fun checkCantMutateFieldOfImmutableTensor() {
    val t = (0, 1);
    increment(mutate ((t!)).0);
}

/**
@compilation_should_fail
@stderr modifying immutable variable `t`
 */
