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
@stderr literal can not be used as lvalue
@stderr incBoth(mutate x, mutate 30)
 */
