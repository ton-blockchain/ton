fun main(am: slice) {
    return grams(am);
}

/**
@compilation_should_fail
@stderr function `grams` requires a constant string, like `grams("0.05")`
 */
