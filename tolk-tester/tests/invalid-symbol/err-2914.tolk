const asdf = 1;

fun main(x: int) {
    return x.asdf();
}

/**
@compilation_should_fail
@stderr non-existing method `asdf` of type `int`
 */
