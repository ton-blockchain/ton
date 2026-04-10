fun T.pairWith<U>(self, value: U) {
    return [self, value];
}

fun main() {
    10.pairWith();
}

/**
@compilation_should_fail
@stderr too few arguments in call to `T.pairWith<U>`, expected 1, have 0
 */
