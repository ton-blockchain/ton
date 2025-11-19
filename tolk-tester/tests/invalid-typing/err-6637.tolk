
fun autoGetIntOrNull() {
    if (random.uint256()) { return 1; }
    return null;
}

fun testAutoInferredIntOrNull() {
    var b: builder = autoGetIntOrNull() as builder;
}

/**
@compilation_should_fail
@stderr type `int?` can not be cast to `builder`
 */
