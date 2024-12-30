fun incNotChained(mutate self: int) {
    self = self + 1;
}

fun cantCallNotChainedMethodsInAChain(x: int) {
    return x.incNotChained().incNotChained();
}

/**
@compilation_should_fail
@stderr can not call method for `int` with object of type `void`
 */
