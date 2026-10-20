fun g(): int? {
    return null
}

fun main() {
    return g() ?? true
}

/**
@compilation_should_fail
@stderr type of operator `??` is `int | bool`; probably, it's not what you expected
@stderr note: this operand is `int?`
@stderr note: this operand is `bool`
 */
