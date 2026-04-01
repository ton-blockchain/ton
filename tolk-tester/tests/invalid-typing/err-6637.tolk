fun getTensor() {
    return (1, 2)
}

fun inc(mutate x: int) {
    x += 1;
}

fun main() {
    inc(mutate getTensor().0);
}

/**
@compilation_should_fail
@stderr can not mutate a temporary expression
 */
