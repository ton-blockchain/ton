fun main(x: int): int {
    try {
        if (x > 10) { throw(44); }
    } catch(code) {}
    return code;
}

/**
@compilation_should_fail
@stderr return code;
@stderr undefined symbol `code`
 */
