
fun testCantUseNullableAsCondition(x: int?) {
    if (x) { return 1; }
    return 0;
}

/**
@compilation_should_fail
@stderr can not use `int?` as a boolean condition
 */
