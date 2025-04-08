fun f(x: bool) {
    return match (x) { true => 0, true => -1 };
}

/**
@compilation_should_fail
@stderr `match` expression should have `else` branch
 */
