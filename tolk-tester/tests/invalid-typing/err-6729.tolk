fun fail(): never {
    throw 1;
}

fun main(): int {
    val [unused: int, [a: int, b: int]] = [1, fail()];
    return 0;
}

/**
@compilation_should_fail
@stderr can not assign `never` to `[...]`
 */
