fun increment(self: int) {
    self = self + 1;
}

/**
@compilation_should_fail
@stderr modifying `self` (left side of assignment), which is immutable by default
@stderr probably, you want to declare `mutate self`
 */
