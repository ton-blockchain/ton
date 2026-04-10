fun getNullableInt(): int? { return 5; }

fun getTensor(x: int?): (int?, int) { return (x, 0); }

fun testSmartCastsDropAfterAssign() {
    var x: int? = 0;
    var y: int? = 0;
    (getTensor(x = getNullableInt()).0, getTensor(y = getNullableInt()).0) = (x + y, x - y);
    return x+y;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int?`
@stderr x + y, x - y
 */
