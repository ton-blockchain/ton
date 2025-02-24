fun getNullableInt(): int? { return 5; }

fun testDoWhileCondition() {
    var (x: int?, y: int?) = (10, 20);
    do {
        x = getNullableInt();
        y = getNullableInt();
    } while(x == null);
    return x * y; // x is 100% int, but y is not
}

/**
@compilation_should_fail
@stderr can not apply operator `*` to `int` and `int?`
 */
