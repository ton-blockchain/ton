fun f(x: int): int {
    return match(x) {
        -1 => -1,
        f(0) => 0,
        else => 1,
    };
}

/**
@compilation_should_fail
@stderr not a constant expression
@stderr f(0) => 0
 */
