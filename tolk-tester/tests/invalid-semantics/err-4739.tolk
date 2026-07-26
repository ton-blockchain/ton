fun T.pairWith<U>(self, value: U) {
    return [self, value];
}

fun main() {
    10.pairWith();
}

/**
@compilation_should_fail
@stderr can not deduce U for generic function `T.pairWith<U>`
 */
