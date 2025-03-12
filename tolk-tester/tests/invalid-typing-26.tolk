fun getNullableInt(): int? { return 5; }

fun testAssertThrowIsConditional() {
    var (x, y) = (getNullableInt(), getNullableInt());
    assert(x != null) throw(y = 10);
    return x + y;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int` and `int?`
 */
