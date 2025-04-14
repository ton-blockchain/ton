fun f(x: int) {
    return match(x) {
        -1 => -1,
        else => 0,
        0 => 0
    };
}

/**
@compilation_should_fail
@stderr `else` branch should be the last
 */
