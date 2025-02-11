fun getNullableInt(): int? { return 5; }

fun testReassignInRedef() {
    var t1: int? = getNullableInt();
    if (t1 != null) {
        var (t1 redef, t2) = (getNullableInt(), 5);
        return t1 + t2;
    }
    return -1;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int`
 */
