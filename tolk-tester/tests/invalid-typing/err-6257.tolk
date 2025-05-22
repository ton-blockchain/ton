fun int.increment(mutate self): self {
    self = self + 1;
    return self;
}

fun int.cantReturnAnotherSelf(mutate self): self {
    self = self + 1;
    var x = 0;
    return x.increment();
}

/**
@compilation_should_fail
@stderr invalid return from `self` function
 */
