fun nothing() {
}

fun main() {
    val x = 0;
    return x.nothing();
}

/**
@compilation_should_fail
@stderr `nothing` has no parameters and can not be called as method
 */
