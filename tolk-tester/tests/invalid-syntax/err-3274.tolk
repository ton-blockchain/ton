
fun main(x: int): int {
    if (x, 1) {
        return 10;
    }
    return 0;
}

/**
@compilation_should_fail
@stderr expected `)`, got `,`
 */
