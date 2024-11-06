fun inc(x: int) {
    return x + 1;
}

fun main() {
    return inc(_);
}

/**
@compilation_should_fail
@stderr rvalue expected
@stderr inc(_)
 */
