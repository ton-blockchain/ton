fun getNullableInt(): int? { return 5; }

fun testTryBodyDontSmartCast() {
    var x = getNullableInt();
    try {
        x = 5;
    } catch {}
    return x * 10; // x is not int here; for now, we have no exception edges, assuming it can be anywhere inside try
}

/**
@compilation_should_fail
@stderr can not apply operator `*` to `int?` and `int`
 */
