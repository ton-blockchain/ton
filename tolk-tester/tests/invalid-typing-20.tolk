fun getNullableInt(): int? { return 5; }

fun testFlowContextAppliedInBinaryOperator() {
    var x: int? = getNullableInt();
    var y: int? = getNullableInt();
    if ((y = null) < y) {
        return -100;
    }
    return 0;
}

/**
@compilation_should_fail
@stderr can not apply operator `<` to `null` and `null`
 */
