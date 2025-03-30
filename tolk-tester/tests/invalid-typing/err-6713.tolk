fun assignNull2<T1, T2>(mutate x: T1?, mutate y: T2?) {
    if (false) {
        x = null;
        y = null;
    }
}

fun testSmartCastsDropAfterNullableGeneric() {
    var (x: int?, y: int?) = (1, 2);
    x * y;   // ok
    assignNull2(x, y);  // treated like assignments to nullable
    x << y;  // error
}

/**
@compilation_should_fail
@stderr can not apply operator `<<` to `int?` and `int?`
 */
