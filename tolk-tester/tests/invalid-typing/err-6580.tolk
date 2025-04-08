type MIntN = int?;

fun getNullableInt(): MIntN { return 5; }

fun testNeverTypeOccurs() {
    var x: int? = getNullableInt();
    if (x == null && x != null) {
        return x + 0;
    }
    return 0;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `never` and `int`
 */
