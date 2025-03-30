fun getOne() { return 1; }

fun main() {
    return getOne<int>();
}

/**
@compilation_should_fail
@stderr calling a not generic function with generic T
 */
