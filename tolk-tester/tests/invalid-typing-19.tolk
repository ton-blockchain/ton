fun getNullableInt(): int? { return 5; }

fun testCantApplyNotNullForAlwaysNull() {
    var x: int? = getNullableInt();
    if (x != null) { return 0; }
    return x! + 1;
}

/**
@compilation_should_fail
@stderr operator `!` used for always null expression
 */
