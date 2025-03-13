fun main(am: slice) {
    return ton(am);
}

/**
@compilation_should_fail
@stderr function `ton` requires a constant string, like `ton("0.05")`
 */
