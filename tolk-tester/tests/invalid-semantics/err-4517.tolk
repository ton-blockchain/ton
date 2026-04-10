global g: int;

fun f(x: int): int {
    match(x) {
        -1 => -1,
        g => 0,
    };
    return 10;
}

/**
@compilation_should_fail
@stderr unknown type name `g`
 */
