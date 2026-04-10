fun int.cantReturnNothingFromSelf(mutate self): self {
    self = self + 1;
}

/**
@compilation_should_fail
@stderr missing return
@stderr }
 */
