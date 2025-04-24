fun cantReturnNothingFromSelf(mutate self: int): self {
    self = self + 1;
}

/**
@compilation_should_fail
@stderr missing return
@stderr }
 */
