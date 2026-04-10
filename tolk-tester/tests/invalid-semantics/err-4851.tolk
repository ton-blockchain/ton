fun int.increment(self) {
    self = self + 1;
}

/**
@compilation_should_fail
@stderr modifying `self`, which is immutable by default
@stderr probably, you want to declare `mutate self`
 */
