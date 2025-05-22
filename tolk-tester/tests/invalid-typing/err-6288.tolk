fun int.cantReturnFromSelf(mutate self): self {
    return 2;
}

/**
@compilation_should_fail
@stderr invalid return from `self` function
 */
