const asdf = 1;

fun main(x: int) {
    return x.asdf();
}

/**
@compilation_should_fail
@stderr calling a non-function
 */
