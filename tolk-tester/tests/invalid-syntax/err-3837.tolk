fun f(x: int) {
    match(x) {
        -1 => -1,
        0 => 0,
        else => 0,
        else => -1
    };
}

/**
@compilation_should_fail
@stderr duplicated `else` branch
@stderr else => -1
 */
