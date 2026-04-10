fun getOne() { return 1; }

fun main() {
    return getOne<int>();
}

/**
@compilation_should_fail
@stderr type arguments not expected here
 */
