fun incNotChained(mutate self: int) {
    self = self + 1;
}

fun failWhenReturnANotChainedValue(x: int): int {
    return x.incNotChained();
}

/**
@compilation_should_fail
@stderr x.incNotChained()
@stderr can not convert type `void` to return type `int`
 */
