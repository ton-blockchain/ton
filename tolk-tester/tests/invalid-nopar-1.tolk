fun eq(x: int): int {
    return x;
}

fun main(x: int): int {
    return eq x;
}

/**
@compilation_should_fail
@stderr expected `;`, got `x`
 */
