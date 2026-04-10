fun main(flags: int) {
    return flags << 1 + 32;
}

/**
@compilation_should_fail
@stderr << has lower precedence than +
*/
