fun f(x: int) {
    return match(x) {
        -1 => -1,
        0 => 0
    };
}

/**
@compilation_should_fail
@stderr `match` expression should have `else` branch
 */
