
fun getNullable4(): int? {
    return 4;
}

fun testCantSumNullable() {
    return 1 + getNullable4();
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int` and `int?`
 */
