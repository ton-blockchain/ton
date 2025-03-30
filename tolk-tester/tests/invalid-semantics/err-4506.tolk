fun increment(mutate x: int) {
    x = x + 1;
}

fun cantCallMutatingAsAMember() {
    var x = 0;
    x.increment();
    return x;
}

/**
@compilation_should_fail
@stderr function `increment` mutates parameter `x`
@stderr consider calling `increment(mutate x)`, not `x.increment`()
@stderr alternatively, rename parameter to `self` to make it a method
 */
