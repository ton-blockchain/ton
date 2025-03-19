const op_increase = 0x123;

fun inc(mutate x: int): int {
    x += 10;
    return x + 1;
}

fun cantCallMutatingFunctionWithImmutable() {
    return inc(mutate op_increase);
}

/**
@compilation_should_fail
@stderr modifying immutable constant
@stderr inc(mutate op_increase)
 */
