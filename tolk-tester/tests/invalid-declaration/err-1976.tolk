struct S<T> {
    x: T = C.x
}

const C = S<int> {}

fun main() {
    return C.x
}

/**
@compilation_should_fail
@stderr const `C` appears, directly or indirectly, in its own initializer
 */
