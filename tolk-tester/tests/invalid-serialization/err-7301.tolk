type Int32Or64 = int32 | int64;

fun f() {
    val o = lazy Int32Or64.fromSlice("");
    match (o) {
        int32 => return o + 1,
        int64 => throw o,
    }
}

/**
@compilation_should_fail
@stderr `lazy` union should contain only structures, but it contains `int32`
 */
