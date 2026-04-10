fun int.incNotChained(mutate self) {
    self = self + 1;
}

fun cantCallNotChainedMethodsInAChain(x: int) {
    return x.incNotChained().incNotChained();
}

/**
@compilation_should_fail
@stderr method `incNotChained` not found for type `void`
 */
