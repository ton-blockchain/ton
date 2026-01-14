const c = (1, 2)

fun increment(mutate x: int) {
    x += 1;
}

fun main() {
    increment(mutate c.0);
    return c;
}

/**
@compilation_should_fail
@stderr modifying immutable constant
 */
