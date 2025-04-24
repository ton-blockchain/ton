fun inc(x: int) {
    return x + 1;
}

fun main() {
    return inc(_);
}

/**
@compilation_should_fail
@stderr `_` can't be used as a value; it's a placeholder for a left side of assignment
@stderr inc(_)
 */
