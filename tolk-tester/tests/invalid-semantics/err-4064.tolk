
@pure
fun f_pure(): int {
    return f_impure();
}

fun f_impure(): int { return 0; }

fun main(): int {
    return f_pure();
}

/**
@compilation_should_fail
@stderr an impure operation in a pure function
@stderr in function `f_pure`
*/
