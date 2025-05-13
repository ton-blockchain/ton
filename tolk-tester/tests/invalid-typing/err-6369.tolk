fun f<T>(x: T, y: T) {}

fun failIncompatibleTypesForT() {
    return f(32, "");
}

/**
@compilation_should_fail
@stderr can not pass `slice` to `int`
@stderr f(32
 */
