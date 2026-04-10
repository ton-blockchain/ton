
fun main(r: int | slice) {
    // after `is` we expect a type, parse `int?`, and expression becomes incorrect
    return r is int ? 10 : 20;
}

/**
@compilation_should_fail
@stderr expected `;`, got `10`
 */
