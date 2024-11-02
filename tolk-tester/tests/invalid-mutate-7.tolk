fun incBoth(mutate x: int, mutate y: int) {
    x += 10;
    y += 10;
}

fun cantCallMutatingFunctionWithRvalue() {
    var x = 10;
    incBoth(mutate x, mutate 30);
}

/**
@compilation_should_fail
@stderr lvalue expected (call a mutating function)
@stderr incBoth(mutate x, mutate 30)
 */
