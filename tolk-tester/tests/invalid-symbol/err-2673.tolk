fun cantReturnSelf(mutate x: int): int {
    x += 1;
    return self;
}

/**
@compilation_should_fail
@stderr using `self` in a non-member function (it does not accept the first `self` parameter)
@stderr return self
 */
