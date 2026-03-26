fun main() {
    return ton("0.1234567891");
}

/**
@compilation_should_fail
@stderr too many digits after a dot, nanotons are 10^9
 */
