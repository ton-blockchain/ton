
fun increment(mutate x: int = 0) {
    x += 1;
}

/**
@compilation_should_fail
@stderr `mutate` parameter can't have a default value
 */
