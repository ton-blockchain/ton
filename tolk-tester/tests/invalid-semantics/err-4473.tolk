fun slice.load32(self): int {
    return self.loadUint(32);
}

/**
@compilation_should_fail
@stderr modifying `self`, which is immutable by default
 */
