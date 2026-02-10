struct Empty {
}

fun main() {
    return Empty [1,2];
}

/**
@compilation_should_fail
@stderr `Empty` is not a valid constructor for `[...]`
 */
