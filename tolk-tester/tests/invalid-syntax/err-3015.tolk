fun main() {
    return ton(123);
}

/**
@compilation_should_fail
@stderr function `ton` requires a constant string, like `ton("0.05")`
 */
