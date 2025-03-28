fun increment(mutate self: int): self {
    self = self + 1;
    return self;
}

fun cantReturnAnotherSelf(mutate self: int): self {
    self = self + 1;
    var x = 0;
    return x.increment();
}

/**
@compilation_should_fail
@stderr invalid return from `self` function
 */
