fun increment(x: int, self: int): int {
    return x + self;
}

/**
@compilation_should_fail
@stderr `self` can only be the first parameter
 */
