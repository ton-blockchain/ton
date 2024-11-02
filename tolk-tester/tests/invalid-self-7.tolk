fun increment(x: int): int {
    return self + 1;
}

/**
@compilation_should_fail
@stderr using `self` in a non-member function
 */
