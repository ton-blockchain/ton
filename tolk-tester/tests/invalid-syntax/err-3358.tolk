fun main() {
    return grams("0.1234567891");
}

/**
@compilation_should_fail
@stderr too many digits after a dot, nanograms are 10^9
 */
