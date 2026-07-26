fun main() {
    return grams(123);
}

/**
@compilation_should_fail
@stderr function `grams` requires a constant string, like `grams("0.05")`
 */
