struct S {
    x: int = C.x
}

const C: S = {};

fun main(): int {
    return 0;
}

/**
@compilation_should_fail
@stderr const `C` appears, directly or indirectly, in its own initializer
 */
