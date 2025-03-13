
fun main(x: int): int {
    if x {
        return 10;
    }
    return 0;
}

/**
@compilation_should_fail
@stderr expected `(`, got `x`
 */
